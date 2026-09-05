#include "core/audio/pipewire/pipewire_audio_devices.h"

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/audio/audio_device_safety.h"
#include "core/audio/mic_monitor.h"
#include "core/audio/pulse/pactl.h"
#include "core/pipewire/pipewire_support.h"

namespace studiocast::audio::pw_backend {

namespace {

using studiocast::pw::AudioNodeConfig;
using studiocast::pw::AudioNodeRole;
using studiocast::pw::PipeWireAudioNode;

constexpr int kSampleRate = 48000;
constexpr std::uint32_t kFrameSamples = 480;

// The virtual microphone is mono, the virtual speakers are stereo. These match
// the formats the Pulse devices use today.
constexpr std::uint32_t kMicChannels = 1;
constexpr std::uint32_t kSpeakerChannels = 2;

AudioConsumerSnapshot SnapshotFrom(const PipeWireAudioNode *node) {
  AudioConsumerSnapshot out;
  if (!node) {
    out.error = "The native PipeWire device is not created.";
    return out;
  }
  out.count = node->ConsumerCount();
  out.present = out.count > 0;
  return out;
}

// PipeWire names a monitor source after its sink, so the Pulse suffix must go
// before the name can address a node.
std::string StripMonitorSuffix(const std::string &name) {
  static constexpr std::string_view kSuffix = ".monitor";
  if (name.size() > kSuffix.size() &&
      name.compare(name.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0)
    return name.substr(0, name.size() - kSuffix.size());
  return name;
}

bool Contains(const std::string &hay, const std::string &needle) {
  return hay.find(needle) != std::string::npos;
}

// True when the module arguments hold `key=value` as a whole whitespace
// delimited token. A plain substring search would also accept a longer name
// that starts the same, such as `sink_name=studiocast_sink_backup`, and this
// file unloads what it matches.
bool HasArgument(const std::string &args, const std::string &key,
                 const std::string &value) {
  static constexpr const char *kSpace = " \t\n";
  const std::string want = key + "=" + value;
  std::size_t pos = 0;
  while (pos <= args.size()) {
    const std::size_t end = args.find_first_of(kSpace, pos);
    const std::size_t len =
        end == std::string::npos ? std::string::npos : end - pos;
    if (args.compare(pos, len, want) == 0)
      return true;
    if (end == std::string::npos)
      break;
    pos = end + 1;
  }
  return false;
}

// Names the Pulse device path uses. They are the same strings the pactl
// helpers in core/audio/virtual_mic.cpp and virtual_speaker.cpp load.
constexpr const char *kPulseSinkName = "studiocast_sink";
constexpr const char *kPulseMicSourceName = "studiocast_mic";
constexpr const char *kPulseSpeakersSinkName = "studiocast_speakers";

// True when the module belongs to the microphone monitor, which works on both
// backends and must survive the clean-up. The monitor tags its loopback with
// its own stream name, so a plain `source=studiocast_mic` match is not enough.
bool IsMicMonitorModule(const pulse::PactlModule &m) {
  return m.name == "module-loopback" &&
         Contains(m.args, MicMonitorStreamName());
}

} // namespace

std::vector<StalePulseModule> DetectStalePulseDeviceModules(
    std::string *error) {
  if (error)
    error->clear();

  std::string details;
  if (!pulse::PactlAvailable(&details)) {
    // No pactl means no Pulse modules to remove. That is not a failure.
    return {};
  }

  std::string listErr;
  const auto modules = pulse::ListModules(&listErr);
  if (!listErr.empty()) {
    if (error)
      *error = listErr;
    return {};
  }

  std::vector<StalePulseModule> loopbacks;
  std::vector<StalePulseModule> remaps;
  std::vector<StalePulseModule> sinks;

  for (const auto &m : modules) {
    if (IsMicMonitorModule(m))
      continue;

    const StalePulseModule entry{m.id, m.name, m.args};

    if (m.name == "module-loopback") {
      // The legacy pass-through into the StudioCast sink, and the Pulse
      // speaker route out of the virtual speakers.
      if (HasArgument(m.args, "sink", kPulseSinkName) ||
          HasArgument(m.args,
                      "source", std::string(kPulseSpeakersSinkName) +
                                    ".monitor")) {
        loopbacks.push_back(entry);
      }
      continue;
    }

    if (m.name == "module-remap-source" &&
        HasArgument(m.args, "source_name", kPulseMicSourceName)) {
      remaps.push_back(entry);
      continue;
    }

    if (m.name == "module-null-sink" &&
        (HasArgument(m.args, "sink_name", kPulseSinkName) ||
         HasArgument(m.args, "sink_name", kPulseSpeakersSinkName))) {
      sinks.push_back(entry);
      continue;
    }
  }

  // Unload order: a module is never removed while another one still needs it.
  std::vector<StalePulseModule> out;
  out.reserve(loopbacks.size() + remaps.size() + sinks.size());
  out.insert(out.end(), loopbacks.begin(), loopbacks.end());
  out.insert(out.end(), remaps.begin(), remaps.end());
  out.insert(out.end(), sinks.begin(), sinks.end());
  return out;
}

bool UnloadStalePulseDeviceModules(std::vector<std::string> *removed,
                                   std::string *error) {
  if (removed)
    removed->clear();
  if (error)
    error->clear();

  std::string detectErr;
  const auto stale = DetectStalePulseDeviceModules(&detectErr);
  if (!detectErr.empty()) {
    if (error)
      *error = detectErr;
    return false;
  }

  bool ok = true;
  for (const auto &m : stale) {
    std::string err;
    if (!pulse::UnloadModule(m.id, &err)) {
      ok = false;
      if (error) {
        if (!error->empty())
          *error += "; ";
        *error += "Could not unload " + m.name + " " + std::to_string(m.id);
        if (!err.empty())
          *error += ": " + err;
      }
      continue;
    }
    if (removed) {
      removed->push_back("Removed stale Pulse " + m.name + " (id " +
                         std::to_string(m.id) + "): " + m.args);
    }
  }
  return ok;
}

namespace internal {

void RunSpeakerLoopbackPump(const SpeakerLoopbackPumpHooks &hooks) {
  if (!hooks.cancelled || !hooks.read_frame || !hooks.write_frame ||
      !hooks.backoff)
    return;

  while (!hooks.cancelled()) {
    // A read that found nothing is the usual case: no application plays into
    // the virtual speakers. Wait one frame, or this loop spins.
    if (!hooks.read_frame()) {
      hooks.backoff();
      continue;
    }
    if (!hooks.write_frame())
      hooks.backoff();
  }
}

std::string AudioFormatMismatch(const std::string &what,
                                const studiocast::pw::AudioNodeConfig &node,
                                int sample_rate, std::uint32_t channels,
                                std::uint32_t frame_samples) {
  auto differs = [](const std::string &field, long long wanted,
                    long long have) {
    return field + " " + std::to_string(wanted) + " where the node uses " +
           std::to_string(have);
  };

  std::vector<std::string> problems;
  if (sample_rate != node.sample_rate)
    problems.push_back(differs("sample rate", sample_rate, node.sample_rate));
  if (channels != node.channels)
    problems.push_back(differs("channel count", channels, node.channels));
  if (frame_samples != node.frame_samples) {
    problems.push_back(
        differs("frame size", frame_samples, node.frame_samples));
  }
  if (problems.empty())
    return {};

  std::string out = "The audio pipeline asks the StudioCast " + what + " for ";
  for (std::size_t i = 0; i < problems.size(); ++i) {
    if (i > 0)
      out += i + 1 == problems.size() ? " and " : ", ";
    out += problems[i];
  }
  out += ".";
  return out;
}

} // namespace internal

// ---------------------------------------------------------------------------
// NativeAudioDevices
// ---------------------------------------------------------------------------

struct NativeAudioDevices::State {
  mutable std::mutex mu;
  NativeAudioDeviceOptions options;
  // Shared, so a pipeline or a route that took a reference keeps its node
  // alive even when the supervisor destroys the device on another thread.
  std::shared_ptr<PipeWireAudioNode> mic;
  std::shared_ptr<PipeWireAudioNode> speaker;

  // Pass-through route from the virtual speakers to a real sink.
  std::shared_ptr<PipeWireAudioNode> loopback_playback;
  std::thread loopback_thread;
  std::atomic<bool> loopback_stop{false};

  // What the last Pulse clean-up removed, for the log and the status.
  std::vector<std::string> cleanup_log;
};

NativeAudioDevices::NativeAudioDevices() : state_(std::make_unique<State>()) {}

NativeAudioDevices::~NativeAudioDevices() {
  std::string ignored;
  (void)StopSpeakerLoopback(&ignored);
  (void)DestroyVirtualMic(&ignored);
  (void)DestroyVirtualSpeaker(&ignored);
}

NativeAudioDevices &NativeAudioDevices::Instance() {
  static NativeAudioDevices instance;
  return instance;
}

void NativeAudioDevices::SetOptions(const NativeAudioDeviceOptions &options) {
  std::lock_guard<std::mutex> lock(state_->mu);
  state_->options = options;
}

NativeAudioDeviceOptions NativeAudioDevices::Options() const {
  std::lock_guard<std::mutex> lock(state_->mu);
  return state_->options;
}

bool NativeAudioDevices::CreateVirtualMic(std::string *error) {
  // The node the daemon already has is the answer, as long as it is still in
  // the graph. The Pulse scan below forks pactl twice, and the supervisor asks
  // for this device on every poll, so it must not run for a device that is
  // already there.
  {
    std::lock_guard<std::mutex> lock(state_->mu);
    if (state_->mic && state_->mic->IsRunning())
      return true;
  }

  RemoveStalePulseDevices();

  std::lock_guard<std::mutex> lock(state_->mu);
  if (state_->mic && state_->mic->IsRunning())
    return true;
  // A node the server took down never comes back by itself, so let it go and
  // make a new one below.
  state_->mic.reset();

  AudioNodeConfig cfg;
  cfg.role = AudioNodeRole::kVirtualSource;
  cfg.node_name =
      state_->options.node_name_suffix.empty()
          ? std::string(studiocast::pw::kVirtualMicNodeName)
          : std::string(studiocast::pw::kVirtualMicNodeName) +
                state_->options.node_name_suffix;
  cfg.node_description = std::string(studiocast::pw::kVirtualMicDescription) +
                         state_->options.node_name_suffix;
  cfg.sample_rate = kSampleRate;
  cfg.channels = kMicChannels;
  cfg.frame_samples = kFrameSamples;

  auto node = std::make_shared<PipeWireAudioNode>();
  if (!node->Start(cfg, error))
    return false;
  state_->mic = std::move(node);
  return true;
}

bool NativeAudioDevices::DestroyVirtualMic(std::string *error) {
  if (error)
    error->clear();
  std::lock_guard<std::mutex> lock(state_->mu);
  state_->mic.reset();
  return true;
}

bool NativeAudioDevices::CreateVirtualSpeaker(std::string *error) {
  // See CreateVirtualMic: no Pulse scan for a device that is already there,
  // and a node the server dropped is replaced instead of answered with.
  bool went_down = false;
  {
    std::lock_guard<std::mutex> lock(state_->mu);
    if (state_->speaker && state_->speaker->IsRunning())
      return true;
    went_down = state_->speaker != nullptr;
  }

  if (went_down) {
    // The route pump reads that node, so it has to end before the node goes.
    std::string ignored;
    (void)StopSpeakerLoopback(&ignored);
  }

  RemoveStalePulseDevices();

  std::lock_guard<std::mutex> lock(state_->mu);
  if (state_->speaker && state_->speaker->IsRunning())
    return true;
  state_->speaker.reset();

  AudioNodeConfig cfg;
  cfg.role = AudioNodeRole::kVirtualSink;
  cfg.node_name =
      state_->options.node_name_suffix.empty()
          ? std::string(studiocast::pw::kVirtualSpeakerNodeName)
          : std::string(studiocast::pw::kVirtualSpeakerNodeName) +
                state_->options.node_name_suffix;
  cfg.node_description =
      std::string(studiocast::pw::kVirtualSpeakerDescription) +
      state_->options.node_name_suffix;
  cfg.sample_rate = kSampleRate;
  cfg.channels = kSpeakerChannels;
  cfg.frame_samples = kFrameSamples;

  auto node = std::make_shared<PipeWireAudioNode>();
  if (!node->Start(cfg, error))
    return false;
  state_->speaker = std::move(node);
  return true;
}

bool NativeAudioDevices::DestroyVirtualSpeaker(std::string *error) {
  if (error)
    error->clear();
  std::string ignored;
  (void)StopSpeakerLoopback(&ignored);
  std::lock_guard<std::mutex> lock(state_->mu);
  state_->speaker.reset();
  return true;
}

bool NativeAudioDevices::StartSpeakerLoopback(
    const std::string &target_sink_name, std::string *error) {
  if (error)
    error->clear();

  std::string reason;
  if (IsUnsafeSpeakerTargetSinkName(target_sink_name, &reason)) {
    if (error)
      *error = reason;
    return false;
  }

  std::string ignored;
  (void)StopSpeakerLoopback(&ignored);

  std::lock_guard<std::mutex> lock(state_->mu);
  if (!state_->speaker) {
    if (error)
      *error = "The native virtual speakers are not created.";
    return false;
  }

  AudioNodeConfig cfg;
  cfg.role = AudioNodeRole::kPlayback;
  cfg.node_name =
      "studiocast_speakers_route" + state_->options.node_name_suffix;
  cfg.node_description =
      "StudioCast Speakers Route" + state_->options.node_name_suffix;
  cfg.target_object = target_sink_name;
  cfg.sample_rate = kSampleRate;
  cfg.channels = kSpeakerChannels;
  cfg.frame_samples = kFrameSamples;

  auto playback = std::make_shared<PipeWireAudioNode>();
  if (!playback->Start(cfg, error))
    return false;

  state_->loopback_playback = std::move(playback);
  state_->loopback_stop.store(false, std::memory_order_release);

  // An earlier route woke the shared node to end its pump. The node belongs to
  // the service and carries samples again from here.
  state_->speaker->ClearStopRequest();

  // The pump holds its own references, so neither node can go away under it.
  std::shared_ptr<PipeWireAudioNode> from = state_->speaker;
  std::shared_ptr<PipeWireAudioNode> to = state_->loopback_playback;
  state_->loopback_thread = std::thread([this, from, to] {
    // A plain pump. The samples pass through with no processing, which is what
    // the pass-through route means.
    std::vector<float> frame(
        static_cast<std::size_t>(kFrameSamples) * kSpeakerChannels, 0.0f);
    const std::size_t bytes = frame.size() * sizeof(float);
    std::string err;

    internal::SpeakerLoopbackPumpHooks hooks;
    hooks.cancelled = [this] {
      return state_->loopback_stop.load(std::memory_order_acquire);
    };
    hooks.read_frame = [&] { return from->Read(frame.data(), bytes, &err); };
    hooks.write_frame = [&] { return to->Write(frame.data(), bytes, &err); };
    hooks.backoff = [] {
      // One frame. A node that nothing drives answers at once, so without this
      // the loop would ask a few million times a second.
      std::this_thread::sleep_for(std::chrono::milliseconds(
          kFrameSamples * 1000 / static_cast<std::uint32_t>(kSampleRate)));
    };
    internal::RunSpeakerLoopbackPump(hooks);
  });

  return true;
}

bool NativeAudioDevices::StopSpeakerLoopback(std::string *error) {
  if (error)
    error->clear();

  std::thread worker;
  std::shared_ptr<PipeWireAudioNode> speaker;
  {
    std::lock_guard<std::mutex> lock(state_->mu);
    if (!state_->loopback_thread.joinable() && !state_->loopback_playback)
      return true;
    state_->loopback_stop.store(true, std::memory_order_release);
    // The pump can sit in a read of the shared node for the whole read
    // timeout, so wake it. The wake-up is taken back below.
    speaker = state_->speaker;
    if (speaker)
      speaker->RequestStop();
    if (state_->loopback_playback)
      state_->loopback_playback->RequestStop();
    worker = std::move(state_->loopback_thread);
  }

  if (worker.joinable())
    worker.join();

  std::lock_guard<std::mutex> lock(state_->mu);
  // The virtual speakers belong to the service, not to this route. A stop
  // request that stayed on them would make every later read and write of the
  // node fail at once: the next pump would spin, and the processed speaker
  // pipeline, which reads the same node, would never carry a sample again.
  if (speaker && speaker == state_->speaker)
    speaker->ClearStopRequest();
  state_->loopback_playback.reset();
  return true;
}

void NativeAudioDevices::RemoveStalePulseDevices() {
  {
    std::lock_guard<std::mutex> lock(state_->mu);
    // A caller that shares the machine with another StudioCast leaves the
    // sound server alone: the modules it would remove may belong to a running
    // daemon on the Pulse backend.
    if (!state_->options.remove_stale_pulse_devices)
      return;
  }

  std::vector<std::string> removed;
  std::string error;
  const bool ok = UnloadStalePulseDeviceModules(&removed, &error);

  for (const auto &line : removed)
    std::cerr << "[studiocast] " << line << "\n";
  if (!ok && !error.empty()) {
    std::cerr << "[studiocast] Stale Pulse device clean-up: " << error << "\n";
  }

  if (removed.empty())
    return;
  std::lock_guard<std::mutex> lock(state_->mu);
  state_->cleanup_log = std::move(removed);
}

std::vector<std::string> NativeAudioDevices::LastPulseCleanupLog() const {
  std::lock_guard<std::mutex> lock(state_->mu);
  return state_->cleanup_log;
}

bool NativeAudioDevices::MicWentDown() const {
  std::lock_guard<std::mutex> lock(state_->mu);
  return state_->mic && !state_->mic->IsRunning();
}

bool NativeAudioDevices::SpeakerWentDown() const {
  std::lock_guard<std::mutex> lock(state_->mu);
  return state_->speaker && !state_->speaker->IsRunning();
}

AudioConsumerSnapshot NativeAudioDevices::DetectMicrophoneConsumers() const {
  std::lock_guard<std::mutex> lock(state_->mu);
  return SnapshotFrom(state_->mic.get());
}

AudioConsumerSnapshot NativeAudioDevices::DetectSpeakerConsumers() const {
  std::lock_guard<std::mutex> lock(state_->mu);
  return SnapshotFrom(state_->speaker.get());
}

std::shared_ptr<PipeWireAudioNode> NativeAudioDevices::MicNode() const {
  std::lock_guard<std::mutex> lock(state_->mu);
  return state_->mic;
}

std::shared_ptr<PipeWireAudioNode> NativeAudioDevices::SpeakerNode() const {
  std::lock_guard<std::mutex> lock(state_->mu);
  return state_->speaker;
}

void ShutdownNativeAudioDevices() {
  auto &devices = NativeAudioDevices::Instance();
  std::string ignored;
  (void)devices.StopSpeakerLoopback(&ignored);
  (void)devices.DestroyVirtualMic(&ignored);
  (void)devices.DestroyVirtualSpeaker(&ignored);
}

// ---------------------------------------------------------------------------
// PipeWireAudioIo
// ---------------------------------------------------------------------------

namespace {

class PipeWireAudioIo final : public AudioPipelineIo {
public:
  ~PipeWireAudioIo() override { Close(); }

  void SetStopRequestedFlag(const std::atomic<bool> *stop_requested) override {
    external_stop_ = stop_requested;
  }

  bool Open(const AudioPipelineConfig &cfg, std::string *error) override {
    if (error)
      error->clear();
    Close();

    auto &devices = NativeAudioDevices::Instance();
    speakers_path_ = cfg.allow_monitor_source;

    if (speakers_path_) {
      // The speaker pipeline reads what applications play into the virtual
      // speakers, and plays the processed result into a real sink.
      read_node_ = devices.SpeakerNode();
      if (!read_node_) {
        if (error)
          *error = "The native PipeWire virtual speakers are not created.";
        return false;
      }
      const std::string mismatch = internal::AudioFormatMismatch(
          "virtual speakers", read_node_->Format(), cfg.sample_rate,
          cfg.channels, cfg.frame_samples);
      if (!mismatch.empty()) {
        if (error)
          *error = mismatch;
        read_node_.reset();
        return false;
      }
      // A node the service owns may still carry the stop request of whoever
      // woke it last. It carries samples for this pipeline from here.
      read_node_->ClearStopRequest();

      AudioNodeConfig out;
      out.role = AudioNodeRole::kPlayback;
      out.node_name = "studiocast_speakers_out";
      out.node_description = "StudioCast Speakers Output";
      out.target_object = StripMonitorSuffix(cfg.sink_name);
      out.sample_rate = cfg.sample_rate;
      out.channels = cfg.channels;
      out.frame_samples = cfg.frame_samples;

      owned_playback_ = std::make_shared<PipeWireAudioNode>();
      if (!owned_playback_->Start(out, error)) {
        owned_playback_.reset();
        read_node_.reset();
        return false;
      }
      write_node_ = owned_playback_;
      return true;
    }

    // The microphone pipeline captures from a real source and writes into the
    // virtual microphone node that the service owns.
    write_node_ = devices.MicNode();
    if (!write_node_) {
      if (error)
        *error = "The native PipeWire virtual microphone is not created.";
      return false;
    }
    const std::string mismatch = internal::AudioFormatMismatch(
        "virtual microphone", write_node_->Format(), cfg.sample_rate,
        cfg.channels, cfg.frame_samples);
    if (!mismatch.empty()) {
      if (error)
        *error = mismatch;
      write_node_.reset();
      return false;
    }
    // See the speakers path: the service owns this node, so a stop request
    // left on it must not outlive the caller that made it.
    write_node_->ClearStopRequest();

    const auto resolution = ResolveSafeInputSourceName(cfg.source_name);
    if (!resolution.ok) {
      if (error)
        *error = resolution.error;
      write_node_.reset();
      return false;
    }

    AudioNodeConfig in;
    in.role = AudioNodeRole::kCapture;
    in.node_name = "studiocast_mic_capture";
    in.node_description = "StudioCast Microphone Capture";
    in.target_object = resolution.source_name;
    in.sample_rate = cfg.sample_rate;
    in.channels = cfg.channels;
    in.frame_samples = cfg.frame_samples;

    owned_capture_ = std::make_shared<PipeWireAudioNode>();
    if (!owned_capture_->Start(in, error)) {
      owned_capture_.reset();
      write_node_.reset();
      return false;
    }
    read_node_ = owned_capture_;
    return true;
  }

  bool Read(void *dst, std::size_t bytes, std::string *error) override {
    if (StopRequested())
      return false;
    if (!read_node_) {
      if (error)
        *error = "The PipeWire capture node is not open.";
      return false;
    }
    return read_node_->Read(dst, bytes, error);
  }

  bool Write(const void *src, std::size_t bytes, std::string *error) override {
    if (StopRequested())
      return false;
    if (!write_node_) {
      if (error)
        *error = "The PipeWire playback node is not open.";
      return false;
    }
    return write_node_->Write(src, bytes, error);
  }

  bool GetCaptureLatencyUs(std::uint64_t *latency_us) override {
    return read_node_ && read_node_->GetLatencyUs(latency_us);
  }

  bool GetPlaybackLatencyUs(std::uint64_t *latency_us) override {
    return write_node_ && write_node_->GetLatencyUs(latency_us);
  }

  void Flush() override {
    if (read_node_)
      read_node_->Flush();
    if (write_node_)
      write_node_->Flush();
  }

  void RequestStop() override {
    stop_.store(true, std::memory_order_release);
    if (owned_capture_)
      owned_capture_->RequestStop();
    if (owned_playback_)
      owned_playback_->RequestStop();
    // A node the service owns keeps running for the next pipeline start, so
    // only the read or write side that this I/O created is stopped.
  }

private:
  bool StopRequested() const {
    if (stop_.load(std::memory_order_acquire))
      return true;
    return external_stop_ && external_stop_->load(std::memory_order_acquire);
  }

  void Close() {
    read_node_.reset();
    write_node_.reset();
    owned_capture_.reset();
    owned_playback_.reset();
    stop_.store(false, std::memory_order_release);
  }

  const std::atomic<bool> *external_stop_ = nullptr;
  std::atomic<bool> stop_{false};
  bool speakers_path_ = false;

  // Nodes this object created and must destroy.
  std::shared_ptr<PipeWireAudioNode> owned_capture_;
  std::shared_ptr<PipeWireAudioNode> owned_playback_;

  // The ends of the pipeline. One of them is a node the service owns, and the
  // reference kept here holds it up for as long as this I/O reads or writes
  // it, whatever the supervisor does to the device meanwhile.
  std::shared_ptr<PipeWireAudioNode> read_node_;
  std::shared_ptr<PipeWireAudioNode> write_node_;
};

} // namespace

std::unique_ptr<AudioPipelineIo> CreatePipeWireAudioIo() {
  return std::make_unique<PipeWireAudioIo>();
}

} // namespace studiocast::audio::pw_backend
