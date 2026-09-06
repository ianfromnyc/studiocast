#include "core/audio/virtual_audio_service.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <thread>
#include <utility>

#include "core/audio/audio_backend_resolver.h"
#include "core/audio/audio_consumer_detector.h"
#include "core/audio/audio_device_safety.h"
#include "core/audio/audio_pipeline.h"
#include "core/audio/audio_processor.h"
#include "core/audio/mic_monitor.h"
#include "core/audio/pulse/pactl.h"
#include "core/audio/virtual_mic.h"
#include "core/audio/virtual_speaker.h"
#include "core/config/settings.h"
#include "core/maxine/availability.h"
#include "core/maxine/gpu_selection.h"
#include "core/maxine/paths.h"
#include "core/open_audio/open_audio_audio_processor.h"

// Effect planning is build-time independent from the Pulse audio pipeline.
#include "core/maxine/afx/afx_effect.h"

#if STUDIOCAST_HAVE_PULSE_SIMPLE
#include "core/maxine/afx/afx_audio_processor.h"
#include "core/maxine/afx/afx_stereo_audio_processor.h"
#include "core/maxine/afx_api.h"
#endif

namespace studiocast::audio {

namespace {

constexpr const char *kVirtualMicSourceName = "studiocast_mic";
constexpr const char *kVirtualSpeakersSinkName = "studiocast_speakers";

using AudioEffectsEnginePreference =
    studiocast::audio::effects::AudioEffectsEnginePreference;
using BroadcastAudioEffects = studiocast::audio::effects::BroadcastAudioEffects;
using BroadcastMicrophoneEffects =
    studiocast::audio::effects::BroadcastMicrophoneEffects;
using BroadcastSpeakerEffects =
    studiocast::audio::effects::BroadcastSpeakerEffects;

struct MicrophoneAvailabilityCacheKey {
  int schema_version = 0;
  AudioEffectsEnginePreference engine = AudioEffectsEnginePreference::kAuto;
  BroadcastMicrophoneEffects microphone{};
};

struct SpeakerAvailabilityCacheKey {
  int schema_version = 0;
  AudioEffectsEnginePreference engine = AudioEffectsEnginePreference::kAuto;
  BroadcastSpeakerEffects speaker{};
};

struct MicrophoneSourceStatus {
  std::string selected_source;
  std::string availability = "unknown";
  std::string error;
  std::vector<std::string> warnings;
};

struct CachedMicrophoneSourceStatus {
  std::string configured_source;
  bool resolve_auto = false;
  MicrophoneSourceStatus status;
};

bool operator==(const MicrophoneAvailabilityCacheKey &a,
                const MicrophoneAvailabilityCacheKey &b) {
  return a.schema_version == b.schema_version && a.engine == b.engine &&
         a.microphone == b.microphone;
}

bool operator!=(const MicrophoneAvailabilityCacheKey &a,
                const MicrophoneAvailabilityCacheKey &b) {
  return !(a == b);
}

bool operator==(const SpeakerAvailabilityCacheKey &a,
                const SpeakerAvailabilityCacheKey &b) {
  return a.schema_version == b.schema_version && a.engine == b.engine &&
         a.speaker == b.speaker;
}

bool operator!=(const SpeakerAvailabilityCacheKey &a,
                const SpeakerAvailabilityCacheKey &b) {
  return !(a == b);
}

MicrophoneAvailabilityCacheKey
MakeMicrophoneAvailabilityCacheKey(const BroadcastAudioEffects &fx) {
  auto mic = fx.microphone;
  mic.strength = 0;
  return MicrophoneAvailabilityCacheKey{fx.schema_version, fx.engine,
                                        std::move(mic)};
}

SpeakerAvailabilityCacheKey
MakeSpeakerAvailabilityCacheKey(const BroadcastAudioEffects &fx) {
  auto speaker = fx.speaker;
  speaker.strength = 0;
  return SpeakerAvailabilityCacheKey{fx.schema_version, fx.engine,
                                     std::move(speaker)};
}

BroadcastMicrophoneEffects
MicrophoneRestartRelevantEffects(BroadcastMicrophoneEffects mic) {
  // Strength maps to runtime intensity/aux control; it should not by itself
  // force Pulse stream teardown.
  mic.strength = 0;
  return mic;
}

BroadcastSpeakerEffects
SpeakerRestartRelevantEffects(BroadcastSpeakerEffects speaker) {
  // Open Audio speaker strength maps to an atomic runtime control. Maxine still
  // restarts on strength changes because AFX intensity updates are not proven
  // safe while the audio thread is processing.
  speaker.strength = 0;
  return speaker;
}

bool MicrophonePipelineEffectsRequireRestart(
    const BroadcastAudioEffects &oldFx, const BroadcastAudioEffects &newFx) {
  if (oldFx.schema_version != newFx.schema_version ||
      oldFx.engine != newFx.engine) {
    return true;
  }
  return MicrophoneRestartRelevantEffects(oldFx.microphone) !=
         MicrophoneRestartRelevantEffects(newFx.microphone);
}

bool SpeakerPipelineEffectsRequireRestart(
    const BroadcastSpeakerEffects &oldSpeaker,
    const BroadcastSpeakerEffects &newSpeaker) {
  return SpeakerRestartRelevantEffects(oldSpeaker) !=
         SpeakerRestartRelevantEffects(newSpeaker);
}

std::chrono::milliseconds
StartFailureRetryDelay(const VirtualAudioServiceConfig &cfg) {
  return std::chrono::milliseconds(std::max(250, cfg.start_retry_ms));
}

std::chrono::milliseconds
WorkerDeathRetryDelay(const VirtualAudioServiceConfig &cfg) {
  return std::chrono::milliseconds(std::max(25, cfg.start_retry_ms));
}

void SetOpenAudioRuntimeModel(
    OpenAudioRuntimeStatus *status,
    const studiocast::open_audio::ResolvedOpenAudioModel &model) {
  if (!status)
    return;
  status->selected_model_id = model.model_id;
  status->selected_model_path = model.onnx_path.string();
}

OpenAudioRuntimeStatus MakeActiveOpenAudioRuntimeStatus(
    const studiocast::open_audio::ResolvedOpenAudioModel &model,
    const studiocast::open_audio::OpenAudioAudioProcessor &processor) {
  OpenAudioRuntimeStatus status;
  status.active = true;
  status.active_provider = processor.ActiveProviderForStatus();
  status.using_cpu_fallback = processor.UsingCpuFallbackForStatus();
  status.last_runtime_warning = processor.LastStartupWarningForStatus();
  SetOpenAudioRuntimeModel(&status, model);
  return status;
}

void MarkOpenAudioRuntimeDisabled(OpenAudioRuntimeStatus *status,
                                  const std::string &warning) {
  if (!status)
    return;
  status->active = false;
  status->disabled = true;
  status->active_provider = "disabled";
  status->last_runtime_warning = warning;
}

std::string StripAudioProcessorWarningPrefix(std::string warning) {
  const std::string prefix = "AudioProcessor warning: ";
  if (warning.compare(0, prefix.size(), prefix) == 0)
    warning.erase(0, prefix.size());
  return warning;
}

void ApplyOpenAudioRuntimeWarning(OpenAudioRuntimeStatus *status,
                                  const std::string &pipeline_error) {
  if (!status || pipeline_error.find("Open Audio") == std::string::npos)
    return;

  const std::string warning = StripAudioProcessorWarningPrefix(pipeline_error);
  status->last_runtime_warning = warning;

  if (warning.find("switched to CPU fallback") != std::string::npos) {
    status->active = true;
    status->disabled = false;
    status->using_cpu_fallback = true;
    status->active_provider = "cpu";
  } else if (warning.find("disabled after repeated runtime failures") !=
             std::string::npos) {
    MarkOpenAudioRuntimeDisabled(status, warning);
  } else if (warning.find("initialization failed") != std::string::npos ||
             warning.find("init failed") != std::string::npos ||
             warning.find("temporarily disabled") != std::string::npos) {
    MarkOpenAudioRuntimeDisabled(status, warning);
  }
}

std::optional<std::string>
ChooseSpeakerTargetSinkName(const std::string &configured_target,
                            std::string *error) {
  return ChooseSafeSpeakerTargetSinkName(configured_target, error);
}

bool PulseSourceListContains(const std::vector<pulse::PactlSource> &sources,
                             const std::string &name) {
  return std::any_of(sources.begin(), sources.end(),
                     [&](const auto &source) { return source.name == name; });
}

MicrophoneSourceStatus
ResolveMicrophoneSourceStatus(const VirtualAudioServiceConfig &cfg,
                              bool resolve_auto) {
  MicrophoneSourceStatus out;

  std::string configured = cfg.source_name;
  if (configured == "auto")
    configured.clear();

  if (!configured.empty()) {
    out.selected_source = configured;

    std::string unsafeReason;
    if (IsUnsafeInputSourceName(configured, &unsafeReason)) {
      out.availability = "unavailable";
      out.error = unsafeReason;
      return out;
    }

    std::string listErr;
    const auto sources = pulse::ListSources(&listErr);
    if (!listErr.empty()) {
      out.warnings.push_back(
          "Pulse source list could not confirm configured source "
          "availability: " +
          listErr);
      out.availability = "unknown";
      return out;
    }

    if (!PulseSourceListContains(sources, configured)) {
      out.availability = "unavailable";
      out.error = "Configured Pulse source '" + configured +
                  "' is not currently available. Reconnect it or choose "
                  "another physical microphone/input source.";
      return out;
    }

    out.availability = "available";
    return out;
  }

  if (!resolve_auto)
    return out;

  const auto resolved = ResolveSafeInputSourceName(cfg.source_name);
  out.warnings = resolved.warnings;
  if (!resolved.ok) {
    out.availability = "unavailable";
    out.error = resolved.error.empty()
                    ? "Failed to resolve a safe Pulse microphone source."
                    : resolved.error;
    return out;
  }

  out.selected_source = resolved.source_name;
  out.availability = "available";
  return out;
}

void FillMaxineAvailability(AudioBackendAvailability *out) {
  if (!out)
    return;

  // Maxine availability probe (audio needs AFX).
  if (!studiocast::maxine::BackendBuilt()) {
    out->maxine_ok = false;
    out->maxine_reason = "Maxine support not enabled in this build.";
    return;
  }

  const auto settings = studiocast::config::LoadSettings();
  const auto sel = studiocast::maxine::SelectGpu(settings.gpu);
  if (!sel.selected || !sel.selected->compute_capability) {
    out->maxine_ok = false;
    out->maxine_reason = "Failed to select a supported NVIDIA GPU.";
    if (!sel.error.empty())
      out->maxine_reason += " " + sel.error;
    return;
  }

  const auto paths = studiocast::maxine::ResolveMaxinePaths();
  if (!paths.afx.ok) {
    out->maxine_ok = false;
    out->maxine_reason = "AFX SDK not available";
    if (!paths.afx.problems.empty()) {
      out->maxine_reason += ": ";
      out->maxine_reason += paths.afx.problems.front();
    }
    return;
  }

  out->maxine_ok = true;
  out->maxine_reason.clear();
}

AudioBackendAvailability ProbeAudioBackendAvailabilityForMicrophone(
    const VirtualAudioServiceConfig &cfg) {
  AudioBackendAvailability out;

#if !STUDIOCAST_HAVE_ONNXRUNTIME
  (void)cfg;
#endif

  FillMaxineAvailability(&out);

#if !STUDIOCAST_ENABLE_OPEN_AUDIO
  out.open_source_ok = false;
  out.open_source_reason = "Open Audio backend is disabled in this build.";
#elif STUDIOCAST_HAVE_ONNXRUNTIME
  // Open Audio backend availability probe: validate that a model can be
  // resolved.
  {
    std::string oerr;
    if (studiocast::open_audio::ResolveOpenAudioModelForMicrophone(
            cfg.effects, nullptr, &oerr)) {
      out.open_source_ok = true;
      out.open_source_reason.clear();
    } else {
      out.open_source_ok = false;
      out.open_source_reason =
          oerr.empty() ? "Open Audio backend unavailable." : oerr;
    }
  }
#else
  out.open_source_ok = false;
  out.open_source_reason =
      "Open Audio backend unavailable: ONNX Runtime not found at build time.";
#endif

  return out;
}

AudioBackendAvailability
ProbeAudioBackendAvailabilityForSpeaker(const VirtualAudioServiceConfig &cfg) {
  AudioBackendAvailability out;

#if !STUDIOCAST_HAVE_ONNXRUNTIME
  (void)cfg;
#endif

  FillMaxineAvailability(&out);

#if !STUDIOCAST_ENABLE_OPEN_AUDIO
  out.open_source_ok = false;
  out.open_source_reason = "Open Audio backend is disabled in this build.";
#elif STUDIOCAST_HAVE_ONNXRUNTIME
  {
    std::string oerr;
    if (studiocast::open_audio::ResolveOpenAudioModelForSpeaker(
            cfg.effects, nullptr, &oerr)) {
      out.open_source_ok = true;
      out.open_source_reason.clear();
    } else {
      out.open_source_ok = false;
      out.open_source_reason =
          oerr.empty() ? "Open Audio backend unavailable." : oerr;
    }
  }
#else
  out.open_source_ok = false;
  out.open_source_reason =
      "Open Audio backend unavailable: ONNX Runtime not found at build time.";
#endif

  return out;
}

} // namespace

VirtualAudioService::VirtualAudioService() = default;

VirtualAudioService::VirtualAudioService(VirtualAudioServiceHooks hooks)
    : hooks_(std::move(hooks)) {}

VirtualAudioService::~VirtualAudioService() { Stop(); }

bool VirtualAudioService::Start(const VirtualAudioServiceConfig &cfg,
                                std::string *error) {
  Stop();
  {
    std::lock_guard<std::mutex> lock(mu_);
    cfg_ = cfg;
    st_ = VirtualAudioServiceStatus{};
    st_.service_running = false;
    st_.pipeline_running = false;
    st_.pipeline_starting = false;
    st_.pipeline_active_needed = false;
    st_.pipeline_state.clear();
    st_.pipeline_idle_reason.clear();
    st_.mic_consumer_present = false;
    st_.mic_consumer_count = 0;
    st_.mic_consumer_error.clear();
    st_.speakers_routing_active = false;
    st_.speakers_route_mode.clear();
    st_.speakers_pipeline_running = false;
    st_.speakers_pipeline_starting = false;
    st_.speakers_pipeline_active_needed = false;
    st_.speakers_pipeline_state.clear();
    st_.speakers_pipeline_idle_reason.clear();
    st_.speakers_consumer_present = false;
    st_.speakers_consumer_count = 0;
    st_.speakers_consumer_error.clear();
    st_.open_audio_runtime = {};
    st_.speakers_open_audio_runtime = {};
    st_.pipeline_frames_processed = 0;
    st_.pipeline_process_time_us_sum = 0;
    st_.pipeline_process_time_us_max = 0;
    st_.pipeline_process_time_us_last = 0;
    st_.pipeline_process_overruns = 0;
    st_.speakers_pipeline_frames_processed = 0;
    st_.speakers_pipeline_process_time_us_sum = 0;
    st_.speakers_pipeline_process_time_us_max = 0;
    st_.speakers_pipeline_process_time_us_last = 0;
    st_.speakers_pipeline_process_overruns = 0;
    mic_created_ = false;
    speakers_created_ = false;
    speakers_loopback_running_ = false;
    speakers_loopback_target_.clear();
    speakers_loopback_latency_ms_ = 0;
    monitor_running_ = false;
    monitor_sink_requested_.clear();
    monitor_sink_resolved_.clear();
    monitor_module_id_ = -1;
    monitor_latency_ms_ = 0;
    monitor_volume_ = 0;
    monitor_output_lost_ = false;
    monitor_route_may_exist_ = false;
    consumer_detector_.reset();
  }

  stop_.store(false, std::memory_order_release);
  try {
    th_ = std::thread([this]() { ThreadMain(); });
  } catch (const std::exception &e) {
    if (error)
      *error = std::string("Failed to start VirtualAudioService thread: ") +
               e.what();
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    running_ = true;
    st_.service_running = true;
  }

  return true;
}

void VirtualAudioService::Stop() {
  stop_.store(true, std::memory_order_release);
  if (th_.joinable()) {
    th_.join();
  }
  {
    std::lock_guard<std::mutex> lock(mu_);
    running_ = false;
    st_.service_running = false;
    st_.pipeline_running = false;
    st_.pipeline_starting = false;
    st_.pipeline_active_needed = false;
    st_.pipeline_state.clear();
    st_.pipeline_idle_reason.clear();
    st_.mic_consumer_present = false;
    st_.mic_consumer_count = 0;
    st_.mic_consumer_error.clear();
    st_.speakers_routing_active = false;
    st_.speakers_route_mode.clear();
    st_.speakers_pipeline_running = false;
    st_.speakers_pipeline_starting = false;
    st_.speakers_pipeline_active_needed = false;
    st_.speakers_pipeline_state.clear();
    st_.speakers_pipeline_idle_reason.clear();
    st_.speakers_consumer_present = false;
    st_.speakers_consumer_count = 0;
    st_.speakers_consumer_error.clear();
    st_.open_audio_runtime = {};
    st_.speakers_open_audio_runtime = {};
    st_.pipeline_frames_processed = 0;
    st_.pipeline_process_time_us_sum = 0;
    st_.pipeline_process_time_us_max = 0;
    st_.pipeline_process_time_us_last = 0;
    st_.pipeline_process_overruns = 0;
    st_.pipeline_pulse_capture_latency_us_last = 0;
    st_.pipeline_pulse_playback_latency_us_last = 0;
    st_.pipeline_pulse_latency_us_max = 0;
    st_.pipeline_resync_events = 0;
    st_.speakers_pipeline_frames_processed = 0;
    st_.speakers_pipeline_process_time_us_sum = 0;
    st_.speakers_pipeline_process_time_us_max = 0;
    st_.speakers_pipeline_process_time_us_last = 0;
    st_.speakers_pipeline_process_overruns = 0;
    st_.speakers_pipeline_pulse_capture_latency_us_last = 0;
    st_.speakers_pipeline_pulse_playback_latency_us_last = 0;
    st_.speakers_pipeline_pulse_latency_us_max = 0;
    st_.speakers_pipeline_resync_events = 0;
    st_.mic_app_consumer_count = 0;
    st_.mic_monitor_consumer_count = 0;
    st_.monitor_active = false;
    st_.monitor_module_id = -1;
    st_.monitor_sink_active.clear();
    st_.monitor_latency_ms_active = 0;
    st_.monitor_note.clear();
    st_.monitor_last_error.clear();
    monitor_running_ = false;
    monitor_sink_requested_.clear();
    monitor_sink_resolved_.clear();
    monitor_module_id_ = -1;
    monitor_latency_ms_ = 0;
    monitor_volume_ = 0;
    monitor_output_lost_ = false;
    monitor_route_may_exist_ = false;
    consumer_detector_.reset();
  }
}

void VirtualAudioService::UpdateConfig(const VirtualAudioServiceConfig &cfg) {
  std::lock_guard<std::mutex> lock(mu_);
  cfg_ = cfg;
}

VirtualAudioServiceConfig VirtualAudioService::Config() const {
  std::lock_guard<std::mutex> lock(mu_);
  return cfg_;
}

VirtualAudioServiceStatus VirtualAudioService::Status() const {
  std::lock_guard<std::mutex> lock(mu_);
  return st_;
}

void VirtualAudioService::SleepFor(std::chrono::milliseconds d) const {
  if (hooks_.sleep_for) {
    hooks_.sleep_for(d);
    return;
  }
  std::this_thread::sleep_for(d);
}

std::unique_ptr<AudioPipelineRunner>
VirtualAudioService::CreatePipeline(AudioProcessor *processor) const {
  if (hooks_.create_pipeline) {
    return hooks_.create_pipeline(processor);
  }
#if STUDIOCAST_HAVE_PULSE_SIMPLE
  return std::make_unique<studiocast::audio::AudioPipeline>(processor);
#else
  (void)processor;
  return nullptr;
#endif
}

bool VirtualAudioService::CreateVirtualMicDevice(std::string *error) const {
  if (hooks_.create_virtual_mic) {
    return hooks_.create_virtual_mic(error);
  }
  return studiocast::audio::CreateVirtualMic(error);
}

bool VirtualAudioService::CreateVirtualSpeakerDevice(std::string *error) const {
  if (hooks_.create_virtual_speaker) {
    return hooks_.create_virtual_speaker(error);
  }
  return studiocast::audio::CreateVirtualSpeaker(error);
}

bool VirtualAudioService::StartSpeakerLoopbackRoute(
    const std::string &target_sink_name, int latency_ms,
    std::string *error) const {
  if (hooks_.start_speaker_loopback) {
    return hooks_.start_speaker_loopback(target_sink_name, latency_ms, error);
  }
  return studiocast::audio::StartSpeakerLoopback(target_sink_name, latency_ms,
                                                 error);
}

bool VirtualAudioService::StopSpeakerLoopbackRoute(std::string *error) const {
  if (hooks_.stop_speaker_loopback) {
    return hooks_.stop_speaker_loopback(error);
  }
  return studiocast::audio::StopSpeakerLoopback(error);
}

bool VirtualAudioService::DestroyVirtualSpeakerDevice(
    std::string *error) const {
  if (hooks_.destroy_virtual_speaker) {
    return hooks_.destroy_virtual_speaker(error);
  }
  return studiocast::audio::DestroyVirtualSpeaker(error);
}

bool VirtualAudioService::StartMicMonitorRoute(
    const MicMonitorConfig &cfg, const std::string &mic_source_name,
    MicMonitorState *out, std::string *error) const {
  if (hooks_.start_mic_monitor) {
    return hooks_.start_mic_monitor(cfg, mic_source_name, out, error);
  }
  return studiocast::audio::StartMicMonitor(cfg, mic_source_name, out, error);
}

bool VirtualAudioService::StopMicMonitorRoute(std::string *error) const {
  if (hooks_.stop_mic_monitor) {
    return hooks_.stop_mic_monitor(error);
  }
  return studiocast::audio::StopMicMonitor(error);
}

bool VirtualAudioService::SetMicMonitorRouteVolume(int module_id, int volume,
                                                   std::string *error) const {
  if (hooks_.set_mic_monitor_volume) {
    return hooks_.set_mic_monitor_volume(module_id, volume, error);
  }
  return studiocast::audio::SetMicMonitorVolume(module_id, volume, error);
}

MicMonitorState
VirtualAudioService::DetectMicMonitorRoute(std::string *error) const {
  if (hooks_.detect_mic_monitor) {
    return hooks_.detect_mic_monitor(error);
  }
  return studiocast::audio::DetectMicMonitor(error);
}

std::optional<bool>
VirtualAudioService::MicMonitorSinkPresentRoute(const std::string &sink_name,
                                                std::string *error) const {
  if (hooks_.mic_monitor_sink_present) {
    return hooks_.mic_monitor_sink_present(sink_name, error);
  }
  return studiocast::audio::MicMonitorSinkPresent(sink_name, error);
}

AudioBackendAvailability
VirtualAudioService::ProbeMicrophoneBackendAvailability(
    const VirtualAudioServiceConfig &cfg) const {
  if (hooks_.probe_microphone_backend_availability) {
    return hooks_.probe_microphone_backend_availability(cfg);
  }
  return ProbeAudioBackendAvailabilityForMicrophone(cfg);
}

AudioBackendAvailability VirtualAudioService::ProbeSpeakerBackendAvailability(
    const VirtualAudioServiceConfig &cfg) const {
  if (hooks_.probe_speaker_backend_availability) {
    return hooks_.probe_speaker_backend_availability(cfg);
  }
  return ProbeAudioBackendAvailabilityForSpeaker(cfg);
}

AudioConsumerSnapshot VirtualAudioService::DetectMicrophoneConsumers() const {
  if (hooks_.detect_microphone_consumers) {
    return hooks_.detect_microphone_consumers();
  }
  if (!consumer_detector_) {
    consumer_detector_ = CreateDefaultAudioConsumerDetector();
  }
  return consumer_detector_->DetectSourceConsumersByName(kVirtualMicSourceName);
}

AudioConsumerSnapshot VirtualAudioService::DetectSpeakerConsumers() const {
  if (hooks_.detect_speaker_consumers) {
    return hooks_.detect_speaker_consumers();
  }
  if (!consumer_detector_) {
    consumer_detector_ = CreateDefaultAudioConsumerDetector();
  }
  return consumer_detector_->DetectSinkConsumersByName(
      kVirtualSpeakersSinkName);
}

void VirtualAudioService::SetLastError(std::string msg) {
  std::lock_guard<std::mutex> lock(mu_);
  st_.last_error = std::move(msg);
}

void VirtualAudioService::ThreadMain() {
  using namespace std::chrono;

  steady_clock::time_point nextStartRetry{};
  steady_clock::time_point nextSpeakerStartRetry{};
  steady_clock::time_point nextSpeakerLoopbackStartRetry{};
  steady_clock::time_point nextSpeakerLoopbackStopRetry{};
  steady_clock::time_point nextSpeakerDestroyRetry{};
  steady_clock::time_point nextMonitorStartRetry{};
  // Consecutive failed monitor starts. Each one spaces the next retry further
  // out, so a setting that can never work stops running pactl every quarter
  // second for the life of the daemon.
  int monitorStartFailures = 0;
  // The settings the last start attempt used, whether it worked or not. A
  // change to them is a fresh try, so the wait starts over.
  std::string monitorAttemptSink;
  int monitorAttemptLatencyMs = 0;
  steady_clock::time_point nextMonitorStopRetry{};
  steady_clock::time_point nextMonitorVolumeRetry{};
  // The stop and the volume space their retries out the same way the start
  // does. Each one runs several pactl processes, so a failure that never ends
  // must not run them four times a second for the life of the daemon.
  int monitorStopFailures = 0;
  int monitorVolumeFailures = 0;
  // The cleanup that runs while the output is lost keeps its own counter and
  // deadline: the monitor is still wanted there, and the wait of the stop the
  // user asks for starts over on every pass of a wanted monitor.
  //
  // It also gives up at the top of its backoff, which the other loops never
  // do. Only the user ends a lost output, so a cleanup that never gave up
  // would block the audio supervisor for one stop deadline per backoff step
  // for the rest of the run. A stop that failed this many times with a growing
  // wait will not work on the next try for a reason the wait can fix.
  constexpr int kMonitorLostCleanupMaxTries = 8;
  steady_clock::time_point nextMonitorLostCleanup{};
  int monitorLostCleanupFailures = 0;
  // True while the reported error belongs to a failed volume step. A later
  // step that works clears that error, and only that one.
  bool monitorVolumeFailed = false;
  steady_clock::time_point nextMonitorVerify{};
  std::string speakerLoopbackStopRetryError;
  std::string speakerDestroyRetryError;

  // If the Open Audio backend fails to initialize, latch-disable it for a short
  // cooldown to avoid rapid restart loops.
  steady_clock::time_point openAudioCooldownUntil{};
  std::string openAudioCooldownReason;

  // If microphone Maxine setup fails after availability selection, keep the
  // mic route alive in pass-through and avoid retry churn for a short cooldown.
  steady_clock::time_point maxineCooldownUntil{};
  std::string maxineCooldownReason;

  // Separate cooldown for speaker processing, so a failure in one direction
  // doesn't permanently disable the other.
  steady_clock::time_point speakerOpenAudioCooldownUntil{};
  std::string speakerOpenAudioCooldownReason;

  steady_clock::time_point lastMicConsumerSeen{};
  steady_clock::time_point lastSpeakerConsumerSeen{};

  constexpr auto availabilityProbeTtl = seconds(2);
  std::optional<CachedMicrophoneSourceStatus> cachedMicSourceStatus;
  steady_clock::time_point nextMicSourceStatusProbe{};

  std::optional<AudioBackendAvailability> cachedMicAvailability;
  std::optional<MicrophoneAvailabilityCacheKey> cachedMicAvailabilityKey;
  steady_clock::time_point nextMicAvailabilityProbe{};

  std::optional<AudioBackendAvailability> cachedSpeakerAvailability;
  std::optional<SpeakerAvailabilityCacheKey> cachedSpeakerAvailabilityKey;
  steady_clock::time_point nextSpeakerAvailabilityProbe{};

#if STUDIOCAST_HAVE_PULSE_SIMPLE
  std::unique_ptr<studiocast::maxine::afx::AfxApi> api;
  std::unique_ptr<studiocast::maxine::afx::AfxEffect> fx;
  std::unique_ptr<AudioProcessor> processor;
  std::unique_ptr<AudioPipelineRunner> pipeline;

  // Independent speaker processing pipeline (virtual speakers -> physical
  // sink).
  std::unique_ptr<studiocast::maxine::afx::AfxApi> spk_api;
  std::unique_ptr<studiocast::maxine::afx::AfxEffect> spk_fx;
  std::unique_ptr<AudioProcessor> spk_processor;
  std::unique_ptr<AudioPipelineRunner> spk_pipeline;

  std::string lastBackend;
  std::optional<studiocast::audio::effects::BroadcastAudioEffects> lastFx;
  std::string lastSource;
  std::filesystem::path lastAfxLib;

  std::string lastSpeakerBackend;
  std::optional<studiocast::audio::effects::BroadcastSpeakerEffects>
      lastSpeakerFx;
  std::string lastSpeakerTargetSink;
  std::filesystem::path lastSpeakerAfxLib;

  bool micRestartingAfterTerminalFailure = false;
  bool speakerRestartingAfterTerminalFailure = false;
#endif

#if STUDIOCAST_HAVE_PULSE_SIMPLE
  auto clearMicPipelineStatsLocked = [this]() {
    st_.pipeline_frames_processed = 0;
    st_.pipeline_process_time_us_sum = 0;
    st_.pipeline_process_time_us_max = 0;
    st_.pipeline_process_time_us_last = 0;
    st_.pipeline_process_overruns = 0;
    st_.pipeline_pulse_capture_latency_us_last = 0;
    st_.pipeline_pulse_playback_latency_us_last = 0;
    st_.pipeline_pulse_latency_us_max = 0;
    st_.pipeline_resync_events = 0;
  };

  auto clearSpeakerPipelineStatsLocked = [this]() {
    st_.speakers_pipeline_frames_processed = 0;
    st_.speakers_pipeline_process_time_us_sum = 0;
    st_.speakers_pipeline_process_time_us_max = 0;
    st_.speakers_pipeline_process_time_us_last = 0;
    st_.speakers_pipeline_process_overruns = 0;
    st_.speakers_pipeline_pulse_capture_latency_us_last = 0;
    st_.speakers_pipeline_pulse_playback_latency_us_last = 0;
    st_.speakers_pipeline_pulse_latency_us_max = 0;
    st_.speakers_pipeline_resync_events = 0;
  };
#endif

  // A daemon that is killed while the monitor plays leaves its tagged loopback
  // loaded, and nothing else removes it. Clear it once per service start, off
  // the caller's thread, whatever the monitor setting says.
  {
    std::string staleErr;
    if (StopMicMonitorRoute(&staleErr)) {
      monitor_route_may_exist_ = false;
    } else {
      monitor_route_may_exist_ = true;
      std::lock_guard<std::mutex> lock(mu_);
      st_.monitor_last_error =
          "Failed to remove a leftover microphone monitor: " + staleErr;
    }
  }

  while (!stop_.load(std::memory_order_acquire)) {
    VirtualAudioServiceConfig cfg;
    {
      std::lock_guard<std::mutex> lock(mu_);
      cfg = cfg_;
    }

    const int pollMs = std::max(25, cfg.poll_ms);

    const bool resolveAutoMicSource = cfg.enabled;
    const bool shouldProbeMicSource = cfg.enabled || !cfg.source_name.empty();
    MicrophoneSourceStatus micSourceStatus;
    if (shouldProbeMicSource) {
      const auto sourceNow = steady_clock::now();
      const bool sourceStatusExpired =
          !cachedMicSourceStatus ||
          cachedMicSourceStatus->configured_source != cfg.source_name ||
          cachedMicSourceStatus->resolve_auto != resolveAutoMicSource ||
          sourceNow >= nextMicSourceStatusProbe;
      if (sourceStatusExpired) {
        cachedMicSourceStatus = CachedMicrophoneSourceStatus{
            cfg.source_name, resolveAutoMicSource,
            ResolveMicrophoneSourceStatus(cfg, resolveAutoMicSource)};
        nextMicSourceStatusProbe = sourceNow + availabilityProbeTtl;
      }
      micSourceStatus = cachedMicSourceStatus->status;
    } else {
      cachedMicSourceStatus.reset();
      nextMicSourceStatusProbe = steady_clock::time_point{};
    }

    {
      std::lock_guard<std::mutex> lock(mu_);
      st_.selected_source = micSourceStatus.selected_source;
      st_.source_availability = micSourceStatus.availability;
      st_.source_error = micSourceStatus.error;
      st_.source_warnings = micSourceStatus.warnings;
    }

    using Pref = studiocast::audio::effects::AudioEffectsEnginePreference;
    const bool speakerEffectsRequested = AnySpeakerEffectRequested(cfg.effects);
    const bool wantSpeakerProcessing = cfg.speakers_enabled &&
                                       speakerEffectsRequested &&
                                       (cfg.effects.engine != Pref::kOff);

#if STUDIOCAST_HAVE_PULSE_SIMPLE
    const bool wantSpeakerProcessingEffective = wantSpeakerProcessing;
#else
    // Speaker processing requires the daemon audio pipeline (libpulse-simple).
    // If unavailable, always fall back to loopback pass-through.
    const bool wantSpeakerProcessingEffective = false;
#endif

    AudioConsumerSnapshot micConsumers;
    AudioConsumerSnapshot speakerConsumers;
    bool micPipelineNeeded = false;
    bool wantSpeakerProcessingActive = false;

    // Ensure virtual devices are present (best-effort).
    //
    // Note: `enabled` controls the microphone processing pipeline; virtual
    // devices may be created and routed independently.
    bool speakerLoopbackStopBlockedProcessing = false;
    std::string speakerLoopbackStopError;
    {
      // Mic device.
      if (cfg.create_virtual_mic && !mic_created_) {
        std::string err;
        if (CreateVirtualMicDevice(&err)) {
          std::lock_guard<std::mutex> lock(mu_);
          mic_created_ = true;
          st_.mic_present = true;
        } else {
          SetLastError("Failed to create virtual mic: " + err);
        }
      }

      // Speakers device and pass-through routing.
      auto setSpeakersError = [&](std::string msg) {
        std::lock_guard<std::mutex> lock(mu_);
        st_.speakers_last_error = std::move(msg);
      };
      auto clearSpeakersError = [&]() {
        std::lock_guard<std::mutex> lock(mu_);
        st_.speakers_last_error.clear();
      };
      auto preserveActiveLoopbackFromState = [&]() -> bool {
        const auto state = studiocast::audio::LoadVirtualSpeakerState();
        if (!state.loopback_module_id)
          return false;

        speakers_loopback_running_ = true;
        if (state.loopback_target_sink_name) {
          speakers_loopback_target_ = *state.loopback_target_sink_name;
        }
        {
          std::lock_guard<std::mutex> lock(mu_);
          st_.speakers_route_mode = "loopback";
          st_.speakers_routing_active = true;
          st_.speaker_target_sink_active =
              state.loopback_target_sink_name.value_or(
                  speakers_loopback_target_);
        }
        return true;
      };
      auto markActiveLoopbackStopFailure = [&](const std::string &err) {
        {
          std::lock_guard<std::mutex> lock(mu_);
          st_.speakers_route_mode = "loopback";
          st_.speakers_routing_active = true;
          if (st_.speaker_target_sink_active.empty()) {
            st_.speaker_target_sink_active = speakers_loopback_target_;
          }
        }
        setSpeakersError("Failed to stop speakers routing: " + err);
      };
      auto stopSpeakerLoopbackForTransition = [&](std::string *error) -> bool {
        const auto stopNow = steady_clock::now();
        if (stopNow < nextSpeakerLoopbackStopRetry) {
          const std::string err =
              speakerLoopbackStopRetryError.empty()
                  ? "previous stop failure is still in retry backoff"
                  : speakerLoopbackStopRetryError;
          if (error)
            *error = err;
          markActiveLoopbackStopFailure(err);
          return false;
        }

        std::string err;
        if (StopSpeakerLoopbackRoute(&err)) {
          nextSpeakerLoopbackStopRetry = steady_clock::time_point{};
          speakerLoopbackStopRetryError.clear();
          speakers_loopback_running_ = false;
          speakers_loopback_target_.clear();
          speakers_loopback_latency_ms_ = 0;
          {
            std::lock_guard<std::mutex> lock(mu_);
            st_.speakers_routing_active = false;
            st_.speaker_target_sink_active.clear();
          }
          clearSpeakersError();
          if (error)
            error->clear();
          return true;
        }

        if (err.empty())
          err = "speaker loopback stop failed";
        nextSpeakerLoopbackStopRetry = stopNow + StartFailureRetryDelay(cfg);
        speakerLoopbackStopRetryError = err;
        if (error)
          *error = err;
        markActiveLoopbackStopFailure(err);
        return false;
      };

      const bool wantSpeakersDevice =
          cfg.create_virtual_speakers || cfg.speakers_enabled;

      if (wantSpeakersDevice && !speakers_created_) {
        std::string err;
        if (CreateVirtualSpeakerDevice(&err)) {
          speakers_created_ = true;
          {
            std::lock_guard<std::mutex> lock(mu_);
            st_.speakers_present = true;
          }
          clearSpeakersError();
        } else {
          setSpeakersError("Failed to create virtual speakers: " + err);
        }
      }

      const auto consumerNow = steady_clock::now();
      const auto consumerGrace =
          milliseconds(std::max(0, cfg.consumer_grace_ms));

      if (cfg.enabled) {
        micConsumers = DetectMicrophoneConsumers();
        if (micConsumers.present)
          lastMicConsumerSeen = consumerNow;
      }

      if (wantSpeakerProcessingEffective) {
        speakerConsumers = DetectSpeakerConsumers();
        if (speakerConsumers.present)
          lastSpeakerConsumerSeen = consumerNow;
      }

#if STUDIOCAST_HAVE_PULSE_SIMPLE
      const bool keepMicDuringGrace =
          !micConsumers.present && pipeline &&
          lastMicConsumerSeen != steady_clock::time_point{} &&
          consumerGrace.count() > 0 &&
          (consumerNow - lastMicConsumerSeen) < consumerGrace;
      const bool keepSpeakerDuringGrace =
          !speakerConsumers.present && spk_pipeline &&
          lastSpeakerConsumerSeen != steady_clock::time_point{} &&
          consumerGrace.count() > 0 &&
          (consumerNow - lastSpeakerConsumerSeen) < consumerGrace;
#else
      const bool keepMicDuringGrace = false;
      const bool keepSpeakerDuringGrace = false;
#endif

      micPipelineNeeded =
          cfg.enabled && (micConsumers.present || keepMicDuringGrace);
      wantSpeakerProcessingActive =
          wantSpeakerProcessingEffective &&
          (speakerConsumers.present || keepSpeakerDuringGrace);

      {
        std::lock_guard<std::mutex> lock(mu_);
        st_.mic_consumer_present = micConsumers.present;
        st_.mic_consumer_count = micConsumers.count;
        st_.mic_consumer_error = micConsumers.error;
        st_.speakers_consumer_present = speakerConsumers.present;
        st_.speakers_consumer_count = speakerConsumers.count;
        st_.speakers_consumer_error = speakerConsumers.error;
        st_.pipeline_active_needed = micPipelineNeeded;
        st_.speakers_pipeline_active_needed = wantSpeakerProcessingActive;
      }

      // Keep speakers routing state consistent with config.
      //
      // Pass-through mode uses Pulse module-loopback.
      // When speaker effects are enabled, we disable the loopback and run a
      // processed pipeline (see speaker pipeline supervisor below).
      if (cfg.speakers_enabled) {
        if (wantSpeakerProcessingEffective) {
          // Ensure loopback is stopped (avoid double-routing).
          if (speakers_loopback_running_) {
            std::string err;
            if (!stopSpeakerLoopbackForTransition(&err)) {
              speakerLoopbackStopBlockedProcessing = true;
              speakerLoopbackStopError = err;
            }
          }

          if (!speakerLoopbackStopBlockedProcessing) {
            std::lock_guard<std::mutex> lock(mu_);
            st_.speakers_route_mode = "pipeline";
          }
        } else {
#if STUDIOCAST_HAVE_PULSE_SIMPLE
          // If we are switching back to pass-through, stop the processed
          // pipeline before enabling loopback.
          if (spk_pipeline) {
            spk_pipeline->Stop();
            spk_pipeline.reset();
          }
          spk_processor.reset();
          if (spk_fx) {
            spk_fx->Destroy();
            spk_fx.reset();
          }
          spk_api.reset();
          lastSpeakerFx.reset();
          lastSpeakerBackend.clear();
          lastSpeakerTargetSink.clear();
          lastSpeakerAfxLib.clear();
          {
            std::lock_guard<std::mutex> lock(mu_);
            st_.speakers_pipeline_running = false;
            st_.speakers_pipeline_starting = false;
            st_.speakers_backend_active.clear();
            st_.speakers_effects_note.clear();
            st_.speakers_intensity = 0.0f;
            st_.speakers_pipeline_last_error.clear();
            st_.speakers_open_audio_runtime = {};
            st_.speakers_pipeline_state = "disabled";
            st_.speakers_pipeline_idle_reason =
                "Speaker processing is not requested.";
            clearSpeakerPipelineStatsLocked();
          }
#endif

          const bool needLoopbackRestart =
              (!speakers_loopback_running_) ||
              (speakers_loopback_target_ != cfg.speaker_target_sink) ||
              (speakers_loopback_latency_ms_ != cfg.speaker_latency_ms);
          const auto loopbackNow = steady_clock::now();
          if (needLoopbackRestart &&
              loopbackNow >= nextSpeakerLoopbackStartRetry) {
            std::string err;
            if (StartSpeakerLoopbackRoute(cfg.speaker_target_sink,
                                          std::max(1, cfg.speaker_latency_ms),
                                          &err)) {
              speakers_created_ = true;
              speakers_loopback_running_ = true;
              speakers_loopback_target_ = cfg.speaker_target_sink;
              speakers_loopback_latency_ms_ = cfg.speaker_latency_ms;
              nextSpeakerLoopbackStartRetry = steady_clock::time_point{};
              nextSpeakerLoopbackStopRetry = steady_clock::time_point{};
              speakerLoopbackStopRetryError.clear();

              const auto state = studiocast::audio::LoadVirtualSpeakerState();
              {
                std::lock_guard<std::mutex> lock(mu_);
                st_.speakers_present = true;
                st_.speakers_routing_active = true;
                st_.speakers_route_mode = "loopback";
                st_.speaker_target_sink_active =
                    state.loopback_target_sink_name.value_or(std::string());
              }
              clearSpeakersError();
            } else {
              nextSpeakerLoopbackStartRetry =
                  loopbackNow + StartFailureRetryDelay(cfg);
              const bool failedStoppingExisting =
                  err.find("Failed to stop existing speaker loopback") !=
                  std::string::npos;
              bool preservedActiveLoopback = false;
              if (failedStoppingExisting) {
                preservedActiveLoopback = preserveActiveLoopbackFromState();
              }
              if (!preservedActiveLoopback) {
                speakers_loopback_running_ = false;
                speakers_loopback_target_.clear();
                speakers_loopback_latency_ms_ = 0;
                {
                  std::lock_guard<std::mutex> lock(mu_);
                  st_.speakers_route_mode = "loopback";
                  st_.speakers_routing_active = false;
                  st_.speaker_target_sink_active.clear();
                }
              }
              setSpeakersError("Failed to start speakers routing: " + err);
            }
          } else {
            std::lock_guard<std::mutex> lock(mu_);
            st_.speakers_route_mode = "loopback";
          }
        }
      } else {
#if STUDIOCAST_HAVE_PULSE_SIMPLE
        // Stop processed speaker pipeline.
        if (spk_pipeline) {
          spk_pipeline->Stop();
          spk_pipeline.reset();
        }
        spk_processor.reset();
        if (spk_fx) {
          spk_fx->Destroy();
          spk_fx.reset();
        }
        spk_api.reset();
        lastSpeakerFx.reset();
        lastSpeakerBackend.clear();
        lastSpeakerTargetSink.clear();
        lastSpeakerAfxLib.clear();
        {
          std::lock_guard<std::mutex> lock(mu_);
          st_.speakers_pipeline_running = false;
          st_.speakers_pipeline_starting = false;
          st_.speakers_backend_active.clear();
          st_.speakers_effects_note.clear();
          st_.speakers_intensity = 0.0f;
          st_.speakers_pipeline_last_error.clear();
          st_.speakers_open_audio_runtime = {};
          st_.speakers_pipeline_state = "disabled";
          st_.speakers_pipeline_idle_reason =
              "Speaker processing is not requested.";
          clearSpeakerPipelineStatsLocked();
        }
#endif

        if (speakers_loopback_running_) {
          std::string err;
          (void)stopSpeakerLoopbackForTransition(&err);
        }

        {
          std::lock_guard<std::mutex> lock(mu_);
          st_.speakers_route_mode =
              speakers_loopback_running_ ? "loopback" : "off";
        }

        // Optional cleanup: if the user disables the device, and we previously
        // created it, destroy it. This keeps daemon-managed speaker state
        // predictable.
        if (!cfg.create_virtual_speakers && speakers_created_ &&
            !speakers_loopback_running_) {
          const auto destroyNow = steady_clock::now();
          if (destroyNow >= nextSpeakerDestroyRetry) {
            std::string err;
            if (DestroyVirtualSpeakerDevice(&err)) {
              nextSpeakerDestroyRetry = steady_clock::time_point{};
              speakerDestroyRetryError.clear();
              speakers_created_ = false;
              speakers_loopback_running_ = false;
              speakers_loopback_target_.clear();
              speakers_loopback_latency_ms_ = 0;
              {
                std::lock_guard<std::mutex> lock(mu_);
                st_.speakers_present = false;
                st_.speakers_routing_active = false;
                st_.speaker_target_sink_active.clear();
              }
              clearSpeakersError();
            } else {
              if (err.empty())
                err = "virtual speaker destroy failed";
              nextSpeakerDestroyRetry =
                  destroyNow + StartFailureRetryDelay(cfg);
              speakerDestroyRetryError = err;
              setSpeakersError("Failed to destroy virtual speakers: " + err);
            }
          } else {
            const std::string err =
                speakerDestroyRetryError.empty()
                    ? "previous destroy failure is still in retry backoff"
                    : speakerDestroyRetryError;
            setSpeakersError("Failed to destroy virtual speakers: " + err);
          }
        }
      }
    }

    // Microphone monitor supervisor.
    //
    // The monitor loops the processed microphone feed to an output sink. It is
    // a Pulse consumer of `studiocast_mic`, so it also starts the microphone
    // pipeline. The consumer counts below keep it apart from real apps.
    {
      const auto monitorCfg = NormalizeMicMonitorConfig(cfg.monitor);
      // The monitor loops from `studiocast_mic`, so it needs the virtual
      // microphone. Without it every start would fail, forever.
      const bool wantMonitor =
          monitorCfg.enabled && cfg.enabled && mic_created_;
      const auto monitorNow = steady_clock::now();
      // The consumer snapshot for this pass was taken before this block runs,
      // so it knows only the route that was already loaded. Split the counts
      // by that, not by the route this pass is about to load or unload.
      const bool monitorRanWhenConsumersWereCounted = monitor_running_;

      auto setMonitorError = [&](std::string msg) {
        // Any other write owns the reported error from here on, so a later
        // volume step must not clear it.
        monitorVolumeFailed = false;
        std::lock_guard<std::mutex> lock(mu_);
        st_.monitor_last_error = std::move(msg);
      };
      auto setMonitorNote = [&](std::string msg) {
        std::lock_guard<std::mutex> lock(mu_);
        st_.monitor_note = std::move(msg);
      };
      // Forgets the live route but keeps what the last start asked for, so a
      // settings change stays tellable from a lost output.
      auto clearMonitorRouteState = [&]() {
        monitor_running_ = false;
        std::lock_guard<std::mutex> lock(mu_);
        st_.monitor_active = false;
        st_.monitor_module_id = -1;
        st_.monitor_sink_active.clear();
        st_.monitor_latency_ms_active = 0;
      };
      auto forgetMonitorRequest = [&]() {
        monitor_sink_requested_.clear();
        monitor_sink_resolved_.clear();
        monitor_module_id_ = -1;
        monitor_latency_ms_ = 0;
        monitor_volume_ = 0;
      };
      // True only when the sound server answered and the sink was not in its
      // list. A sound server that cannot be reached gives no answer at all,
      // and then the monitor keeps its pin and retries: a monitor that waits
      // for the sound server comes back on its own, while a lost output needs
      // the user.
      //
      // Why the sound server could not answer is known here and nowhere else,
      // so it is kept for the error of the failed start. A monitor that
      // retries a pinned start would otherwise leave no trace of whether the
      // sink list timed out, failed, or was never asked.
      std::string monitorSinkQuestionError;
      auto monitorOutputIsGone = [&](const std::string &sink) {
        monitorSinkQuestionError.clear();
        std::string presentErr;
        const auto present = MicMonitorSinkPresentRoute(sink, &presentErr);
        if (!present.has_value() && !presentErr.empty())
          monitorSinkQuestionError = presentErr;
        return present.has_value() && !*present;
      };
      // The output the monitor played on is gone, for example because the
      // headset was unplugged. A configured sink of "auto" would now resolve
      // to whatever the Pulse default has become, usually the built-in
      // speakers, which is a feedback loop the user never asked for. Stop
      // instead and say what happened.
      //
      // The pin on the resolved output goes with the output it names, so the
      // next explicit restart resolves "auto" afresh instead of asking for a
      // sink that is gone.
      //
      // This is the failure the feature is designed for, and the sentence says
      // what to do about it. It goes in the note, which the GUI prints as
      // written. An error would become "The monitor needs attention. Open
      // Support for technical details."
      // `lost_sink` is taken by value because a caller may hand in
      // `monitor_sink_resolved_`, which this drops.
      //
      // This says nothing about `monitor_route_may_exist_`. A lost output is
      // an answer about the sink list, and only the module list, or a stop
      // that worked, says whether a loopback is still loaded. A caller that
      // has such an answer clears the flag itself.
      auto reportMonitorOutputLost = [&](std::string lost_sink) {
        clearMonitorRouteState();
        monitor_output_lost_ = true;
        nextMonitorLostCleanup = steady_clock::time_point{};
        monitorLostCleanupFailures = 0;
        monitor_sink_resolved_.clear();
        monitorStartFailures = 0;
        nextMonitorStartRetry = steady_clock::time_point{};
        setMonitorError(std::string());
        setMonitorNote("The monitor output '" + lost_sink +
                       "' disappeared, so the monitor stopped. Choose another "
                       "output, or turn the monitor off and on again to use "
                       "the system default.");
      };

      if (!wantMonitor) {
        if (monitor_output_lost_) {
          // Turning the monitor off ends the lost-output stop and its message.
          monitor_output_lost_ = false;
          forgetMonitorRequest();
          setMonitorError(std::string());
        }
        if ((monitor_running_ || monitor_route_may_exist_) &&
            monitorNow >= nextMonitorStopRetry) {
          std::string err;
          if (StopMicMonitorRoute(&err)) {
            monitor_route_may_exist_ = false;
            monitorStopFailures = 0;
            nextMonitorStopRetry = steady_clock::time_point{};
            clearMonitorRouteState();
            forgetMonitorRequest();
            setMonitorError(std::string());
          } else {
            // The loopback may still play, so keep coming back to it instead
            // of forgetting it.
            monitor_route_may_exist_ = true;
            monitorStopFailures = std::min(monitorStopFailures + 1, 8);
            nextMonitorStopRetry =
                monitorNow + StartFailureRetryDelay(cfg) * monitorStopFailures;
            setMonitorError("Failed to stop the microphone monitor: " + err);
          }
        } else if (!monitor_running_ && !monitor_route_may_exist_) {
          // Nothing plays and nothing is wanted, so an older failure, a failed
          // start included, is history. Keeping it would tell the user for
          // ever that a monitor they turned off needs attention.
          setMonitorError(std::string());
        }
        nextMonitorStartRetry = steady_clock::time_point{};
        monitorStartFailures = 0;
        monitorAttemptSink.clear();
        monitorAttemptLatencyMs = 0;
        // A monitor that waits for microphone processing, or for the virtual
        // microphone, is an ordinary state and not a failure, so it goes in
        // the note.
        std::string monitorNote;
        if (monitorCfg.enabled && !cfg.enabled) {
          monitorNote = "Microphone processing is off, so the monitor stays "
                        "idle. Turn microphone processing on to hear the "
                        "monitor.";
        } else if (monitorCfg.enabled && !mic_created_) {
          monitorNote = "StudioCast Microphone is not available yet, so the "
                        "monitor stays idle.";
        }
        setMonitorNote(std::move(monitorNote));
      } else {
        // A monitor the user wants again starts its stop wait over. The
        // deadline goes with the counter: keeping it would make the next stop
        // the user asks for wait out the backoff of the failures before it.
        monitorStopFailures = 0;
        nextMonitorStopRetry = steady_clock::time_point{};
        // A lost output keeps its note until something restarts the monitor.
        if (!monitor_output_lost_)
          setMonitorNote(std::string());
        // A changed setting is an explicit restart by the user: it resolves
        // the output afresh and ends a lost-output stop.
        const bool monitorSettingsChanged =
            monitor_sink_requested_ != monitorCfg.sink ||
            monitor_latency_ms_ != monitorCfg.latency_ms;
        if (monitorSettingsChanged) {
          monitor_output_lost_ = false;
          setMonitorNote(std::string());
        }
        if (monitorAttemptSink != monitorCfg.sink ||
            monitorAttemptLatencyMs != monitorCfg.latency_ms) {
          monitorAttemptSink = monitorCfg.sink;
          monitorAttemptLatencyMs = monitorCfg.latency_ms;
          monitorStartFailures = 0;
          nextMonitorStartRetry = steady_clock::time_point{};
        }

        // The Pulse server unloads the loopback when the sink disappears, for
        // example when a headset is unplugged. Re-check now and then so the
        // status follows the route.
        if (monitor_running_ && monitorNow >= nextMonitorVerify) {
          std::string detectErr;
          const auto live = DetectMicMonitorRoute(&detectErr);
          nextMonitorVerify = monitorNow + seconds(2);
          if (!detectErr.empty()) {
            // A failed check says nothing about the loopback, so the status
            // must not go on claiming an active monitor. Stop the route the
            // service can no longer see, and keep the knowledge that it may
            // still play when the stop fails too.
            std::string stopErr;
            if (StopMicMonitorRoute(&stopErr))
              monitor_route_may_exist_ = false;
            clearMonitorRouteState();
            setMonitorError("Failed to check the microphone monitor: " +
                            detectErr);
          } else if (!live.active) {
            // Pulse unloads the loopback when the output disappears. The
            // check read the module list and found no tagged loopback, so
            // nothing is left to remove.
            monitor_route_may_exist_ = false;
            reportMonitorOutputLost(monitor_sink_resolved_.empty()
                                        ? monitorCfg.sink
                                        : monitor_sink_resolved_);
          }
        }

        // The volume belongs to the loopback sink input, so it applies in
        // place. Reloading the module for it would break the sound once per
        // step of the volume control.
        if (monitor_running_ && monitor_volume_ != monitorCfg.volume &&
            monitorNow >= nextMonitorVolumeRetry) {
          std::string volumeErr;
          if (SetMicMonitorRouteVolume(monitor_module_id_, monitorCfg.volume,
                                       &volumeErr)) {
            monitor_volume_ = monitorCfg.volume;
            monitorVolumeFailures = 0;
            nextMonitorVolumeRetry = steady_clock::time_point{};
            if (monitorVolumeFailed) {
              monitorVolumeFailed = false;
              setMonitorError(std::string());
            }
          } else {
            monitorVolumeFailures = std::min(monitorVolumeFailures + 1, 8);
            nextMonitorVolumeRetry = monitorNow + StartFailureRetryDelay(cfg) *
                                                      monitorVolumeFailures;
            setMonitorError("Failed to set the microphone monitor volume: " +
                            volumeErr);
            monitorVolumeFailed = true;
          }
        }

        // A lost output stops the monitor but removes no loopback: a stop that
        // failed in the same window can have left one loaded, and it plays the
        // microphone into the speakers until something unloads it. The cleanup
        // therefore keeps its retry while the output is lost, instead of
        // waiting for the service to stop. It stays quiet, because the note
        // already tells the user what to do and the shutdown cleanup reports a
        // stop that never works.
        //
        // The retry ends at the top of the backoff. Every try blocks the audio
        // supervisor for one stop deadline, and only the user ends a lost
        // output, so a retry without an end would block the supervisor for the
        // rest of the run. The knowledge that a loopback may still play stays
        // behind, and the stop at shutdown, or the next thing the user asks
        // for, uses it.
        if (monitor_output_lost_ && monitor_route_may_exist_ &&
            monitorLostCleanupFailures < kMonitorLostCleanupMaxTries &&
            monitorNow >= nextMonitorLostCleanup) {
          std::string cleanupErr;
          if (StopMicMonitorRoute(&cleanupErr)) {
            monitor_route_may_exist_ = false;
            monitorLostCleanupFailures = 0;
            nextMonitorLostCleanup = steady_clock::time_point{};
            clearMonitorRouteState();
          } else {
            monitorLostCleanupFailures = std::min(
                monitorLostCleanupFailures + 1, kMonitorLostCleanupMaxTries);
            nextMonitorLostCleanup =
                monitorNow +
                StartFailureRetryDelay(cfg) * monitorLostCleanupFailures;
          }
        }

        const bool needRestart = !monitor_output_lost_ &&
                                 (!monitor_running_ || monitorSettingsChanged);

        if (needRestart && monitorNow >= nextMonitorStartRetry) {
          // A restart the user did not ask for keeps the output the last start
          // really used. Only a settings change, or a monitor that was off,
          // resolves "auto" afresh: doing it here would let a failed check
          // move the monitor onto the new Pulse default, which is the feedback
          // loop the lost-output stop exists to prevent.
          MicMonitorConfig startCfg = monitorCfg;
          const bool startUsedThePin =
              !monitorSettingsChanged && !monitor_sink_resolved_.empty();
          if (startUsedThePin)
            startCfg.sink = monitor_sink_resolved_;

          MicMonitorState state;
          std::string err;
          if (StartMicMonitorRoute(startCfg, micSourceStatus.selected_source,
                                   &state, &err)) {
            monitor_running_ = true;
            monitor_route_may_exist_ = true;
            monitor_sink_requested_ = monitorCfg.sink;
            monitor_sink_resolved_ = state.sink;
            monitor_module_id_ = state.module_id;
            monitor_latency_ms_ = monitorCfg.latency_ms;
            monitor_volume_ = monitorCfg.volume;
            monitorStartFailures = 0;
            nextMonitorStartRetry = steady_clock::time_point{};
            nextMonitorVolumeRetry = steady_clock::time_point{};
            nextMonitorVerify = monitorNow + seconds(2);
            monitorVolumeFailed = false;
            monitorVolumeFailures = 0;
            {
              std::lock_guard<std::mutex> lock(mu_);
              st_.monitor_active = true;
              st_.monitor_module_id = state.module_id;
              st_.monitor_sink_active = state.sink;
              st_.monitor_latency_ms_active = state.latency_ms;
              st_.monitor_last_error = state.warning;
            }
          } else if (startUsedThePin && monitorOutputIsGone(startCfg.sink)) {
            // A pinned start that fails is the moment to ask whether the
            // output the pin names is still there. A failed route check
            // already stopped the route without noticing the loss, so this is
            // the only place the loss can still be found: retrying it would
            // ask a sink that no longer exists for the rest of the run.
            reportMonitorOutputLost(startCfg.sink);
          } else {
            monitorStartFailures = std::min(monitorStartFailures + 1, 8);
            nextMonitorStartRetry =
                monitorNow + StartFailureRetryDelay(cfg) * monitorStartFailures;
            clearMonitorRouteState();
            std::string message =
                "Failed to start the microphone monitor: " + err;
            if (!monitorSinkQuestionError.empty()) {
              // The pin holds, so say why the sound server could not say
              // whether the output is still there. The message opens with the
              // shared no-answer text, because the GUI reads that text as a
              // wait and not as a request to open Support.
              message += " " + std::string(kSoundServerNoAnswerMessage) +
                         ", so it is not known whether the output '" +
                         startCfg.sink +
                         "' is still there: " + monitorSinkQuestionError;
            }
            setMonitorError(message);
          }
        }
      }

      {
        std::lock_guard<std::mutex> lock(mu_);
        st_.mic_monitor_consumer_count =
            (monitorRanWhenConsumersWereCounted && st_.mic_consumer_count > 0)
                ? 1
                : 0;
        st_.mic_app_consumer_count = std::max(
            0, st_.mic_consumer_count - st_.mic_monitor_consumer_count);
      }
    }

    (void)speakerLoopbackStopError;

#if STUDIOCAST_HAVE_PULSE_SIMPLE
    // Speaker processed pipeline supervisor.
    //
    // This runs independently from the microphone pipeline, allowing speaker
    // noise removal even when microphone effects are disabled.
    if (wantSpeakerProcessingEffective && !wantSpeakerProcessingActive) {
      if (spk_pipeline) {
        spk_pipeline->Stop();
        spk_pipeline.reset();
      }
      spk_processor.reset();
      if (spk_fx) {
        spk_fx->Destroy();
        spk_fx.reset();
      }
      spk_api.reset();
      lastSpeakerFx.reset();
      lastSpeakerBackend.clear();
      lastSpeakerTargetSink.clear();
      lastSpeakerAfxLib.clear();
      speakerRestartingAfterTerminalFailure = false;

      {
        std::lock_guard<std::mutex> lock(mu_);
        if (!speakerLoopbackStopBlockedProcessing) {
          st_.speakers_route_mode = "pipeline";
          st_.speakers_routing_active = false;
          st_.speaker_target_sink_active.clear();
        }
        st_.speakers_pipeline_running = false;
        st_.speakers_pipeline_starting = false;
        st_.speakers_backend_active.clear();
        st_.speakers_effects_note.clear();
        st_.speakers_intensity = 0.0f;
        st_.speakers_pipeline_last_error.clear();
        st_.speakers_open_audio_runtime = {};
        st_.speakers_pipeline_state = "idle_no_consumer";
        st_.speakers_pipeline_idle_reason =
            speakerConsumers.error.empty()
                ? "No active virtual speakers consumer."
                : "Virtual speakers consumer detection unavailable: " +
                      speakerConsumers.error;
        clearSpeakerPipelineStatsLocked();
      }
    } else if (wantSpeakerProcessingEffective) {
      if (speakerLoopbackStopBlockedProcessing) {
        if (spk_pipeline) {
          spk_pipeline->Stop();
          spk_pipeline.reset();
        }
        spk_processor.reset();
        if (spk_fx) {
          spk_fx->Destroy();
          spk_fx.reset();
        }
        spk_api.reset();
        lastSpeakerFx.reset();
        lastSpeakerBackend.clear();
        lastSpeakerTargetSink.clear();
        lastSpeakerAfxLib.clear();

        {
          std::lock_guard<std::mutex> lock(mu_);
          st_.speakers_route_mode = "loopback";
          st_.speakers_pipeline_running = false;
          st_.speakers_pipeline_starting = false;
          st_.speakers_routing_active = true;
          if (st_.speaker_target_sink_active.empty()) {
            st_.speaker_target_sink_active = speakers_loopback_target_;
          }
          st_.speakers_pipeline_last_error =
              "Speaker processing blocked while stopping existing loopback: " +
              speakerLoopbackStopError;
          st_.speakers_open_audio_runtime = {};
          st_.speakers_pipeline_state = "blocked";
          st_.speakers_pipeline_idle_reason.clear();
          clearSpeakerPipelineStatsLocked();
        }
      } else if (!speakers_created_) {
        // We can't route/process speaker audio until the virtual speakers
        // device exists.
        if (spk_pipeline) {
          spk_pipeline->Stop();
          spk_pipeline.reset();
        }
        spk_processor.reset();
        if (spk_fx) {
          spk_fx->Destroy();
          spk_fx.reset();
        }

        {
          std::lock_guard<std::mutex> lock(mu_);
          st_.speakers_routing_active = false;
          st_.speakers_route_mode = "pipeline";
          st_.speaker_target_sink_active.clear();
          st_.speakers_pipeline_running = false;
          st_.speakers_pipeline_starting = false;
          clearSpeakerPipelineStatsLocked();
          if (st_.speakers_pipeline_last_error.empty()) {
            st_.speakers_pipeline_last_error =
                "Virtual speakers device not created.";
          }
          st_.speakers_open_audio_runtime = {};
          st_.speakers_pipeline_state = "failed";
          st_.speakers_pipeline_idle_reason.clear();
        }
      } else {
        const auto now = steady_clock::now();

        // Compute the "plan" for speaker noise removal using the same AFX
        // planner.
        auto speakerPlan =
            studiocast::maxine::afx::PlanBroadcastMicrophoneEffect(
                /*studio_voice_enabled=*/false,
                /*noise_removal_enabled=*/
                cfg.effects.speaker.noise_removal_enabled,
                /*room_echo_removal_enabled=*/
                cfg.effects.speaker.room_echo_removal_enabled,
                /*strength=*/cfg.effects.speaker.strength);
        if (!speakerPlan.enabled)
          speakerPlan.intensity = 0.0f;

        // Availability + backend selection.
        const auto speakerAvailabilityKey =
            MakeSpeakerAvailabilityCacheKey(cfg.effects);
        const bool speakerAvailabilityExpired =
            !cachedSpeakerAvailability ||
            !cachedSpeakerAvailabilityKey.has_value() ||
            *cachedSpeakerAvailabilityKey != speakerAvailabilityKey ||
            now >= nextSpeakerAvailabilityProbe;
        if (speakerAvailabilityExpired) {
          cachedSpeakerAvailability = ProbeSpeakerBackendAvailability(cfg);
          cachedSpeakerAvailabilityKey = speakerAvailabilityKey;
          nextSpeakerAvailabilityProbe = now + availabilityProbeTtl;
        }
        AudioBackendAvailability speakerAvail = *cachedSpeakerAvailability;
        bool speakerOpenAudioCooldownActive = false;
        if (now < speakerOpenAudioCooldownUntil) {
          speakerOpenAudioCooldownActive = true;
          speakerAvail.open_source_ok = false;
          speakerAvail.open_source_reason =
              speakerOpenAudioCooldownReason.empty()
                  ? "Open Audio backend is temporarily disabled due to a "
                    "previous failure."
                  : speakerOpenAudioCooldownReason;
        }
        const auto speakerDecision =
            ResolveAudioBackend(cfg.effects, speakerAvail);

        const bool wantSpkMaxine =
            (speakerDecision.backend == AudioBackendKind::kMaxine) &&
            speakerPlan.enabled;
        const bool wantSpkOpenAudio =
            (speakerDecision.backend == AudioBackendKind::kOpenSource) &&
            speakerPlan.enabled;

        std::string desiredSpkBackend = "passthrough";
        if (wantSpkMaxine) {
          desiredSpkBackend = "maxine";
        } else if (wantSpkOpenAudio) {
          desiredSpkBackend = "open_source";
        }
        {
          std::lock_guard<std::mutex> lock(mu_);
          if (speakerOpenAudioCooldownActive) {
            MarkOpenAudioRuntimeDisabled(&st_.speakers_open_audio_runtime,
                                         speakerAvail.open_source_reason);
          } else if (!wantSpkOpenAudio) {
            st_.speakers_open_audio_runtime = {};
          }
        }

        // Choose target sink. If misconfigured (virtual sink), fall back to a
        // safe physical sink.
        std::string sinkErr;
        auto sinkOpt =
            ChooseSpeakerTargetSinkName(cfg.speaker_target_sink, &sinkErr);
        if (!sinkOpt) {
          std::string sinkErr2;
          sinkOpt =
              ChooseSpeakerTargetSinkName(/*configured_target=*/"", &sinkErr2);
        }

        if (!sinkOpt) {
          // Can't route speakers anywhere.
          if (spk_pipeline) {
            spk_pipeline->Stop();
            spk_pipeline.reset();
          }
          spk_processor.reset();
          if (spk_fx) {
            spk_fx->Destroy();
            spk_fx.reset();
          }
          spk_api.reset();
          lastSpeakerFx.reset();
          lastSpeakerBackend.clear();
          lastSpeakerTargetSink.clear();
          lastSpeakerAfxLib.clear();

          {
            std::lock_guard<std::mutex> lock(mu_);
            st_.speakers_route_mode = "pipeline";
            st_.speakers_routing_active = false;
            st_.speaker_target_sink_active.clear();
            st_.speakers_pipeline_running = false;
            st_.speakers_pipeline_starting = false;
            st_.speakers_backend_active = "passthrough";
            st_.speakers_effects_note = "Speakers processing is enabled, but "
                                        "no valid output sink was found.";
            st_.speakers_intensity = speakerPlan.intensity;
            st_.speakers_pipeline_last_error = sinkErr;
            st_.speakers_open_audio_runtime = {};
            st_.speakers_pipeline_state = "failed";
            st_.speakers_pipeline_idle_reason.clear();
            clearSpeakerPipelineStatsLocked();
          }
        } else {
          const std::string sinkName = *sinkOpt;

          // Start/restart the pipeline if needed.
          const bool speakerRestartKeyChanged =
              (!lastSpeakerFx.has_value() ||
               SpeakerPipelineEffectsRequireRestart(*lastSpeakerFx,
                                                    cfg.effects.speaker));
          const bool speakerStrengthChanged =
              lastSpeakerFx.has_value() &&
              lastSpeakerFx->strength != cfg.effects.speaker.strength;
          const bool speakerEffectsChanged =
              speakerRestartKeyChanged ||
              (wantSpkMaxine && speakerStrengthChanged);
          std::optional<AudioPipelineStats> spkStatsBeforeRestart;
          if (spk_pipeline) {
            spkStatsBeforeRestart = spk_pipeline->GetStats();
          }
          const bool spkPipelineDead =
              (spkStatsBeforeRestart && !spkStatsBeforeRestart->running);
          std::string spkTerminalError;
          if (spkPipelineDead) {
            spkTerminalError = spkStatsBeforeRestart->last_error.empty()
                                   ? "Speaker audio pipeline stopped."
                                   : spkStatsBeforeRestart->last_error;
          }
          const bool needSpkRestart =
              (!spk_pipeline) || spkPipelineDead ||
              (lastSpeakerBackend != desiredSpkBackend) ||
              (lastSpeakerTargetSink != sinkName) ||
              ((wantSpkMaxine || wantSpkOpenAudio) && speakerEffectsChanged);

          if (spkPipelineDead) {
            if (spk_pipeline) {
              spk_pipeline->Stop();
              spk_pipeline.reset();
            }
            spk_processor.reset();
            if (spk_fx) {
              spk_fx->Destroy();
              spk_fx.reset();
            }
            if (!wantSpkMaxine) {
              spk_api.reset();
              lastSpeakerAfxLib.clear();
            }
            lastSpeakerFx.reset();
            lastSpeakerBackend.clear();
            lastSpeakerTargetSink.clear();
            speakerRestartingAfterTerminalFailure = true;
            nextSpeakerStartRetry = now + WorkerDeathRetryDelay(cfg);

            {
              std::lock_guard<std::mutex> lock(mu_);
              st_.speakers_route_mode = "pipeline";
              st_.speakers_pipeline_starting = false;
              st_.speakers_pipeline_running = false;
              st_.speakers_routing_active = false;
              st_.speaker_target_sink_active.clear();
              st_.speakers_pipeline_last_error = spkTerminalError;
              ApplyOpenAudioRuntimeWarning(&st_.speakers_open_audio_runtime,
                                           spkTerminalError);
              if (st_.speakers_open_audio_runtime.active)
                st_.speakers_open_audio_runtime.active = false;
              st_.speakers_pipeline_state = "failed";
              st_.speakers_pipeline_idle_reason.clear();
              clearSpeakerPipelineStatsLocked();
            }
          } else if (now >= nextSpeakerStartRetry && needSpkRestart) {
            if (spk_pipeline) {
              spk_pipeline->Stop();
              spk_pipeline.reset();
            }
            spk_processor.reset();
            if (spk_fx) {
              spk_fx->Destroy();
              spk_fx.reset();
            }
            if (!wantSpkMaxine) {
              // Pass-through + open-source don't need the AFX runtime.
              spk_api.reset();
              lastSpeakerAfxLib.clear();
              lastSpeakerFx.reset();
            }

            {
              std::lock_guard<std::mutex> lock(mu_);
              st_.speakers_route_mode = "pipeline";
              st_.speakers_pipeline_starting = true;
              st_.speakers_pipeline_running = false;
              st_.speakers_routing_active = false;
              st_.speaker_target_sink_active.clear();
              st_.speakers_backend_active =
                  std::string(ToString(speakerDecision.backend));
              st_.speakers_effects_note = speakerDecision.note;
              st_.speakers_intensity = speakerPlan.intensity;
              st_.speakers_pipeline_last_error = spkTerminalError;
              st_.speakers_pipeline_state = "starting";
              st_.speakers_pipeline_idle_reason.clear();
            }

            // Build the processor (Maxine/Open Audio/Passthrough), with
            // graceful fallback.
            if (wantSpkOpenAudio) {
              studiocast::open_audio::ResolvedOpenAudioModel selected;
              std::string oerr;
              auto oa = studiocast::open_audio::OpenAudioAudioProcessor::
                  CreateForSpeaker(cfg.effects, &selected, &oerr);
              if (!oa) {
                speakerOpenAudioCooldownUntil =
                    now + StartFailureRetryDelay(cfg);
                speakerOpenAudioCooldownReason = oerr;

                desiredSpkBackend = "passthrough";
                spk_processor = std::make_unique<PassthroughAudioProcessor>();
                {
                  std::lock_guard<std::mutex> lock(mu_);
                  st_.speakers_open_audio_runtime = {};
                  SetOpenAudioRuntimeModel(&st_.speakers_open_audio_runtime,
                                           selected);
                  MarkOpenAudioRuntimeDisabled(&st_.speakers_open_audio_runtime,
                                               "Open Audio init failed: " +
                                                   oerr);
                  st_.speakers_backend_active = "passthrough";
                  st_.speakers_effects_note =
                      "Open-source speaker backend failed to initialize; using "
                      "pass-through.\n" +
                      oerr;
                  st_.speakers_pipeline_last_error =
                      "Open Audio init failed: " + oerr;
                }
              } else {
                {
                  std::lock_guard<std::mutex> lock(mu_);
                  st_.speakers_open_audio_runtime =
                      MakeActiveOpenAudioRuntimeStatus(selected, *oa);
                }
                spk_processor = std::move(oa);
              }
            } else if (wantSpkMaxine) {
              // Resolve GPU selection (settings.conf) and AFX SDK paths.
              const auto settings = studiocast::config::LoadSettings();
              const auto sel = studiocast::maxine::SelectGpu(settings.gpu);
              if (!sel.selected || !sel.selected->compute_capability) {
                desiredSpkBackend = "passthrough";
                spk_processor = std::make_unique<PassthroughAudioProcessor>();
                {
                  std::lock_guard<std::mutex> lock(mu_);
                  st_.speakers_backend_active = "passthrough";
                  st_.speakers_effects_note =
                      "Failed to select a supported NVIDIA GPU for speaker "
                      "effects; using pass-through.\n" +
                      sel.error;
                  st_.speakers_pipeline_last_error =
                      "GPU selection failed: " + sel.error;
                }
              } else {
                const auto paths = studiocast::maxine::ResolveMaxinePaths();
                if (!paths.afx.ok) {
                  std::string msg = "AFX SDK not available";
                  if (!paths.afx.problems.empty()) {
                    msg += ": ";
                    msg += paths.afx.problems.front();
                  }
                  desiredSpkBackend = "passthrough";
                  spk_processor = std::make_unique<PassthroughAudioProcessor>();
                  {
                    std::lock_guard<std::mutex> lock(mu_);
                    st_.speakers_backend_active = "passthrough";
                    st_.speakers_effects_note =
                        "AFX SDK not available for speaker effects; using "
                        "pass-through.\n" +
                        msg;
                    st_.speakers_pipeline_last_error = msg;
                  }
                } else {
                  if (!spk_api || paths.afx.library != lastSpeakerAfxLib) {
                    spk_api =
                        std::make_unique<studiocast::maxine::afx::AfxApi>();
                    std::string aerr;
                    if (!spk_api->InitializeFromLibraryPath(paths.afx.library,
                                                            &aerr)) {
                      desiredSpkBackend = "passthrough";
                      spk_api.reset();
                      spk_processor =
                          std::make_unique<PassthroughAudioProcessor>();
                      {
                        std::lock_guard<std::mutex> lock(mu_);
                        st_.speakers_backend_active = "passthrough";
                        st_.speakers_effects_note =
                            "Failed to initialize AFX runtime for speaker "
                            "effects; using pass-through.\n" +
                            aerr;
                        st_.speakers_pipeline_last_error =
                            "AFX init failed: " + aerr;
                      }
                    }
                    lastSpeakerAfxLib = paths.afx.library;
                  }

                  if (!spk_processor) {
                    if (!spk_fx) {
                      spk_fx =
                          std::make_unique<studiocast::maxine::afx::AfxEffect>(
                              spk_api.get());
                    } else {
                      spk_fx->SetApi(spk_api.get());
                    }

                    studiocast::maxine::afx::AfxEffectConfig e;
                    e.effect_selector = speakerPlan.effect_selector;
                    e.feature_id = speakerPlan.feature_id;
                    e.features_dir = paths.afx.features_dir;
                    e.compute_capability = sel.selected->compute_capability;
                    e.sample_rate = 48000;
                    e.frame_samples = 480;
                    // AFX speaker effects are treated as mono voice processors.
                    // We keep the effect configured for mono and preserve
                    // stereo in the AudioProcessor wrapper via Mid/Side
                    // processing.
                    e.channels = 1;
                    e.intensity = speakerPlan.intensity;
                    e.use_denoiser_v2_model = speakerPlan.use_denoiser_v2_model;

                    std::string ferr;
                    if (!spk_fx->Configure(e, &ferr) || !spk_fx->Load(&ferr)) {
                      desiredSpkBackend = "passthrough";
                      spk_fx->Destroy();
                      spk_fx.reset();
                      spk_processor =
                          std::make_unique<PassthroughAudioProcessor>();
                      {
                        std::lock_guard<std::mutex> lock(mu_);
                        st_.speakers_backend_active = "passthrough";
                        st_.speakers_effects_note =
                            "Failed to configure/load AFX speaker effect; "
                            "using pass-through.\n" +
                            ferr;
                        st_.speakers_pipeline_last_error =
                            "AFX load failed: " + ferr;
                      }
                    } else {
                      spk_processor = std::make_unique<
                          studiocast::maxine::afx::AfxStereoAudioProcessor>(
                          spk_fx.get());
                    }
                  }
                }
              }
            } else {
              spk_processor = std::make_unique<PassthroughAudioProcessor>();
            }

            // Start pipeline (even in pass-through mode; this replaces
            // module-loopback when speaker effects are enabled).
            spk_pipeline = CreatePipeline(spk_processor.get());
            std::string perr;
            bool spkStartOk = false;
            if (!spk_pipeline) {
              perr = "speaker audio pipeline factory returned null";
            } else {
              studiocast::audio::AudioPipelineConfig pcfg;
              pcfg.source_name =
                  studiocast::audio::VirtualSpeakerMonitorSourceName();
              pcfg.allow_monitor_source = true;
              pcfg.sink_name = sinkName;
              // Speaker processing should preserve stereo.
              pcfg.channels = 2;
              spkStartOk = spk_pipeline->Start(pcfg, &perr);
              if (spkStartOk) {
                lastSpeakerBackend = desiredSpkBackend;
                lastSpeakerTargetSink = sinkName;
                lastSpeakerFx = cfg.effects.speaker;

                {
                  std::lock_guard<std::mutex> lock(mu_);
                  st_.speakers_pipeline_starting = false;
                  st_.speakers_pipeline_running = true;
                  st_.speakers_routing_active = true;
                  st_.speakers_backend_active = desiredSpkBackend;
                  st_.speaker_target_sink_active = sinkName;
                  st_.speakers_pipeline_state = "running";
                  st_.speakers_pipeline_idle_reason.clear();
                  // Preserve st_.speakers_effects_note (decision/fallback
                  // message).
                  if (!speakerRestartingAfterTerminalFailure) {
                    st_.speakers_pipeline_last_error.clear();
                  }
                }
                speakerRestartingAfterTerminalFailure = false;
              }
            }

            if (!spkStartOk) {
              if (perr.empty()) {
                perr = "speaker audio pipeline failed to start";
              }
              {
                std::lock_guard<std::mutex> lock(mu_);
                st_.speakers_pipeline_starting = false;
                st_.speakers_pipeline_running = false;
                st_.speakers_routing_active = false;
                st_.speaker_target_sink_active.clear();
                st_.speakers_pipeline_frames_processed = 0;
                st_.speakers_pipeline_process_time_us_sum = 0;
                st_.speakers_pipeline_process_time_us_max = 0;
                st_.speakers_pipeline_process_time_us_last = 0;
                st_.speakers_pipeline_process_overruns = 0;
                st_.speakers_pipeline_pulse_capture_latency_us_last = 0;
                st_.speakers_pipeline_pulse_playback_latency_us_last = 0;
                st_.speakers_pipeline_pulse_latency_us_max = 0;
                st_.speakers_pipeline_resync_events = 0;
                st_.speakers_pipeline_last_error =
                    "Failed to start speaker pipeline: " + perr;
                if (st_.speakers_open_audio_runtime.active)
                  st_.speakers_open_audio_runtime.active = false;
                st_.speakers_pipeline_state = "failed";
                st_.speakers_pipeline_idle_reason.clear();
              }

              spk_pipeline.reset();
              spk_processor.reset();
              nextSpeakerStartRetry = now + StartFailureRetryDelay(cfg);
            }
          } else if (wantSpkOpenAudio) {
            if (auto *oa = dynamic_cast<
                    studiocast::open_audio::OpenAudioAudioProcessor *>(
                    spk_processor.get())) {
              oa->UpdateFromSpeakerConfig(cfg.effects.speaker);
              lastSpeakerFx = cfg.effects.speaker;
              std::lock_guard<std::mutex> lock(mu_);
              st_.speakers_intensity = speakerPlan.intensity;
            }
          }

          if (spk_pipeline) {
            const auto stats = spk_pipeline->GetStats();
            {
              std::lock_guard<std::mutex> lock(mu_);
              st_.speakers_pipeline_running = stats.running;
              st_.speakers_routing_active = stats.running;
              if (stats.running) {
                st_.speaker_target_sink_active = sinkName;
              } else {
                st_.speaker_target_sink_active.clear();
              }
              st_.speakers_pipeline_frames_processed = stats.frames_processed;
              st_.speakers_pipeline_process_time_us_sum =
                  stats.process_time_us_sum;
              st_.speakers_pipeline_process_time_us_max =
                  stats.process_time_us_max;
              st_.speakers_pipeline_process_time_us_last =
                  stats.process_time_us_last;
              st_.speakers_pipeline_process_overruns = stats.process_overruns;
              st_.speakers_pipeline_pulse_capture_latency_us_last =
                  stats.pulse_capture_latency_us_last;
              st_.speakers_pipeline_pulse_playback_latency_us_last =
                  stats.pulse_playback_latency_us_last;
              st_.speakers_pipeline_pulse_latency_us_max =
                  stats.pulse_latency_us_max;
              st_.speakers_pipeline_resync_events = stats.resync_events;
              if (!stats.last_error.empty()) {
                st_.speakers_pipeline_last_error = stats.last_error;
                ApplyOpenAudioRuntimeWarning(&st_.speakers_open_audio_runtime,
                                             stats.last_error);
              }
              st_.speakers_pipeline_state =
                  stats.running ? "running" : "failed";
              if (stats.running)
                st_.speakers_pipeline_idle_reason.clear();
            }
          }
        }
      }
    }
#endif

    if (!micPipelineNeeded) {
#if STUDIOCAST_HAVE_PULSE_SIMPLE
      if (pipeline) {
        pipeline->Stop();
        pipeline.reset();
      }
      processor.reset();
      if (fx) {
        fx->Destroy();
        fx.reset();
      }
      api.reset();
      lastFx.reset();
      lastSource.clear();
      lastBackend.clear();
      lastAfxLib.clear();
      micRestartingAfterTerminalFailure = false;
#endif
      {
        std::lock_guard<std::mutex> lock(mu_);
        st_.pipeline_running = false;
        st_.pipeline_starting = false;
        st_.pipeline_frames_processed = 0;
        st_.pipeline_process_time_us_sum = 0;
        st_.pipeline_process_time_us_max = 0;
        st_.pipeline_process_time_us_last = 0;
        st_.pipeline_process_overruns = 0;
        st_.pipeline_pulse_capture_latency_us_last = 0;
        st_.pipeline_pulse_playback_latency_us_last = 0;
        st_.pipeline_pulse_latency_us_max = 0;
        st_.pipeline_resync_events = 0;
        st_.effect_selector.clear();
        st_.feature_id.clear();
        st_.intensity = 0.0f;
        st_.effects_backend_active.clear();
        st_.effects_note.clear();
        st_.open_audio_runtime = {};
        st_.pipeline_active_needed = false;
        st_.pipeline_state = cfg.enabled ? "idle_no_consumer" : "disabled";
        st_.pipeline_idle_reason =
            cfg.enabled
                ? (micConsumers.error.empty()
                       ? "No active virtual microphone consumer."
                       : "Virtual microphone consumer detection unavailable: " +
                             micConsumers.error)
                : "Audio processing disabled.";
      }
      SleepFor(milliseconds(pollMs));
      continue;
    }

    if (!micSourceStatus.error.empty()) {
#if STUDIOCAST_HAVE_PULSE_SIMPLE
      if (pipeline) {
        pipeline->Stop();
        pipeline.reset();
      }
      processor.reset();
      if (fx) {
        fx->Destroy();
        fx.reset();
      }
      api.reset();
      lastFx.reset();
      lastSource.clear();
      lastBackend.clear();
      lastAfxLib.clear();
      micRestartingAfterTerminalFailure = false;
#endif
      SetLastError(micSourceStatus.error);
      {
        std::lock_guard<std::mutex> lock(mu_);
        st_.pipeline_running = false;
        st_.pipeline_starting = false;
        st_.pipeline_state = "failed";
        st_.pipeline_idle_reason.clear();
        st_.pipeline_frames_processed = 0;
        st_.pipeline_process_time_us_sum = 0;
        st_.pipeline_process_time_us_max = 0;
        st_.pipeline_process_time_us_last = 0;
        st_.pipeline_process_overruns = 0;
        st_.pipeline_pulse_capture_latency_us_last = 0;
        st_.pipeline_pulse_playback_latency_us_last = 0;
        st_.pipeline_pulse_latency_us_max = 0;
        st_.pipeline_resync_events = 0;
        st_.effect_selector.clear();
        st_.feature_id.clear();
        st_.intensity = 0.0f;
        st_.effects_backend_active.clear();
        st_.effects_note.clear();
        st_.open_audio_runtime = {};
      }
      SleepFor(milliseconds(pollMs));
      continue;
    }

    auto plan = studiocast::maxine::afx::PlanBroadcastMicrophoneEffect(
        cfg.effects.microphone.studio_voice_enabled,
        cfg.effects.microphone.noise_removal_enabled,
        cfg.effects.microphone.room_echo_removal_enabled,
        cfg.effects.microphone.strength);

    if (!plan.enabled) {
      // Planner maps strength -> intensity even when disabled; normalize for
      // pass-through status.
      plan.intensity = 0.0f;
    }

    {
      std::lock_guard<std::mutex> lock(mu_);
      st_.effect_selector = plan.effect_selector;
      st_.feature_id = plan.feature_id;
      st_.intensity = plan.intensity;
    }

    // Backend selection.
    AudioBackendAvailability avail;
    bool micOpenAudioCooldownActive = false;
    if (AnyMicrophoneEffectRequested(cfg.effects)) {
      const auto now2 = steady_clock::now();
      const auto micAvailabilityKey =
          MakeMicrophoneAvailabilityCacheKey(cfg.effects);
      const bool micAvailabilityExpired =
          !cachedMicAvailability || !cachedMicAvailabilityKey.has_value() ||
          *cachedMicAvailabilityKey != micAvailabilityKey ||
          now2 >= nextMicAvailabilityProbe;
      if (micAvailabilityExpired) {
        cachedMicAvailability = ProbeMicrophoneBackendAvailability(cfg);
        cachedMicAvailabilityKey = micAvailabilityKey;
        nextMicAvailabilityProbe = now2 + availabilityProbeTtl;
      }
      avail = *cachedMicAvailability;
      if (now2 < openAudioCooldownUntil) {
        micOpenAudioCooldownActive = true;
        avail.open_source_ok = false;
        avail.open_source_reason = openAudioCooldownReason.empty()
                                       ? "Open Audio backend is temporarily "
                                         "disabled due to a previous failure."
                                       : openAudioCooldownReason;
      }
      if (now2 < maxineCooldownUntil) {
        avail.maxine_ok = false;
        avail.maxine_reason = maxineCooldownReason.empty()
                                  ? "Maxine backend is temporarily disabled "
                                    "due to a previous failure."
                                  : maxineCooldownReason;
      }
    }
    const auto decision = ResolveAudioBackend(cfg.effects, avail);
    {
      std::lock_guard<std::mutex> lock(mu_);
      st_.effects_backend_active = std::string(ToString(decision.backend));
      st_.effects_note = decision.note;
      if (micOpenAudioCooldownActive) {
        MarkOpenAudioRuntimeDisabled(&st_.open_audio_runtime,
                                     avail.open_source_reason);
      }
    }

#if !STUDIOCAST_HAVE_PULSE_SIMPLE
    SetLastError(
        "Audio pipeline disabled at build time (libpulse-simple not found)");
    {
      std::lock_guard<std::mutex> lock(mu_);
      st_.pipeline_running = false;
      st_.pipeline_starting = false;
      st_.pipeline_state = "failed";
      st_.pipeline_idle_reason.clear();
    }
    SleepFor(milliseconds(pollMs));
    continue;
#else

    const auto now = steady_clock::now();
    if (now < nextStartRetry) {
      if (pipeline) {
        const auto stats = pipeline->GetStats();
        if (!stats.last_error.empty()) {
          SetLastError(stats.last_error);
        }
        {
          std::lock_guard<std::mutex> lock(mu_);
          st_.pipeline_running = stats.running;
          st_.pipeline_frames_processed = stats.frames_processed;
          st_.pipeline_process_time_us_sum = stats.process_time_us_sum;
          st_.pipeline_process_time_us_max = stats.process_time_us_max;
          st_.pipeline_process_time_us_last = stats.process_time_us_last;
          st_.pipeline_process_overruns = stats.process_overruns;
          st_.pipeline_pulse_capture_latency_us_last =
              stats.pulse_capture_latency_us_last;
          st_.pipeline_pulse_playback_latency_us_last =
              stats.pulse_playback_latency_us_last;
          st_.pipeline_pulse_latency_us_max = stats.pulse_latency_us_max;
          st_.pipeline_resync_events = stats.resync_events;
          ApplyOpenAudioRuntimeWarning(&st_.open_audio_runtime,
                                       stats.last_error);
          st_.pipeline_state = stats.running ? "running" : "failed";
          if (!stats.running) {
            st_.pipeline_starting = false;
          }
          if (stats.running)
            st_.pipeline_idle_reason.clear();
        }
      } else {
        std::lock_guard<std::mutex> lock(mu_);
        st_.pipeline_running = false;
        st_.pipeline_starting = false;
        st_.pipeline_state = "failed";
        st_.pipeline_idle_reason.clear();
      }
      SleepFor(milliseconds(pollMs));
      continue;
    }

    const bool wantMaxine =
        (decision.backend == AudioBackendKind::kMaxine) && plan.enabled;
    const bool wantOpenAudio =
        (decision.backend == AudioBackendKind::kOpenSource) && plan.enabled;

    std::string desiredBackend = "passthrough";
    if (wantMaxine) {
      desiredBackend = "maxine";
    } else if (wantOpenAudio) {
      desiredBackend = "open_source";
    }
    if (!wantOpenAudio && !micOpenAudioCooldownActive) {
      std::lock_guard<std::mutex> lock(mu_);
      st_.open_audio_runtime = {};
    }

    const bool micRestartKeyChanged =
        (!lastFx.has_value() ||
         MicrophonePipelineEffectsRequireRestart(*lastFx, cfg.effects));
    const bool micStrengthChanged =
        lastFx.has_value() &&
        lastFx->microphone.strength != cfg.effects.microphone.strength;
    const bool effectsChanged =
        micRestartKeyChanged || (wantMaxine && micStrengthChanged);
    std::optional<AudioPipelineStats> statsBeforeRestart;
    if (pipeline) {
      statsBeforeRestart = pipeline->GetStats();
    }
    const bool pipelineDead =
        (statsBeforeRestart && !statsBeforeRestart->running);
    std::string terminalError;
    if (pipelineDead) {
      terminalError = statsBeforeRestart->last_error.empty()
                          ? "Audio pipeline stopped."
                          : statsBeforeRestart->last_error;
      SetLastError(terminalError);
    }
    if (pipelineDead) {
      if (pipeline) {
        pipeline->Stop();
        pipeline.reset();
      }
      processor.reset();
      if (fx) {
        fx->Destroy();
        fx.reset();
      }
      if (!wantMaxine) {
        api.reset();
        lastAfxLib.clear();
      }
      lastFx.reset();
      lastSource.clear();
      lastBackend.clear();
      micRestartingAfterTerminalFailure = true;
      nextStartRetry = now + WorkerDeathRetryDelay(cfg);
      {
        std::lock_guard<std::mutex> lock(mu_);
        st_.pipeline_running = false;
        st_.pipeline_starting = false;
        st_.pipeline_state = "failed";
        st_.pipeline_idle_reason.clear();
        ApplyOpenAudioRuntimeWarning(&st_.open_audio_runtime, terminalError);
        if (st_.open_audio_runtime.active)
          st_.open_audio_runtime.active = false;
        clearMicPipelineStatsLocked();
      }
      SleepFor(milliseconds(pollMs));
      continue;
    }

    const std::string micPipelineSource = micSourceStatus.selected_source;
    const bool needRestart = (!pipeline) || (lastBackend != desiredBackend) ||
                             (lastSource != micPipelineSource) ||
                             ((wantMaxine || wantOpenAudio) && effectsChanged);

    if (needRestart) {
      if (pipeline) {
        pipeline->Stop();
        pipeline.reset();
      }
      processor.reset();
      if (fx) {
        fx->Destroy();
        fx.reset();
      }
      if (!wantMaxine) {
        // Pass-through mode does not need the AFX runtime.
        api.reset();
        lastAfxLib.clear();
        lastFx.reset();
      }
    }

    auto startMicPassthroughFallback =
        [&](const std::string &note,
            const std::string &failure_reason) -> bool {
      maxineCooldownUntil = steady_clock::now() + StartFailureRetryDelay(cfg);
      maxineCooldownReason = failure_reason;
      SetLastError(failure_reason);

      if (pipeline) {
        pipeline->Stop();
        pipeline.reset();
      }
      processor.reset();
      if (fx) {
        fx->Destroy();
        fx.reset();
      }
      api.reset();
      lastAfxLib.clear();
      lastFx.reset();
      lastSource.clear();
      lastBackend.clear();

      {
        std::lock_guard<std::mutex> lock(mu_);
        st_.pipeline_starting = true;
        st_.pipeline_running = false;
        st_.pipeline_state = "starting";
        st_.pipeline_idle_reason.clear();
        st_.effects_backend_active = "passthrough";
        st_.effects_note = note;
        st_.open_audio_runtime = {};
        clearMicPipelineStatsLocked();
      }

      processor = std::make_unique<PassthroughAudioProcessor>();
      pipeline = CreatePipeline(processor.get());

      studiocast::audio::AudioPipelineConfig pcfg;
      pcfg.source_name = micPipelineSource;
      pcfg.sink_name = "studiocast_sink";

      std::string perr;
      bool pipelineStartOk = false;
      if (!pipeline) {
        perr = "audio pipeline factory returned null";
      } else {
        pipelineStartOk = pipeline->Start(pcfg, &perr);
      }
      if (!pipelineStartOk) {
        if (perr.empty()) {
          perr = "audio pipeline failed to start";
        }
        SetLastError("Failed to start audio pipeline: " + perr);
        pipeline.reset();
        processor.reset();
        nextStartRetry = steady_clock::now() + StartFailureRetryDelay(cfg);
        {
          std::lock_guard<std::mutex> lock(mu_);
          st_.pipeline_starting = false;
          st_.pipeline_running = false;
          st_.pipeline_state = "failed";
          st_.pipeline_idle_reason.clear();
          clearMicPipelineStatsLocked();
        }
        return false;
      }

      lastBackend = "passthrough";
      lastSource = micPipelineSource;
      lastFx = cfg.effects;
      {
        std::lock_guard<std::mutex> lock(mu_);
        st_.pipeline_starting = false;
        st_.pipeline_running = true;
        st_.pipeline_state = "running";
        st_.pipeline_idle_reason.clear();
        st_.last_error.clear();
      }
      micRestartingAfterTerminalFailure = false;
      return true;
    };

    if (wantOpenAudio) {
      // Open-source backend (Phase 4 stub): validate model selection and keep
      // the pipeline alive.
      if (needRestart) {
        {
          std::lock_guard<std::mutex> lock(mu_);
          st_.pipeline_starting = true;
          st_.pipeline_running = false;
          st_.pipeline_state = "starting";
          st_.pipeline_idle_reason.clear();

          // Open-source backend does not require Maxine GPU selection.
          st_.gpu_index = -1;
          st_.gpu_name.clear();
          st_.gpu_compute_cap.clear();
        }

        studiocast::open_audio::ResolvedOpenAudioModel selected;
        std::string oerr;
        auto oa = studiocast::open_audio::OpenAudioAudioProcessor::
            CreateForMicrophone(cfg.effects, &selected, &oerr);
        if (!oa) {
          // Fall back to pass-through with a cooldown to avoid restart loops.
          SetLastError("Open Audio initialization failed: " + oerr);
          openAudioCooldownUntil = now + StartFailureRetryDelay(cfg);
          openAudioCooldownReason = oerr;

          desiredBackend = "passthrough";
          processor = std::make_unique<PassthroughAudioProcessor>();

          // Update status to reflect actual backend (decision may still say
          // open_source this tick).
          {
            std::lock_guard<std::mutex> lock(mu_);
            st_.open_audio_runtime = {};
            SetOpenAudioRuntimeModel(&st_.open_audio_runtime, selected);
            MarkOpenAudioRuntimeDisabled(&st_.open_audio_runtime,
                                         "Open Audio init failed: " + oerr);
            st_.effects_backend_active = "passthrough";
            st_.effects_note = "Open-source audio backend failed to "
                               "initialize; using pass-through.\n" +
                               oerr;
          }
        } else {
          {
            std::lock_guard<std::mutex> lock(mu_);
            st_.open_audio_runtime =
                MakeActiveOpenAudioRuntimeStatus(selected, *oa);
          }
          processor = std::move(oa);
        }

        pipeline = CreatePipeline(processor.get());

        studiocast::audio::AudioPipelineConfig pcfg;
        pcfg.source_name = micPipelineSource;
        pcfg.sink_name = "studiocast_sink";

        std::string perr;
        bool pipelineStartOk = false;
        if (!pipeline) {
          perr = "audio pipeline factory returned null";
        } else {
          pipelineStartOk = pipeline->Start(pcfg, &perr);
        }
        if (!pipelineStartOk) {
          if (perr.empty()) {
            perr = "audio pipeline failed to start";
          }
          SetLastError("Failed to start audio pipeline: " + perr);
          pipeline.reset();
          processor.reset();
          nextStartRetry = now + StartFailureRetryDelay(cfg);
          {
            std::lock_guard<std::mutex> lock(mu_);
            st_.pipeline_starting = false;
            st_.pipeline_running = false;
            st_.pipeline_state = "failed";
            st_.pipeline_idle_reason.clear();
            if (st_.open_audio_runtime.active)
              st_.open_audio_runtime.active = false;
            clearMicPipelineStatsLocked();
          }
          SleepFor(milliseconds(pollMs));
          continue;
        }

        lastBackend = desiredBackend;
        lastSource = micPipelineSource;
        lastFx = cfg.effects;

        {
          std::lock_guard<std::mutex> lock(mu_);
          st_.pipeline_starting = false;
          st_.pipeline_running = true;
          st_.pipeline_state = "running";
          st_.pipeline_idle_reason.clear();
          if (!micRestartingAfterTerminalFailure) {
            st_.last_error.clear();
          }
        }
        micRestartingAfterTerminalFailure = false;
      } else if (auto *oa = dynamic_cast<
                     studiocast::open_audio::OpenAudioAudioProcessor *>(
                     processor.get())) {
        oa->UpdateFromMicrophoneConfig(cfg.effects.microphone);
        lastFx = cfg.effects;
      }

      if (pipeline) {
        const auto stats = pipeline->GetStats();
        if (!stats.last_error.empty()) {
          SetLastError(stats.last_error);
        }
        {
          std::lock_guard<std::mutex> lock(mu_);
          st_.pipeline_running = stats.running;
          st_.pipeline_frames_processed = stats.frames_processed;
          st_.pipeline_process_time_us_sum = stats.process_time_us_sum;
          st_.pipeline_process_time_us_max = stats.process_time_us_max;
          st_.pipeline_process_time_us_last = stats.process_time_us_last;
          st_.pipeline_process_overruns = stats.process_overruns;
          st_.pipeline_pulse_capture_latency_us_last =
              stats.pulse_capture_latency_us_last;
          st_.pipeline_pulse_playback_latency_us_last =
              stats.pulse_playback_latency_us_last;
          st_.pipeline_pulse_latency_us_max = stats.pulse_latency_us_max;
          st_.pipeline_resync_events = stats.resync_events;
          ApplyOpenAudioRuntimeWarning(&st_.open_audio_runtime,
                                       stats.last_error);
          st_.pipeline_state = stats.running ? "running" : "failed";
          if (stats.running)
            st_.pipeline_idle_reason.clear();
        }
      }

      SleepFor(milliseconds(pollMs));
      continue;
    }

    if (!wantMaxine) {
      // Effects are disabled: keep the pipeline alive with pass-through audio.
      if (needRestart) {
        {
          std::lock_guard<std::mutex> lock(mu_);
          st_.pipeline_starting = true;
          st_.pipeline_running = false;
          st_.pipeline_state = "starting";
          st_.pipeline_idle_reason.clear();

          // No GPU requirement in pass-through mode.
          st_.gpu_index = -1;
          st_.gpu_name.clear();
          st_.gpu_compute_cap.clear();
        }

        processor = std::make_unique<PassthroughAudioProcessor>();
        pipeline = CreatePipeline(processor.get());

        studiocast::audio::AudioPipelineConfig pcfg;
        pcfg.source_name = micPipelineSource;
        pcfg.sink_name = "studiocast_sink";

        std::string perr;
        bool pipelineStartOk = false;
        if (!pipeline) {
          perr = "audio pipeline factory returned null";
        } else {
          pipelineStartOk = pipeline->Start(pcfg, &perr);
        }
        if (!pipelineStartOk) {
          if (perr.empty()) {
            perr = "audio pipeline failed to start";
          }
          SetLastError("Failed to start audio pipeline: " + perr);
          pipeline.reset();
          processor.reset();
          nextStartRetry = now + StartFailureRetryDelay(cfg);
          {
            std::lock_guard<std::mutex> lock(mu_);
            st_.pipeline_starting = false;
            st_.pipeline_running = false;
            st_.pipeline_state = "failed";
            st_.pipeline_idle_reason.clear();
            clearMicPipelineStatsLocked();
          }
          SleepFor(milliseconds(pollMs));
          continue;
        }

        lastBackend = desiredBackend;
        lastSource = micPipelineSource;

        {
          std::lock_guard<std::mutex> lock(mu_);
          st_.pipeline_starting = false;
          st_.pipeline_running = true;
          st_.pipeline_state = "running";
          st_.pipeline_idle_reason.clear();
          if (!micRestartingAfterTerminalFailure) {
            st_.last_error.clear();
          }
        }
        micRestartingAfterTerminalFailure = false;
      }

      if (pipeline) {
        const auto stats = pipeline->GetStats();
        if (!stats.last_error.empty()) {
          SetLastError(stats.last_error);
        }
        {
          std::lock_guard<std::mutex> lock(mu_);
          st_.pipeline_running = stats.running;
          st_.pipeline_frames_processed = stats.frames_processed;
          st_.pipeline_process_time_us_sum = stats.process_time_us_sum;
          st_.pipeline_process_time_us_max = stats.process_time_us_max;
          st_.pipeline_process_time_us_last = stats.process_time_us_last;
          st_.pipeline_process_overruns = stats.process_overruns;
          st_.pipeline_pulse_capture_latency_us_last =
              stats.pulse_capture_latency_us_last;
          st_.pipeline_pulse_playback_latency_us_last =
              stats.pulse_playback_latency_us_last;
          st_.pipeline_pulse_latency_us_max = stats.pulse_latency_us_max;
          st_.pipeline_resync_events = stats.resync_events;
          st_.pipeline_state = stats.running ? "running" : "failed";
          if (stats.running)
            st_.pipeline_idle_reason.clear();
        }
      }

      SleepFor(milliseconds(pollMs));
      continue;
    }

    // Resolve GPU selection (settings.conf) and AFX SDK paths.
    const auto settings = studiocast::config::LoadSettings();
    const auto sel = studiocast::maxine::SelectGpu(settings.gpu);
    {
      std::lock_guard<std::mutex> lock(mu_);
      st_.gpu_index = sel.selected ? sel.selected->index : -1;
      st_.gpu_name = sel.selected ? sel.selected->name : std::string();
      st_.gpu_compute_cap = (sel.selected && sel.selected->compute_capability)
                                ? sel.selected->ComputeCapString()
                                : std::string();
    }
    if (!sel.selected || !sel.selected->compute_capability) {
      const std::string reason =
          "Failed to select a supported NVIDIA GPU: " + sel.error;
      (void)startMicPassthroughFallback(
          "Failed to select a supported NVIDIA GPU for microphone effects; "
          "using pass-through.\n" +
              sel.error,
          reason);
      SleepFor(milliseconds(pollMs));
      continue;
    }

    const auto paths = studiocast::maxine::ResolveMaxinePaths();
    if (!paths.afx.ok) {
      std::string msg = "AFX SDK not available";
      if (!paths.afx.problems.empty()) {
        msg += ": ";
        msg += paths.afx.problems.front();
      }
      (void)startMicPassthroughFallback(
          "AFX SDK not available for microphone effects; using "
          "pass-through.\n" +
              msg,
          msg);
      SleepFor(milliseconds(pollMs));
      continue;
    }

    if (!api || paths.afx.library != lastAfxLib) {
      api = std::make_unique<studiocast::maxine::afx::AfxApi>();
      std::string aerr;
      if (!api->InitializeFromLibraryPath(paths.afx.library, &aerr)) {
        (void)startMicPassthroughFallback(
            "Failed to initialize AFX runtime for microphone effects; using "
            "pass-through.\n" +
                aerr,
            "Failed to initialize AFX runtime: " + aerr);
        SleepFor(milliseconds(pollMs));
        continue;
      }
      lastAfxLib = paths.afx.library;
    }

    if (!fx) {
      fx = std::make_unique<studiocast::maxine::afx::AfxEffect>(api.get());
    } else {
      fx->SetApi(api.get());
    }

    if (needRestart) {
      studiocast::maxine::afx::AfxEffectConfig e;
      e.effect_selector = plan.effect_selector;
      e.feature_id = plan.feature_id;
      e.features_dir = paths.afx.features_dir;
      e.compute_capability = sel.selected->compute_capability;
      e.sample_rate = 48000;
      e.frame_samples = 480;
      e.channels = 1;
      e.intensity = plan.intensity;
      e.use_denoiser_v2_model = plan.use_denoiser_v2_model;

      {
        std::lock_guard<std::mutex> lock(mu_);
        st_.pipeline_starting = true;
        st_.pipeline_running = false;
        st_.pipeline_state = "starting";
        st_.pipeline_idle_reason.clear();
      }

      std::string ferr;
      if (!fx->Configure(e, &ferr)) {
        (void)startMicPassthroughFallback(
            "Failed to configure AFX microphone effect; using "
            "pass-through.\n" +
                ferr,
            "Failed to configure AFX effect: " + ferr);
        SleepFor(milliseconds(pollMs));
        continue;
      }
      if (!fx->Load(&ferr)) {
        (void)startMicPassthroughFallback(
            "Failed to load AFX microphone effect; using pass-through.\n" +
                ferr,
            "Failed to load AFX effect: " + ferr);
        SleepFor(milliseconds(pollMs));
        continue;
      }

      processor = std::make_unique<studiocast::maxine::afx::AfxAudioProcessor>(
          fx.get());
      pipeline = CreatePipeline(processor.get());
      studiocast::audio::AudioPipelineConfig pcfg;
      pcfg.source_name = micPipelineSource;
      pcfg.sink_name = "studiocast_sink";

      std::string perr;
      bool pipelineStartOk = false;
      if (!pipeline) {
        perr = "audio pipeline factory returned null";
      } else {
        pipelineStartOk = pipeline->Start(pcfg, &perr);
      }
      if (!pipelineStartOk) {
        if (perr.empty()) {
          perr = "audio pipeline failed to start";
        }
        SetLastError("Failed to start audio pipeline: " + perr);
        pipeline.reset();
        processor.reset();
        nextStartRetry = now + StartFailureRetryDelay(cfg);
        {
          std::lock_guard<std::mutex> lock(mu_);
          st_.pipeline_starting = false;
          st_.pipeline_running = false;
          st_.pipeline_state = "failed";
          st_.pipeline_idle_reason.clear();
          clearMicPipelineStatsLocked();
        }
        SleepFor(milliseconds(pollMs));
        continue;
      }

      lastFx = cfg.effects;
      lastSource = micPipelineSource;
      lastBackend = desiredBackend;
      {
        std::lock_guard<std::mutex> lock(mu_);
        st_.pipeline_starting = false;
        st_.pipeline_running = true;
        st_.pipeline_state = "running";
        st_.pipeline_idle_reason.clear();
        if (!micRestartingAfterTerminalFailure) {
          st_.last_error.clear();
        }
      }
      micRestartingAfterTerminalFailure = false;
    }

    if (pipeline) {
      const auto stats = pipeline->GetStats();
      if (!stats.last_error.empty()) {
        SetLastError(stats.last_error);
      }
      {
        std::lock_guard<std::mutex> lock(mu_);
        st_.pipeline_running = stats.running;
        st_.pipeline_frames_processed = stats.frames_processed;
        st_.pipeline_process_time_us_sum = stats.process_time_us_sum;
        st_.pipeline_process_time_us_max = stats.process_time_us_max;
        st_.pipeline_process_time_us_last = stats.process_time_us_last;
        st_.pipeline_process_overruns = stats.process_overruns;
        st_.pipeline_pulse_capture_latency_us_last =
            stats.pulse_capture_latency_us_last;
        st_.pipeline_pulse_playback_latency_us_last =
            stats.pulse_playback_latency_us_last;
        st_.pipeline_pulse_latency_us_max = stats.pulse_latency_us_max;
        st_.pipeline_resync_events = stats.resync_events;
        st_.pipeline_state = stats.running ? "running" : "failed";
        if (stats.running)
          st_.pipeline_idle_reason.clear();
      }
    }

    SleepFor(milliseconds(pollMs));
#endif
  }

  // The monitor is a listening aid, not a route other apps depend on. Stop it
  // with the service so no loopback outlives the daemon session. A failed
  // check earlier can have cleared `monitor_running_` while the module was
  // still loaded, so the stop follows what may exist as well.
  if (monitor_running_ || monitor_route_may_exist_) {
    std::string err;
    if (StopMicMonitorRoute(&err)) {
      monitor_route_may_exist_ = false;
      monitor_running_ = false;
      monitor_sink_requested_.clear();
      monitor_sink_resolved_.clear();
      monitor_module_id_ = -1;
      monitor_latency_ms_ = 0;
      monitor_volume_ = 0;
      monitor_output_lost_ = false;
      std::lock_guard<std::mutex> lock(mu_);
      st_.monitor_active = false;
      st_.monitor_module_id = -1;
      st_.monitor_sink_active.clear();
      st_.monitor_latency_ms_active = 0;
    } else {
      std::lock_guard<std::mutex> lock(mu_);
      st_.monitor_last_error = "Failed to stop the microphone monitor: " + err;
    }
  }

#if STUDIOCAST_HAVE_PULSE_SIMPLE
  if (pipeline)
    pipeline->Stop();
  processor.reset();
  if (fx)
    fx->Destroy();

  if (spk_pipeline)
    spk_pipeline->Stop();
  spk_processor.reset();
  if (spk_fx)
    spk_fx->Destroy();
#endif
}

} // namespace studiocast::audio
