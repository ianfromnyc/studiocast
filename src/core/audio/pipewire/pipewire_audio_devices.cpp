#include "core/audio/pipewire/pipewire_audio_devices.h"

#include <atomic>
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

// ---------------------------------------------------------------------------
// NativeAudioDevices
// ---------------------------------------------------------------------------

struct NativeAudioDevices::State {
  mutable std::mutex mu;
  std::unique_ptr<PipeWireAudioNode> mic;
  std::unique_ptr<PipeWireAudioNode> speaker;

  // Pass-through route from the virtual speakers to a real sink.
  std::unique_ptr<PipeWireAudioNode> loopback_playback;
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

bool NativeAudioDevices::CreateVirtualMic(std::string *error) {
  // The node the daemon already has is the answer. The Pulse scan below forks
  // pactl twice, and the supervisor asks for this device on every poll, so it
  // must not run for a device that is already there.
  {
    std::lock_guard<std::mutex> lock(state_->mu);
    if (state_->mic)
      return true;
  }

  RemoveStalePulseDevices();

  std::lock_guard<std::mutex> lock(state_->mu);
  if (state_->mic)
    return true;

  AudioNodeConfig cfg;
  cfg.role = AudioNodeRole::kVirtualSource;
  cfg.node_name = std::string(studiocast::pw::kVirtualMicNodeName);
  cfg.node_description = std::string(studiocast::pw::kVirtualMicDescription);
  cfg.sample_rate = kSampleRate;
  cfg.channels = kMicChannels;
  cfg.frame_samples = kFrameSamples;

  auto node = std::make_unique<PipeWireAudioNode>();
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
  // See CreateVirtualMic: no Pulse scan for a device that already exists.
  {
    std::lock_guard<std::mutex> lock(state_->mu);
    if (state_->speaker)
      return true;
  }

  RemoveStalePulseDevices();

  std::lock_guard<std::mutex> lock(state_->mu);
  if (state_->speaker)
    return true;

  AudioNodeConfig cfg;
  cfg.role = AudioNodeRole::kVirtualSink;
  cfg.node_name = std::string(studiocast::pw::kVirtualSpeakerNodeName);
  cfg.node_description =
      std::string(studiocast::pw::kVirtualSpeakerDescription);
  cfg.sample_rate = kSampleRate;
  cfg.channels = kSpeakerChannels;
  cfg.frame_samples = kFrameSamples;

  auto node = std::make_unique<PipeWireAudioNode>();
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
  cfg.node_name = "studiocast_speakers_route";
  cfg.node_description = "StudioCast Speakers Route";
  cfg.target_object = target_sink_name;
  cfg.sample_rate = kSampleRate;
  cfg.channels = kSpeakerChannels;
  cfg.frame_samples = kFrameSamples;

  auto playback = std::make_unique<PipeWireAudioNode>();
  if (!playback->Start(cfg, error))
    return false;

  state_->loopback_playback = std::move(playback);
  state_->loopback_stop.store(false, std::memory_order_release);

  PipeWireAudioNode *from = state_->speaker.get();
  PipeWireAudioNode *to = state_->loopback_playback.get();
  state_->loopback_thread = std::thread([this, from, to] {
    // A plain pump. The samples pass through with no processing, which is what
    // the pass-through route means.
    std::vector<float> frame(
        static_cast<std::size_t>(kFrameSamples) * kSpeakerChannels, 0.0f);
    const std::size_t bytes = frame.size() * sizeof(float);
    std::string err;
    while (!state_->loopback_stop.load(std::memory_order_acquire)) {
      if (!from->Read(frame.data(), bytes, &err)) {
        // No application is playing into the virtual speakers, so there is
        // nothing to move. Try again on the next frame.
        continue;
      }
      if (!to->Write(frame.data(), bytes, &err))
        continue;
    }
  });

  return true;
}

bool NativeAudioDevices::StopSpeakerLoopback(std::string *error) {
  if (error)
    error->clear();

  std::thread worker;
  {
    std::lock_guard<std::mutex> lock(state_->mu);
    if (!state_->loopback_thread.joinable() && !state_->loopback_playback)
      return true;
    state_->loopback_stop.store(true, std::memory_order_release);
    if (state_->speaker)
      state_->speaker->RequestStop();
    if (state_->loopback_playback)
      state_->loopback_playback->RequestStop();
    worker = std::move(state_->loopback_thread);
  }

  if (worker.joinable())
    worker.join();

  std::lock_guard<std::mutex> lock(state_->mu);
  state_->loopback_playback.reset();
  return true;
}

void NativeAudioDevices::RemoveStalePulseDevices() {
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

AudioConsumerSnapshot NativeAudioDevices::DetectMicrophoneConsumers() const {
  std::lock_guard<std::mutex> lock(state_->mu);
  return SnapshotFrom(state_->mic.get());
}

AudioConsumerSnapshot NativeAudioDevices::DetectSpeakerConsumers() const {
  std::lock_guard<std::mutex> lock(state_->mu);
  return SnapshotFrom(state_->speaker.get());
}

PipeWireAudioNode *NativeAudioDevices::MicNode() const {
  std::lock_guard<std::mutex> lock(state_->mu);
  return state_->mic.get();
}

PipeWireAudioNode *NativeAudioDevices::SpeakerNode() const {
  std::lock_guard<std::mutex> lock(state_->mu);
  return state_->speaker.get();
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

      AudioNodeConfig out;
      out.role = AudioNodeRole::kPlayback;
      out.node_name = "studiocast_speakers_out";
      out.node_description = "StudioCast Speakers Output";
      out.target_object = StripMonitorSuffix(cfg.sink_name);
      out.sample_rate = cfg.sample_rate;
      out.channels = cfg.channels;
      out.frame_samples = cfg.frame_samples;

      owned_playback_ = std::make_unique<PipeWireAudioNode>();
      if (!owned_playback_->Start(out, error)) {
        owned_playback_.reset();
        read_node_ = nullptr;
        return false;
      }
      write_node_ = owned_playback_.get();
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

    const auto resolution = ResolveSafeInputSourceName(cfg.source_name);
    if (!resolution.ok) {
      if (error)
        *error = resolution.error;
      write_node_ = nullptr;
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

    owned_capture_ = std::make_unique<PipeWireAudioNode>();
    if (!owned_capture_->Start(in, error)) {
      owned_capture_.reset();
      write_node_ = nullptr;
      return false;
    }
    read_node_ = owned_capture_.get();
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
    owned_capture_.reset();
    owned_playback_.reset();
    read_node_ = nullptr;
    write_node_ = nullptr;
    stop_.store(false, std::memory_order_release);
  }

  const std::atomic<bool> *external_stop_ = nullptr;
  std::atomic<bool> stop_{false};
  bool speakers_path_ = false;

  // Nodes this object created and must destroy.
  std::unique_ptr<PipeWireAudioNode> owned_capture_;
  std::unique_ptr<PipeWireAudioNode> owned_playback_;

  // Borrowed pointers. One of them names a node the service owns.
  PipeWireAudioNode *read_node_ = nullptr;
  PipeWireAudioNode *write_node_ = nullptr;
};

} // namespace

std::unique_ptr<AudioPipelineIo> CreatePipeWireAudioIo() {
  return std::make_unique<PipeWireAudioIo>();
}

} // namespace studiocast::audio::pw_backend
