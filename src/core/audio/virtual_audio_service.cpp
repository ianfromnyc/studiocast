#include "core/audio/virtual_audio_service.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <memory>
#include <optional>
#include <thread>

#include "core/audio/audio_backend_resolver.h"
#include "core/audio/pulse/pactl.h"
#include "core/audio/virtual_mic.h"
#include "core/audio/virtual_speaker.h"
#include "core/config/settings.h"
#include "core/maxine/availability.h"
#include "core/maxine/gpu_selection.h"
#include "core/maxine/paths.h"
#include "core/open_audio/open_audio_audio_processor.h"
#include "core/util/strings.h"

// Effect planning is build-time independent from the Pulse audio pipeline.
#include "core/maxine/afx/afx_effect.h"

#if STUDIOCAST_HAVE_PULSE_SIMPLE
#include "core/audio/audio_pipeline.h"
#include "core/audio/audio_processor.h"
#include "core/maxine/afx/afx_audio_processor.h"
#include "core/maxine/afx/afx_stereo_audio_processor.h"
#include "core/maxine/afx_api.h"
#endif

namespace studiocast::audio {

namespace {

constexpr const char *kVirtualMicSinkName = "studiocast_sink";
constexpr const char *kVirtualSpeakersSinkName = "studiocast_speakers";

bool IsVirtualSinkName(const std::string &name) {
  return name == kVirtualMicSinkName || name == kVirtualSpeakersSinkName;
}

std::optional<std::string>
ChooseSpeakerTargetSinkName(const std::string &configured_target,
                            std::string *error) {
  if (error)
    error->clear();

  std::string chosen = studiocast::util::TrimCopy(configured_target);
  std::string err;
  if (chosen.empty()) {
    // Prefer default sink, unless it's one of our virtual sinks.
    auto def = pulse::GetDefaultSinkName(&err);
    if (def && !IsVirtualSinkName(*def)) {
      chosen = *def;
    } else {
      // If the user's default sink is our virtual device (common when testing),
      // pick the first non-virtual sink as a best-effort physical target.
      const auto sinks = pulse::ListSinks(&err);
      for (const auto &s : sinks) {
        if (!IsVirtualSinkName(s.name)) {
          chosen = s.name;
          break;
        }
      }
      if (chosen.empty()) {
        if (error) {
          *error = "Failed to choose a target sink. Default sink is virtual or "
                   "missing.";
          if (!err.empty())
            *error += " (note) " + err;
        }
        return std::nullopt;
      }
    }
  }

  if (IsVirtualSinkName(chosen)) {
    if (error)
      *error =
          "Refusing to route speakers to '" + chosen + "' (feedback loop).";
    return std::nullopt;
  }

  return chosen;
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
    st_.speakers_routing_active = false;
    st_.speakers_route_mode.clear();
    st_.speakers_pipeline_running = false;
    st_.speakers_pipeline_starting = false;
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
    st_.speakers_routing_active = false;
    st_.speakers_route_mode.clear();
    st_.speakers_pipeline_running = false;
    st_.speakers_pipeline_starting = false;
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

void VirtualAudioService::SetLastError(std::string msg) {
  std::lock_guard<std::mutex> lock(mu_);
  st_.last_error = std::move(msg);
}

void VirtualAudioService::ThreadMain() {
  using namespace std::chrono;

  steady_clock::time_point nextStartRetry{};
  steady_clock::time_point nextSpeakerStartRetry{};

  // If the Open Audio backend fails to initialize, latch-disable it for a short
  // cooldown to avoid rapid restart loops.
  steady_clock::time_point openAudioCooldownUntil{};
  std::string openAudioCooldownReason;

  // Separate cooldown for speaker processing, so a failure in one direction
  // doesn't permanently disable the other.
  steady_clock::time_point speakerOpenAudioCooldownUntil{};
  std::string speakerOpenAudioCooldownReason;

#if STUDIOCAST_HAVE_PULSE_SIMPLE
  std::unique_ptr<studiocast::maxine::afx::AfxApi> api;
  std::unique_ptr<studiocast::maxine::afx::AfxEffect> fx;
  std::unique_ptr<AudioProcessor> processor;
  std::unique_ptr<studiocast::audio::AudioPipeline> pipeline;

  // Independent speaker processing pipeline (virtual speakers -> physical
  // sink).
  std::unique_ptr<studiocast::maxine::afx::AfxApi> spk_api;
  std::unique_ptr<studiocast::maxine::afx::AfxEffect> spk_fx;
  std::unique_ptr<AudioProcessor> spk_processor;
  std::unique_ptr<studiocast::audio::AudioPipeline> spk_pipeline;

  std::string lastBackend;
  std::optional<studiocast::audio::effects::BroadcastAudioEffects> lastFx;
  std::string lastSource;
  std::filesystem::path lastAfxLib;

  std::string lastSpeakerBackend;
  std::optional<studiocast::audio::effects::BroadcastSpeakerEffects>
      lastSpeakerFx;
  std::string lastSpeakerTargetSink;
  std::filesystem::path lastSpeakerAfxLib;
#endif

  while (!stop_.load(std::memory_order_acquire)) {
    VirtualAudioServiceConfig cfg;
    {
      std::lock_guard<std::mutex> lock(mu_);
      cfg = cfg_;
    }

    const int pollMs = std::max(25, cfg.poll_ms);

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

    // Ensure virtual devices are present (best-effort).
    //
    // Note: `enabled` controls the microphone processing pipeline; virtual
    // devices may be created and routed independently.
    {
      // Mic device.
      if (cfg.create_virtual_mic && !mic_created_) {
        std::string err;
        if (studiocast::audio::CreateVirtualMic(&err)) {
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

      const bool wantSpeakersDevice =
          cfg.create_virtual_speakers || cfg.speakers_enabled;

      if (wantSpeakersDevice && !speakers_created_) {
        std::string err;
        if (studiocast::audio::CreateVirtualSpeaker(&err)) {
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
            if (studiocast::audio::StopSpeakerLoopback(&err)) {
              speakers_loopback_running_ = false;
              speakers_loopback_target_.clear();
              speakers_loopback_latency_ms_ = 0;
              {
                std::lock_guard<std::mutex> lock(mu_);
                st_.speakers_routing_active = false;
                st_.speaker_target_sink_active.clear();
              }
              clearSpeakersError();
            } else {
              setSpeakersError("Failed to stop speakers routing: " + err);
            }
          }

          {
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
          }
#endif

          const bool needLoopbackRestart =
              (!speakers_loopback_running_) ||
              (speakers_loopback_target_ != cfg.speaker_target_sink) ||
              (speakers_loopback_latency_ms_ != cfg.speaker_latency_ms);
          if (needLoopbackRestart) {
            std::string err;
            if (studiocast::audio::StartSpeakerLoopback(
                    cfg.speaker_target_sink,
                    std::max(1, cfg.speaker_latency_ms), &err)) {
              speakers_created_ = true;
              speakers_loopback_running_ = true;
              speakers_loopback_target_ = cfg.speaker_target_sink;
              speakers_loopback_latency_ms_ = cfg.speaker_latency_ms;

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
        }
#endif

        if (speakers_loopback_running_) {
          std::string err;
          if (studiocast::audio::StopSpeakerLoopback(&err)) {
            speakers_loopback_running_ = false;
            speakers_loopback_target_.clear();
            speakers_loopback_latency_ms_ = 0;
            {
              std::lock_guard<std::mutex> lock(mu_);
              st_.speakers_routing_active = false;
              st_.speaker_target_sink_active.clear();
            }
            clearSpeakersError();
          } else {
            setSpeakersError("Failed to stop speakers routing: " + err);
          }
        }

        {
          std::lock_guard<std::mutex> lock(mu_);
          st_.speakers_route_mode = "off";
        }

        // Optional cleanup: if the user disables the device, and we previously
        // created it, destroy it. This keeps daemon-managed speaker state
        // predictable.
        if (!cfg.create_virtual_speakers && speakers_created_) {
          std::string err;
          if (studiocast::audio::DestroyVirtualSpeaker(&err)) {
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
            setSpeakersError("Failed to destroy virtual speakers: " + err);
          }
        }
      }
    }

    {
      std::lock_guard<std::mutex> lock(mu_);
      st_.selected_source = cfg.source_name;
    }

#if STUDIOCAST_HAVE_PULSE_SIMPLE
    // Speaker processed pipeline supervisor.
    //
    // This runs independently from the microphone pipeline, allowing speaker
    // noise removal even when microphone effects are disabled.
    if (wantSpeakerProcessingEffective) {
      if (!speakers_created_) {
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
          if (st_.speakers_pipeline_last_error.empty()) {
            st_.speakers_pipeline_last_error =
                "Virtual speakers device not created.";
          }
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
        AudioBackendAvailability speakerAvail =
            ProbeAudioBackendAvailabilityForSpeaker(cfg);
        if (now < speakerOpenAudioCooldownUntil) {
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
          }
        } else {
          const std::string sinkName = *sinkOpt;

          // Start/restart the pipeline if needed.
          const bool speakerEffectsChanged =
              (!lastSpeakerFx.has_value() ||
               *lastSpeakerFx != cfg.effects.speaker);
          const bool spkPipelineDead =
              (spk_pipeline && !spk_pipeline->GetStats().running);
          const bool needSpkRestart =
              (!spk_pipeline) || spkPipelineDead ||
              (lastSpeakerBackend != desiredSpkBackend) ||
              (lastSpeakerTargetSink != sinkName) ||
              ((wantSpkMaxine || wantSpkOpenAudio) && speakerEffectsChanged);

          if (now >= nextSpeakerStartRetry && needSpkRestart) {
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
              st_.speaker_target_sink_active = sinkName;
              st_.speakers_backend_active =
                  std::string(ToString(speakerDecision.backend));
              st_.speakers_effects_note = speakerDecision.note;
              st_.speakers_intensity = speakerPlan.intensity;
              st_.speakers_pipeline_last_error.clear();
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
                    now + milliseconds(std::max(250, cfg.start_retry_ms));
                speakerOpenAudioCooldownReason = oerr;

                desiredSpkBackend = "passthrough";
                spk_processor = std::make_unique<PassthroughAudioProcessor>();
                {
                  std::lock_guard<std::mutex> lock(mu_);
                  st_.speakers_backend_active = "passthrough";
                  st_.speakers_effects_note =
                      "Open-source speaker backend failed to initialize; using "
                      "pass-through.\n" +
                      oerr;
                  st_.speakers_pipeline_last_error =
                      "Open Audio init failed: " + oerr;
                }
              } else {
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
            spk_pipeline = std::make_unique<studiocast::audio::AudioPipeline>(
                spk_processor.get());
            studiocast::audio::AudioPipelineConfig pcfg;
            pcfg.source_name =
                studiocast::audio::VirtualSpeakerMonitorSourceName();
            pcfg.sink_name = sinkName;
            // Speaker processing should preserve stereo.
            pcfg.channels = 2;

            std::string perr;
            if (!spk_pipeline->Start(pcfg, &perr)) {
              {
                std::lock_guard<std::mutex> lock(mu_);
                st_.speakers_pipeline_starting = false;
                st_.speakers_pipeline_running = false;
                st_.speakers_routing_active = false;
                st_.speakers_pipeline_frames_processed = 0;
                st_.speakers_pipeline_process_time_us_sum = 0;
                st_.speakers_pipeline_process_time_us_max = 0;
                st_.speakers_pipeline_process_time_us_last = 0;
                st_.speakers_pipeline_process_overruns = 0;
                st_.speakers_pipeline_last_error =
                    "Failed to start speaker pipeline: " + perr;
              }

              spk_pipeline.reset();
              spk_processor.reset();
              nextSpeakerStartRetry =
                  now + milliseconds(std::max(250, cfg.start_retry_ms));
            } else {
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
                // Preserve st_.speakers_effects_note (decision/fallback
                // message).
                st_.speakers_pipeline_last_error.clear();
              }
            }
          }

          if (spk_pipeline) {
            const auto stats = spk_pipeline->GetStats();
            {
              std::lock_guard<std::mutex> lock(mu_);
              st_.speakers_pipeline_running = stats.running;
              st_.speakers_routing_active = stats.running;
              st_.speaker_target_sink_active = sinkName;
              st_.speakers_pipeline_frames_processed = stats.frames_processed;
              st_.speakers_pipeline_process_time_us_sum =
                  stats.process_time_us_sum;
              st_.speakers_pipeline_process_time_us_max =
                  stats.process_time_us_max;
              st_.speakers_pipeline_process_time_us_last =
                  stats.process_time_us_last;
              st_.speakers_pipeline_process_overruns = stats.process_overruns;
              if (!stats.last_error.empty()) {
                st_.speakers_pipeline_last_error = stats.last_error;
              }
            }
          }
        }
      }
    }
#endif

    if (!cfg.enabled) {
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
        st_.effect_selector.clear();
        st_.feature_id.clear();
        st_.intensity = 0.0f;
        st_.effects_backend_active.clear();
        st_.effects_note.clear();
      }
      std::this_thread::sleep_for(milliseconds(pollMs));
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
    if (AnyMicrophoneEffectRequested(cfg.effects)) {
      avail = ProbeAudioBackendAvailabilityForMicrophone(cfg);
      const auto now2 = steady_clock::now();
      if (now2 < openAudioCooldownUntil) {
        avail.open_source_ok = false;
        avail.open_source_reason = openAudioCooldownReason.empty()
                                       ? "Open Audio backend is temporarily "
                                         "disabled due to a previous failure."
                                       : openAudioCooldownReason;
      }
    }
    const auto decision = ResolveAudioBackend(cfg.effects, avail);
    {
      std::lock_guard<std::mutex> lock(mu_);
      st_.effects_backend_active = std::string(ToString(decision.backend));
      st_.effects_note = decision.note;
    }

#if !STUDIOCAST_HAVE_PULSE_SIMPLE
    SetLastError(
        "Audio pipeline disabled at build time (libpulse-simple not found)");
    {
      std::lock_guard<std::mutex> lock(mu_);
      st_.pipeline_running = false;
      st_.pipeline_starting = false;
    }
    std::this_thread::sleep_for(milliseconds(pollMs));
    continue;
#else

    const auto now = steady_clock::now();
    if (now < nextStartRetry) {
      std::this_thread::sleep_for(milliseconds(pollMs));
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

    const bool effectsChanged = (!lastFx.has_value() || *lastFx != cfg.effects);
    const bool needRestart = (!pipeline) || (lastBackend != desiredBackend) ||
                             (lastSource != cfg.source_name) ||
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

    if (wantOpenAudio) {
      // Open-source backend (Phase 4 stub): validate model selection and keep
      // the pipeline alive.
      if (needRestart) {
        {
          std::lock_guard<std::mutex> lock(mu_);
          st_.pipeline_starting = true;
          st_.pipeline_running = false;

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
          openAudioCooldownUntil =
              now + milliseconds(std::max(250, cfg.start_retry_ms));
          openAudioCooldownReason = oerr;

          desiredBackend = "passthrough";
          processor = std::make_unique<PassthroughAudioProcessor>();

          // Update status to reflect actual backend (decision may still say
          // open_source this tick).
          {
            std::lock_guard<std::mutex> lock(mu_);
            st_.effects_backend_active = "passthrough";
            st_.effects_note = "Open-source audio backend failed to "
                               "initialize; using pass-through.\n" +
                               oerr;
          }
        } else {
          processor = std::move(oa);
        }

        pipeline =
            std::make_unique<studiocast::audio::AudioPipeline>(processor.get());

        studiocast::audio::AudioPipelineConfig pcfg;
        pcfg.source_name = cfg.source_name;
        pcfg.sink_name = "studiocast_sink";

        std::string perr;
        if (!pipeline->Start(pcfg, &perr)) {
          SetLastError("Failed to start audio pipeline: " + perr);
          pipeline.reset();
          processor.reset();
          nextStartRetry =
              now + milliseconds(std::max(250, cfg.start_retry_ms));
          std::lock_guard<std::mutex> lock(mu_);
          st_.pipeline_starting = false;
          std::this_thread::sleep_for(milliseconds(pollMs));
          continue;
        }

        lastBackend = desiredBackend;
        lastSource = cfg.source_name;
        lastFx = cfg.effects;

        {
          std::lock_guard<std::mutex> lock(mu_);
          st_.pipeline_starting = false;
          st_.pipeline_running = true;
          st_.last_error.clear();
        }
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
        }
      }

      std::this_thread::sleep_for(milliseconds(pollMs));
      continue;
    }

    if (!wantMaxine) {
      // Effects are disabled: keep the pipeline alive with pass-through audio.
      if (needRestart) {
        {
          std::lock_guard<std::mutex> lock(mu_);
          st_.pipeline_starting = true;
          st_.pipeline_running = false;

          // No GPU requirement in pass-through mode.
          st_.gpu_index = -1;
          st_.gpu_name.clear();
          st_.gpu_compute_cap.clear();
        }

        processor = std::make_unique<PassthroughAudioProcessor>();
        pipeline =
            std::make_unique<studiocast::audio::AudioPipeline>(processor.get());

        studiocast::audio::AudioPipelineConfig pcfg;
        pcfg.source_name = cfg.source_name;
        pcfg.sink_name = "studiocast_sink";

        std::string perr;
        if (!pipeline->Start(pcfg, &perr)) {
          SetLastError("Failed to start audio pipeline: " + perr);
          pipeline.reset();
          processor.reset();
          nextStartRetry =
              now + milliseconds(std::max(250, cfg.start_retry_ms));
          std::lock_guard<std::mutex> lock(mu_);
          st_.pipeline_starting = false;
          std::this_thread::sleep_for(milliseconds(pollMs));
          continue;
        }

        lastBackend = desiredBackend;
        lastSource = cfg.source_name;

        {
          std::lock_guard<std::mutex> lock(mu_);
          st_.pipeline_starting = false;
          st_.pipeline_running = true;
          st_.last_error.clear();
        }
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
        }
      }

      std::this_thread::sleep_for(milliseconds(pollMs));
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
      SetLastError("Failed to select a supported NVIDIA GPU: " + sel.error);
      nextStartRetry = now + milliseconds(std::max(250, cfg.start_retry_ms));
      std::this_thread::sleep_for(milliseconds(pollMs));
      continue;
    }

    const auto paths = studiocast::maxine::ResolveMaxinePaths();
    if (!paths.afx.ok) {
      std::string msg = "AFX SDK not available";
      if (!paths.afx.problems.empty()) {
        msg += ": ";
        msg += paths.afx.problems.front();
      }
      SetLastError(msg);
      nextStartRetry = now + milliseconds(std::max(250, cfg.start_retry_ms));
      std::this_thread::sleep_for(milliseconds(pollMs));
      continue;
    }

    if (!api || paths.afx.library != lastAfxLib) {
      api = std::make_unique<studiocast::maxine::afx::AfxApi>();
      std::string aerr;
      if (!api->InitializeFromLibraryPath(paths.afx.library, &aerr)) {
        SetLastError("Failed to initialize AFX runtime: " + aerr);
        api.reset();
        nextStartRetry = now + milliseconds(std::max(250, cfg.start_retry_ms));
        std::this_thread::sleep_for(milliseconds(pollMs));
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
      }

      std::string ferr;
      if (!fx->Configure(e, &ferr)) {
        SetLastError("Failed to configure AFX effect: " + ferr);
        fx->Destroy();
        fx.reset();
        nextStartRetry = now + milliseconds(std::max(250, cfg.start_retry_ms));
        std::lock_guard<std::mutex> lock(mu_);
        st_.pipeline_starting = false;
        std::this_thread::sleep_for(milliseconds(pollMs));
        continue;
      }
      if (!fx->Load(&ferr)) {
        SetLastError("Failed to load AFX effect: " + ferr);
        fx->Destroy();
        fx.reset();
        nextStartRetry = now + milliseconds(std::max(250, cfg.start_retry_ms));
        std::lock_guard<std::mutex> lock(mu_);
        st_.pipeline_starting = false;
        std::this_thread::sleep_for(milliseconds(pollMs));
        continue;
      }

      processor = std::make_unique<studiocast::maxine::afx::AfxAudioProcessor>(
          fx.get());
      pipeline =
          std::make_unique<studiocast::audio::AudioPipeline>(processor.get());
      studiocast::audio::AudioPipelineConfig pcfg;
      pcfg.source_name = cfg.source_name;
      pcfg.sink_name = "studiocast_sink";

      std::string perr;
      if (!pipeline->Start(pcfg, &perr)) {
        SetLastError("Failed to start audio pipeline: " + perr);
        pipeline.reset();
        nextStartRetry = now + milliseconds(std::max(250, cfg.start_retry_ms));
        std::lock_guard<std::mutex> lock(mu_);
        st_.pipeline_starting = false;
        std::this_thread::sleep_for(milliseconds(pollMs));
        continue;
      }

      lastFx = cfg.effects;
      lastSource = cfg.source_name;
      lastBackend = desiredBackend;
      {
        std::lock_guard<std::mutex> lock(mu_);
        st_.pipeline_starting = false;
        st_.pipeline_running = true;
        st_.last_error.clear();
      }
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
      }
    }

    std::this_thread::sleep_for(milliseconds(pollMs));
#endif
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
