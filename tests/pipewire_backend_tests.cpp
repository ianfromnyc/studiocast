#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "core/audio/pipewire/pipewire_audio_devices.h"
#include "core/audio/mic_monitor.h"
#include "core/audio/pulse/pactl.h"
#include "core/audio/virtual_audio_service.h"
#include "core/config/daemon_config.h"
#include "core/pipewire/pipewire_audio_node.h"
#include "core/video/pipewire/pipewire_camera_node.h"

#include "core/pipewire/pipewire_support.h"
#include "core/pipewire/spsc_byte_ring.h"

namespace {

using studiocast::pw::AudioTransport;
using studiocast::pw::AudioTransportPreference;
using studiocast::pw::ParseAudioTransportPreference;
using studiocast::pw::PipeWireAvailability;
using studiocast::pw::PipeWireProbeEnv;
using studiocast::pw::PipeWireSocketProbe;
using studiocast::pw::ProbePipeWireSocket;
using studiocast::pw::ResolveAudioTransport;

bool Expect(bool condition, const std::string &message) {
  if (!condition)
    std::cerr << message << "\n";
  return condition;
}

PipeWireAvailability Available() {
  PipeWireAvailability a;
  a.compiled_in = true;
  a.server_reachable = true;
  return a;
}

PipeWireAvailability NoServer() {
  PipeWireAvailability a;
  a.compiled_in = true;
  a.server_reachable = false;
  a.reason = "PipeWire socket /run/user/1000/pipewire-0 was not found.";
  return a;
}

PipeWireAvailability NotCompiledIn() {
  PipeWireAvailability a;
  a.compiled_in = false;
  a.server_reachable = false;
  a.reason = "StudioCast was built without PipeWire support.";
  return a;
}

bool TestAutoPrefersPipeWireWhenTheServerIsReachable() {
  const auto d =
      ResolveAudioTransport(AudioTransportPreference::kAuto, Available());
  return Expect(d.transport == AudioTransport::kPipeWire,
                "auto should select native PipeWire when it is available") &&
         Expect(!d.used_fallback, "auto to PipeWire is not a fallback") &&
         Expect(d.note.empty(), "auto to PipeWire should not set a note");
}

bool TestAutoUsesPulseWhenNoServerIsReachable() {
  const auto d =
      ResolveAudioTransport(AudioTransportPreference::kAuto, NoServer());
  return Expect(d.transport == AudioTransport::kPulse,
                "auto should select PulseAudio when no server is reachable") &&
         Expect(!d.used_fallback,
                "auto to PulseAudio is the documented default, not a fallback");
}

bool TestAutoUsesPulseWhenPipeWireIsNotCompiledIn() {
  const auto d =
      ResolveAudioTransport(AudioTransportPreference::kAuto, NotCompiledIn());
  return Expect(d.transport == AudioTransport::kPulse,
                "auto should select PulseAudio in a build without PipeWire");
}

bool TestPulsePreferenceNeverSelectsPipeWire() {
  const auto d =
      ResolveAudioTransport(AudioTransportPreference::kPulse, Available());
  return Expect(d.transport == AudioTransport::kPulse,
                "an explicit pulse preference must stay on PulseAudio") &&
         Expect(!d.used_fallback, "an honoured preference is not a fallback") &&
         Expect(d.note.empty(), "an honoured preference should not set a note");
}

bool TestPipeWirePreferenceFallsBackAndExplainsWhy() {
  const auto d =
      ResolveAudioTransport(AudioTransportPreference::kPipeWire, NoServer());
  return Expect(d.transport == AudioTransport::kPulse,
                "an unavailable PipeWire request must fall back to Pulse") &&
         Expect(d.used_fallback, "the fallback flag should be set") &&
         Expect(d.note.find("PipeWire socket") != std::string::npos,
                "the note should carry the availability reason") &&
         Expect(d.note.find('\n') == std::string::npos,
                "a note goes into JSON and into a banner, so it stays one "
                "line");
}

bool TestAudioTransportPreferenceParsing() {
  const auto pulse = ParseAudioTransportPreference("pulse");
  const auto pw = ParseAudioTransportPreference("PipeWire");
  const auto autoPref = ParseAudioTransportPreference(" auto ");
  const auto pulseAlias = ParseAudioTransportPreference("pulseaudio");
  const auto bad = ParseAudioTransportPreference("jack");
  return Expect(pulse && *pulse == AudioTransportPreference::kPulse,
                "\"pulse\" should parse") &&
         Expect(pw && *pw == AudioTransportPreference::kPipeWire,
                "the parser should ignore letter case") &&
         Expect(autoPref && *autoPref == AudioTransportPreference::kAuto,
                "the parser should ignore surrounding spaces") &&
         Expect(pulseAlias && *pulseAlias == AudioTransportPreference::kPulse,
                "\"pulseaudio\" is an accepted alias") &&
         Expect(!bad.has_value(), "an unknown name must not parse");
}

bool TestCompiledInFlagMatchesTheBuildOption() {
#if STUDIOCAST_HAVE_PIPEWIRE
  return Expect(studiocast::pw::PipeWireCompiledIn(),
                "STUDIOCAST_HAVE_PIPEWIRE=1 must report a PipeWire build");
#else
  return Expect(!studiocast::pw::PipeWireCompiledIn(),
                "STUDIOCAST_HAVE_PIPEWIRE=0 must report a build without "
                "PipeWire");
#endif
}

// Builds a probe environment from a fake variable map and a fake set of
// existing paths.
PipeWireProbeEnv FakeEnv(std::map<std::string, std::string> vars,
                         std::string existing_path) {
  PipeWireProbeEnv env;
  env.get_env = [vars = std::move(vars)](const char *name) -> std::string {
    const auto it = vars.find(name ? name : "");
    return it == vars.end() ? std::string() : it->second;
  };
  env.path_exists = [existing = std::move(existing_path)](
                        const std::string &path) { return path == existing; };
  return env;
}

bool TestSocketProbeUsesTheRuntimeDirectory() {
  const auto p = ProbePipeWireSocket(FakeEnv(
      {{"XDG_RUNTIME_DIR", "/run/user/1000"}}, "/run/user/1000/pipewire-0"));
  return Expect(p.found,
                "the default socket under XDG_RUNTIME_DIR was missed") &&
         Expect(p.path == "/run/user/1000/pipewire-0",
                "unexpected socket path: " + p.path);
}

bool TestSocketProbePrefersPipeWireRuntimeDir() {
  const auto p =
      ProbePipeWireSocket(FakeEnv({{"PIPEWIRE_RUNTIME_DIR", "/custom/pw"},
                                   {"XDG_RUNTIME_DIR", "/run/user/1000"}},
                                  "/custom/pw/pipewire-0"));
  return Expect(p.found,
                "PIPEWIRE_RUNTIME_DIR must win over XDG_RUNTIME_DIR") &&
         Expect(p.path == "/custom/pw/pipewire-0",
                "unexpected socket path: " + p.path);
}

bool TestSocketProbeHonoursPipeWireRemote() {
  const auto p =
      ProbePipeWireSocket(FakeEnv({{"XDG_RUNTIME_DIR", "/run/user/1000"},
                                   {"PIPEWIRE_REMOTE", "pipewire-1"}},
                                  "/run/user/1000/pipewire-1"));
  return Expect(p.found, "PIPEWIRE_REMOTE must name the socket") &&
         Expect(p.path == "/run/user/1000/pipewire-1",
                "unexpected socket path: " + p.path);
}

bool TestSocketProbeReportsAMissingSocket() {
  const auto p = ProbePipeWireSocket(
      FakeEnv({{"XDG_RUNTIME_DIR", "/run/user/1000"}}, "/nowhere"));
  return Expect(!p.found, "a missing socket must not be reported as found") &&
         Expect(p.reason.find("/run/user/1000/pipewire-0") != std::string::npos,
                "the reason should name the path it looked for: " + p.reason);
}

bool TestSocketProbeReportsAMissingRuntimeDirectory() {
  const auto p = ProbePipeWireSocket(FakeEnv({}, "/nowhere"));
  bool ok = Expect(!p.found, "no runtime directory means no socket");
  // The probe reads three variables, so the reason must name all three.
  for (const char *name :
       {"PIPEWIRE_RUNTIME_DIR", "XDG_RUNTIME_DIR", "USERPROFILE"}) {
    ok = Expect(p.reason.find(name) != std::string::npos,
                std::string("the reason should name ") + name + ": " +
                    p.reason) &&
         ok;
  }
  return ok;
}

bool TestNodePropertyArithmetic() {
  using studiocast::pw::AudioFrameBytes;
  using studiocast::pw::AudioRingCapacityBytes;
  using studiocast::pw::NodeLatencyProperty;
  using studiocast::pw::NodeRateProperty;

  return Expect(NodeLatencyProperty(480, 48000) == "480/48000",
                "node.latency must be samples over rate") &&
         Expect(NodeRateProperty(48000) == "1/48000",
                "node.rate must be one over rate") &&
         Expect(AudioFrameBytes(480, 2) == 480u * 2u * sizeof(float),
                "a frame is float32 interleaved") &&
         Expect(AudioRingCapacityBytes(480, 1, 4) ==
                    4u * AudioFrameBytes(480, 1),
                "the ring holds the asked number of frames") &&
         Expect(AudioRingCapacityBytes(480, 1, 1) ==
                    2u * AudioFrameBytes(480, 1),
                "the ring never holds fewer than two frames");
}

bool TestCanonicalNodeNames() {
  using namespace studiocast::pw;
  return Expect(std::string(kVirtualMicNodeName) == "studiocast_mic",
                "the virtual microphone node name must not change") &&
         Expect(std::string(kVirtualMicDescription) == "StudioCast Microphone",
                "the virtual microphone description must not change") &&
         Expect(std::string(kVirtualSpeakerNodeName) == "studiocast_speakers",
                "the virtual speaker node name must not change") &&
         Expect(std::string(kVirtualSpeakerDescription) ==
                    "StudioCast Speakers",
                "the virtual speaker description must not change") &&
         Expect(std::string(kVirtualCameraNodeName) == "studiocast_camera",
                "the virtual camera node name must not change") &&
         Expect(std::string(kVirtualCameraDescription) == "StudioCast Camera",
                "the virtual camera description must not change");
}

// PipeWire gives a stream its node id only after the server has the node, so
// a consumer that links at once is announced before the id is known. Such a
// link must still be counted, otherwise the node reports no consumer while one
// listens.
bool TestLinkCounterCountsALinkSeenBeforeTheNodeId() {
  studiocast::pw::NodeLinkCounter counter;
  counter.Reset(studiocast::pw::LinkEnd::kOutput);

  // The registry announces the link first.
  counter.OnLinkAdded(70, 42, 55);
  const int before = counter.ConsumerCount();

  // The stream then learns its own id.
  counter.SetNodeId(42);

  return Expect(before == 0, "a link cannot be counted without a node id") &&
         Expect(counter.ConsumerCount() == 1,
                "an early link must be counted once the node id arrives");
}

// A link that comes and goes before the node id arrives leaves nothing behind.
bool TestLinkCounterForgetsAnEarlyLinkThatWentAway() {
  studiocast::pw::NodeLinkCounter counter;
  counter.Reset(studiocast::pw::LinkEnd::kOutput);

  counter.OnLinkAdded(70, 42, 55);
  counter.OnGlobalRemoved(70);
  counter.SetNodeId(42);

  return Expect(counter.ConsumerCount() == 0,
                "a link that went away must not be counted later");
}

// A link between two other nodes is not a consumer of this node, whenever it
// is seen.
bool TestLinkCounterIgnoresLinksOfOtherNodes() {
  studiocast::pw::NodeLinkCounter counter;
  counter.Reset(studiocast::pw::LinkEnd::kOutput);

  counter.OnLinkAdded(70, 11, 12);
  counter.SetNodeId(42);
  counter.OnLinkAdded(71, 13, 14);
  // The node is the input end of this one, and it hands out samples, so this
  // is not a consumer link either.
  counter.OnLinkAdded(72, 13, 42);

  return Expect(counter.ConsumerCount() == 0,
                "only the links of this node count");
}

// A node that receives samples, such as the virtual speakers, is the input end
// of a consumer link.
bool TestLinkCounterReadsTheInputEndForASink() {
  studiocast::pw::NodeLinkCounter counter;
  counter.Reset(studiocast::pw::LinkEnd::kInput);

  counter.OnLinkAdded(80, 9, 42);
  counter.SetNodeId(42);
  counter.OnLinkAdded(81, 10, 42);
  const int both = counter.ConsumerCount();

  counter.OnGlobalRemoved(80);
  const int one = counter.ConsumerCount();

  counter.Reset(studiocast::pw::LinkEnd::kInput);

  return Expect(both == 2, "both input links must count") &&
         Expect(one == 1, "a removed link must stop counting") &&
         Expect(counter.ConsumerCount() == 0, "a reset must clear the count");
}

// Runs a command and returns its standard output. An empty result means the
// command failed or printed nothing.
std::string RunCapture(const std::string &cmd) {
  std::string out;
  FILE *p = ::popen(cmd.c_str(), "r");
  if (!p)
    return out;
  std::array<char, 4096> buf{};
  while (std::fgets(buf.data(), static_cast<int>(buf.size()), p) != nullptr)
    out.append(buf.data());
  ::pclose(p);
  return out;
}

// The live-server tests need a reachable server and the pw-dump tool. Without
// both, they report a skip and pass, so continuous integration stays green.
bool LiveServerAvailable(const char *test_name) {
  if (!studiocast::pw::PipeWireCompiledIn()) {
    std::cout << "[SKIP] " << test_name << ": built without PipeWire\n";
    return false;
  }
  if (!studiocast::pw::ProbePipeWireSocket().found) {
    std::cout << "[SKIP] " << test_name << ": no PipeWire server\n";
    return false;
  }
  if (RunCapture("command -v pw-dump 2>/dev/null").empty()) {
    std::cout << "[SKIP] " << test_name << ": pw-dump is not installed\n";
    return false;
  }
  return true;
}

bool TestLiveVirtualSourceNodeReachesTheGraph() {
  static constexpr const char *kName = "studiocast_pipewire_selftest";
  if (!LiveServerAvailable("live virtual source node reaches the graph"))
    return true;

  studiocast::pw::AudioNodeConfig cfg;
  cfg.role = studiocast::pw::AudioNodeRole::kVirtualSource;
  cfg.node_name = kName;
  cfg.node_description = "StudioCast Self Test";
  cfg.channels = 1;

  studiocast::pw::PipeWireAudioNode node;
  std::string error;
  if (!Expect(node.Start(cfg, &error), "node start failed: " + error))
    return false;

  // The node id appears once the server has the node.
  std::uint32_t id = 0;
  for (int i = 0; i < 100 && id == 0; ++i) {
    id = node.NodeId();
    if (id == 0)
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  const std::string dump = RunCapture("pw-dump 2>/dev/null");
  node.Stop();

  return Expect(id != 0, "the node never reached the graph") &&
         Expect(dump.find(kName) != std::string::npos,
                "pw-dump did not list the node name") &&
         Expect(dump.find("StudioCast Self Test") != std::string::npos,
                "pw-dump did not list the node description") &&
         Expect(dump.find("\"Audio/Source\"") != std::string::npos,
                "pw-dump listed no Audio/Source node");
}

bool TestLiveVirtualSourceAcceptsWrites() {
  if (!LiveServerAvailable("live virtual source accepts writes"))
    return true;

  studiocast::pw::AudioNodeConfig cfg;
  cfg.role = studiocast::pw::AudioNodeRole::kVirtualSource;
  cfg.node_name = "studiocast_pipewire_selftest_write";
  cfg.channels = 1;

  studiocast::pw::PipeWireAudioNode node;
  std::string error;
  if (!Expect(node.Start(cfg, &error), "node start failed: " + error))
    return false;

  std::vector<float> frame(cfg.frame_samples, 0.0f);
  bool ok = true;
  for (int i = 0; i < 20 && ok; ++i) {
    ok = node.Write(frame.data(), frame.size() * sizeof(float), &error);
  }
  const int consumers = node.ConsumerCount();
  node.Stop();

  return Expect(ok, "writing into the virtual source failed: " + error) &&
         Expect(consumers >= 0, "the consumer count must never be negative");
}

// A virtual source with no consumer is not driven by the graph, so nothing
// empties its ring. The pipeline thread writes a frame every 10 ms, so a full
// ring must give the frame up quickly instead of holding the thread.
bool TestLiveWriteToAFullRingReturnsQuickly() {
  if (!LiveServerAvailable("live write to a full ring returns quickly"))
    return true;

  studiocast::pw::AudioNodeConfig cfg;
  cfg.role = studiocast::pw::AudioNodeRole::kVirtualSource;
  cfg.node_name = "studiocast_pipewire_selftest_full_ring";
  cfg.channels = 1;

  studiocast::pw::PipeWireAudioNode node;
  std::string error;
  if (!Expect(node.Start(cfg, &error), "node start failed: " + error))
    return false;

  // More writes than the ring holds, so the last ones find it full.
  std::vector<float> frame(cfg.frame_samples, 0.0f);
  const std::size_t bytes = frame.size() * sizeof(float);
  bool ok = true;
  std::chrono::steady_clock::duration slowest{0};
  for (int i = 0; i < cfg.ring_frames * 4 && ok; ++i) {
    const auto started = std::chrono::steady_clock::now();
    ok = node.Write(frame.data(), bytes, &error);
    slowest = std::max(slowest, std::chrono::steady_clock::now() - started);
  }
  const std::uint64_t overflows = node.OverflowCount();
  node.Stop();

  const auto slowest_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(slowest).count();
  return Expect(ok, "a dropped frame must still report success: " + error) &&
         Expect(overflows > 0,
                "a ring that nothing empties must count an overflow") &&
         Expect(slowest_ms <= 10,
                "a write waited " + std::to_string(slowest_ms) +
                    " ms; a full ring must not stall the pipeline thread");
}

bool TestPipeWireIoRefusesToOpenWithoutTheVirtualMic() {
  auto io = studiocast::audio::pw_backend::CreatePipeWireAudioIo();
  if (!Expect(io != nullptr, "the PipeWire I/O factory returned nothing"))
    return false;

  studiocast::audio::AudioPipelineConfig cfg;
  cfg.sink_name = "studiocast_sink";
  std::string error;
  const bool opened = io->Open(cfg, &error);
  if (opened)
    io->RequestStop();

  return Expect(!opened, "opening without a virtual microphone must fail") &&
         Expect(!error.empty(), "the failure must explain itself");
}

bool TestLiveNativeVirtualMicRoundTrip() {
  if (!LiveServerAvailable("live native virtual mic round trip"))
    return true;

  auto &devices = studiocast::audio::pw_backend::NativeAudioDevices::Instance();
  std::string error;
  if (!Expect(devices.CreateVirtualMic(&error),
              "creating the native virtual microphone failed: " + error))
    return false;

  const std::string dump = RunCapture("pw-dump 2>/dev/null");
  const auto consumers = devices.DetectMicrophoneConsumers();
  const bool created = devices.MicNode() != nullptr;

  bool wrote = false;
  if (created) {
    std::vector<float> frame(480, 0.0f);
    wrote = devices.MicNode()->Write(frame.data(), frame.size() * sizeof(float),
                                     &error);
  }

  (void)devices.DestroyVirtualMic(&error);

  return Expect(created, "the virtual microphone node was not created") &&
         Expect(dump.find("studiocast_mic") != std::string::npos,
                "pw-dump did not list studiocast_mic") &&
         Expect(dump.find("StudioCast Microphone") != std::string::npos,
                "pw-dump did not list the canonical description") &&
         Expect(wrote,
                "writing into the virtual microphone failed: " + error) &&
         Expect(consumers.error.empty(),
                "consumer detection reported an error: " + consumers.error);
}

bool TestServiceTransportFollowsTheConfiguredPreference() {
  studiocast::audio::VirtualAudioServiceConfig cfg;
  cfg.transport = AudioTransportPreference::kPulse;
  const auto pulse = studiocast::audio::ResolveServiceAudioTransport(cfg);

  cfg.transport = AudioTransportPreference::kPipeWire;
  const auto native = studiocast::audio::ResolveServiceAudioTransport(cfg);

  const bool nativeUsable = studiocast::pw::PipeWireCompiledIn() &&
                            studiocast::pw::ProbePipeWireSocket().found;

  return Expect(pulse.transport == AudioTransport::kPulse,
                "a pulse preference must stay on PulseAudio") &&
         Expect(native.transport == (nativeUsable ? AudioTransport::kPipeWire
                                                  : AudioTransport::kPulse),
                "a pipewire preference must follow real availability");
}

bool TestServiceTransportDefaultsToPulse() {
  // PulseAudio stays the default so an upgrade changes nothing on a machine
  // that already works, PipeWire hosts included.
  const studiocast::audio::VirtualAudioServiceConfig cfg;
  return Expect(cfg.transport == AudioTransportPreference::kPulse,
                "the service default preference must be pulse");
}

bool TestDaemonConfigRoundTripsTheAudioBackendKey() {
  namespace fs = std::filesystem;
  const fs::path root =
      fs::temp_directory_path() / "studiocast-pipewire-config-test";
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);

  const std::string previous =
      std::getenv("XDG_CONFIG_HOME") ? std::getenv("XDG_CONFIG_HOME") : "";
  ::setenv("XDG_CONFIG_HOME", root.c_str(), 1);

  studiocast::config::DaemonConfig cfg;
  const bool defaultsToPulse = cfg.audio_backend == "pulse";
  cfg.audio_backend = "pipewire";

  std::string error;
  const bool saved = studiocast::config::SaveDaemonConfig(cfg, &error);
  const auto loaded = studiocast::config::LoadDaemonConfig();
  const auto service = studiocast::config::ToAudioServiceConfig(loaded);

  if (previous.empty())
    ::unsetenv("XDG_CONFIG_HOME");
  else
    ::setenv("XDG_CONFIG_HOME", previous.c_str(), 1);
  fs::remove_all(root, ec);

  return Expect(defaultsToPulse,
                "the daemon config default backend must be pulse") &&
         Expect(saved, "saving the daemon config failed: " + error) &&
         Expect(loaded.audio_backend == "pipewire",
                "the audio backend key did not survive a save and load") &&
         Expect(service.transport == AudioTransportPreference::kPipeWire,
                "the service config did not take the backend preference");
}

bool TestVideoOutputBackendParsing() {
  using studiocast::pw::ParseVideoOutputPreference;
  using studiocast::pw::VideoOutputPreference;

  return Expect(ParseVideoOutputPreference("auto") ==
                    VideoOutputPreference::kAuto,
                "\"auto\" should parse") &&
         Expect(ParseVideoOutputPreference("V4L2Loopback") ==
                    VideoOutputPreference::kV4l2Loopback,
                "the parser should ignore letter case") &&
         Expect(ParseVideoOutputPreference(" both ") ==
                    VideoOutputPreference::kBoth,
                "the parser should ignore surrounding spaces") &&
         Expect(!ParseVideoOutputPreference("gstreamer").has_value(),
                "an unknown name must not parse");
}

bool TestVideoOutputBackendSelection() {
  using studiocast::pw::ResolveVideoOutputBackends;
  using studiocast::pw::VideoOutputPreference;

  const auto autoPick =
      ResolveVideoOutputBackends(VideoOutputPreference::kAuto, Available());
  const auto both =
      ResolveVideoOutputBackends(VideoOutputPreference::kBoth, Available());
  const auto nativeOnly =
      ResolveVideoOutputBackends(VideoOutputPreference::kPipeWire, Available());
  const auto nativeMissing =
      ResolveVideoOutputBackends(VideoOutputPreference::kPipeWire, NoServer());

  return Expect(autoPick.backends.v4l2loopback && !autoPick.backends.pipewire,
                "auto must keep v4l2loopback alone, because most video "
                "conference applications read V4L2 only") &&
         Expect(both.backends.v4l2loopback && both.backends.pipewire,
                "both must enable the two outputs") &&
         Expect(both.note.empty(),
                "both gets what it asks for, so it needs no note") &&
         Expect(nativeOnly.backends.v4l2loopback &&
                    nativeOnly.backends.pipewire,
                "pipewire must keep v4l2loopback, because the loopback is "
                "still the format source and node-only output is not "
                "implemented") &&
         Expect(nativeOnly.used_fallback,
                "pipewire gives more than it was asked for, so it is a "
                "fallback") &&
         Expect(!nativeOnly.note.empty(),
                "pipewire must say why the loopback is still there") &&
         Expect(nativeOnly.note.find('\n') == std::string::npos,
                "the pipewire note goes into JSON and into a banner, so it "
                "stays one line") &&
         Expect(nativeMissing.backends.v4l2loopback &&
                    !nativeMissing.backends.pipewire,
                "an unavailable server must fall back to v4l2loopback") &&
         Expect(nativeMissing.used_fallback, "the fallback flag should be set") &&
         Expect(!nativeMissing.note.empty(),
                "the fallback must explain itself") &&
         Expect(nativeMissing.note.find('\n') == std::string::npos,
                "a note goes into JSON and into a banner, so it stays one "
                "line");
}

bool TestCameraFrameByteArithmetic() {
  using studiocast::video::PixelFormat;
  using studiocast::video::pw_backend::CameraFrameBytes;
  return Expect(CameraFrameBytes(1280, 720, PixelFormat::rgb24) ==
                    1280u * 720u * 3u,
                "rgb24 is three bytes a pixel") &&
         Expect(CameraFrameBytes(1280, 720, PixelFormat::yuyv) ==
                    1280u * 720u * 2u,
                "yuyv is two bytes a pixel");
}

bool TestLiveVirtualCameraNodeReachesTheGraph() {
  if (!LiveServerAvailable("live virtual camera node reaches the graph"))
    return true;

  studiocast::video::pw_backend::CameraNodeConfig cfg;
  cfg.node_name = "studiocast_camera_selftest";
  cfg.node_description = "StudioCast Camera Self Test";
  cfg.width = 320;
  cfg.height = 240;
  cfg.fps = 30;

  studiocast::video::pw_backend::PipeWireCameraNode node;
  std::string error;
  if (!Expect(node.Start(cfg, &error), "camera node start failed: " + error))
    return false;

  std::uint32_t id = 0;
  for (int i = 0; i < 100 && id == 0; ++i) {
    id = node.NodeId();
    if (id == 0)
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  std::vector<std::uint8_t> frame(
      studiocast::video::pw_backend::CameraFrameBytes(
          cfg.width, cfg.height, cfg.format),
      0x20);
  bool wrote = true;
  for (int i = 0; i < 10 && wrote; ++i)
    wrote = node.WriteFrame(frame.data(), frame.size(), &error);

  const std::string dump = RunCapture("pw-dump 2>/dev/null");
  const int consumers = node.ConsumerCount();
  node.Stop();

  return Expect(id != 0, "the camera node never reached the graph") &&
         Expect(wrote, "staging a frame failed: " + error) &&
         Expect(dump.find("studiocast_camera_selftest") != std::string::npos,
                "pw-dump did not list the camera node") &&
         Expect(dump.find("\"Video/Source\"") != std::string::npos,
                "pw-dump listed no Video/Source node") &&
         Expect(consumers >= 0, "the consumer count must never be negative");
}

bool TestCameraNodeRejectsAShortFrame() {
  studiocast::video::pw_backend::CameraNodeConfig cfg;
  cfg.width = 64;
  cfg.height = 64;
  studiocast::video::pw_backend::PipeWireCameraNode node;
  std::string error;
  const bool wrote = node.WriteFrame(nullptr, 0, &error);
  return Expect(!wrote, "a stopped node must refuse a frame") &&
         Expect(!error.empty(), "the refusal must explain itself");
}

// Proves the node is a working PipeWire camera, not only a graph entry: a
// real consumer must link to it and receive the staged frames.
bool TestLiveVirtualCameraFeedsAGstreamerConsumer() {
  static constexpr const char *kName = "studiocast_camera_gsttest";
  if (!LiveServerAvailable("live virtual camera feeds a GStreamer consumer"))
    return true;
  if (RunCapture("command -v gst-launch-1.0 2>/dev/null").empty()) {
    std::cout << "[SKIP] live virtual camera feeds a GStreamer consumer: "
                 "gst-launch-1.0 is not installed\n";
    return true;
  }

  studiocast::video::pw_backend::CameraNodeConfig cfg;
  cfg.node_name = kName;
  cfg.width = 160;
  cfg.height = 120;
  cfg.fps = 30;

  studiocast::video::pw_backend::PipeWireCameraNode node;
  std::string error;
  if (!Expect(node.Start(cfg, &error), "camera node start failed: " + error))
    return false;

  std::atomic<bool> stop{false};
  // The consumer link disappears when GStreamer exits, so the count has to be
  // sampled while it runs.
  std::atomic<int> peak_consumers{0};
  // A frame is dropped only when it replaces a frame the callback never took.
  // The consumer takes one frame per staged frame here, so the count must stay
  // near zero. It only means something while the consumer runs, so it is
  // sampled as soon as a few frames have gone out.
  std::atomic<std::uint64_t> drops_while_streaming{0};
  std::atomic<bool> drops_sampled{false};
  const std::size_t bytes = studiocast::video::pw_backend::CameraFrameBytes(
      cfg.width, cfg.height, cfg.format);
  std::thread feeder([&] {
    std::vector<std::uint8_t> frame(bytes, 0x40);
    std::string err;
    while (!stop.load(std::memory_order_acquire)) {
      (void)node.WriteFrame(frame.data(), frame.size(), &err);
      const int seen = node.ConsumerCount();
      if (seen > peak_consumers.load(std::memory_order_relaxed))
        peak_consumers.store(seen, std::memory_order_relaxed);
      if (!drops_sampled.load(std::memory_order_relaxed) &&
          node.FramesSent() >= 3) {
        drops_while_streaming.store(node.FramesDropped(),
                                    std::memory_order_relaxed);
        drops_sampled.store(true, std::memory_order_relaxed);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  });

  const std::string cmd =
      std::string("timeout 15 gst-launch-1.0 -q pipewiresrc target-object=") +
      kName + " num-buffers=5 ! videoconvert ! fakesink 2>&1; echo rc=$?";
  const std::string out = RunCapture(cmd);
  const std::uint64_t sent = node.FramesSent();

  stop.store(true, std::memory_order_release);
  feeder.join();
  const int consumers = peak_consumers.load(std::memory_order_relaxed);
  const bool sampled = drops_sampled.load(std::memory_order_relaxed);
  const std::uint64_t drops =
      drops_while_streaming.load(std::memory_order_relaxed);
  node.Stop();

  // Two allows for the writer staging a second frame before a cycle runs.
  const bool drops_ok = !sampled || drops <= 2;

  return Expect(out.find("rc=0") != std::string::npos,
                "the GStreamer consumer did not finish cleanly: " + out) &&
         Expect(sent > 0, "the node handed no frame to a consumer") &&
         Expect(consumers > 0, "no consumer link was counted") &&
         Expect(drops_ok, "a consumer that keeps up must not see drops, got " +
                              std::to_string(drops));
}

// Restores the real pactl runner when the test ends.
class ScopedPactlHook final {
public:
  explicit ScopedPactlHook(studiocast::audio::pulse::PactlExecCaptureHook hook) {
    studiocast::audio::pulse::SetPactlExecCaptureHookForTesting(
        std::move(hook));
  }
  ~ScopedPactlHook() {
    studiocast::audio::pulse::SetPactlExecCaptureHookForTesting(nullptr);
  }
};

// A module list that holds every leftover the Pulse device path can make,
// plus two modules the cleanup must not touch.
const char *kStaleModuleList =
    "536870916\tmodule-null-sink\tsink_name=studiocast_sink "
    "sink_properties=device.description=StudioCast\n"
    "536870917\tmodule-remap-source\tmaster=studiocast_sink.monitor "
    "source_name=studiocast_mic\n"
    "536870918\tmodule-null-sink\tsink_name=studiocast_speakers\n"
    "536870919\tmodule-loopback\tsource=studiocast_speakers.monitor "
    "sink=alsa_output.pci-0000_00_1f.3.analog-stereo latency_msec=10\n"
    "536870920\tmodule-loopback\tsink=studiocast_sink "
    "source=physical_test_mic latency_msec=10\n"
    "536870921\tmodule-loopback\tsource=studiocast_mic "
    "sink=alsa_output.pci-0000_00_1f.3.analog-stereo latency_msec=20 "
    "source_output_properties=media.name=StudioCast_Microphone_Monitor\n"
    "536870922\tmodule-null-sink\tsink_name=other_app_sink\n";

bool TestStalePulseModuleDetectionSkipsTheMonitorAndOtherApps() {
  ScopedPactlHook hook([](const std::string &command) {
    if (command == "pactl --version 2>&1")
      return studiocast::util::ExecResult{0, false, "pactl 17.0\n"};
    if (command == "pactl list short modules 2>&1")
      return studiocast::util::ExecResult{0, false, kStaleModuleList};
    return studiocast::util::ExecResult{99, false, "unexpected command: " + command};
  });

  std::string error;
  const auto stale =
      studiocast::audio::pw_backend::DetectStalePulseDeviceModules(&error);

  std::vector<int> ids;
  for (const auto &m : stale)
    ids.push_back(m.id);

  const std::vector<int> want{536870919, 536870920, 536870917, 536870916,
                              536870918};
  return Expect(error.empty(), "detection reported an error: " + error) &&
         Expect(ids == want,
                "unexpected stale module ids or order; got " +
                    [&ids] {
                      std::string s;
                      for (int id : ids)
                        s += std::to_string(id) + " ";
                      return s;
                    }());
}

bool TestStalePulseModuleCleanupIssuesTheRightPactlCalls() {
  std::vector<std::string> commands;
  ScopedPactlHook hook([&commands](const std::string &command) {
    commands.push_back(command);
    if (command == "pactl --version 2>&1")
      return studiocast::util::ExecResult{0, false, "pactl 17.0\n"};
    if (command == "pactl list short modules 2>&1")
      return studiocast::util::ExecResult{0, false, kStaleModuleList};
    if (command.rfind("pactl unload-module ", 0) == 0)
      return studiocast::util::ExecResult{0, false, ""};
    return studiocast::util::ExecResult{99, false, "unexpected command: " + command};
  });

  std::vector<std::string> removed;
  std::string error;
  const bool ok = studiocast::audio::pw_backend::UnloadStalePulseDeviceModules(
      &removed, &error);

  std::vector<std::string> unloads;
  for (const auto &c : commands) {
    if (c.rfind("pactl unload-module ", 0) == 0)
      unloads.push_back(c);
  }

  // Loopbacks go first, then the remap source, then the null sinks it used,
  // so a module is never unloaded while another one still needs it.
  const std::vector<std::string> want{
      "pactl unload-module 536870919 2>&1", "pactl unload-module 536870920 2>&1",
      "pactl unload-module 536870917 2>&1", "pactl unload-module 536870916 2>&1",
      "pactl unload-module 536870918 2>&1"};

  bool keptMonitor = true;
  bool keptOtherApp = true;
  for (const auto &c : unloads) {
    if (c == "pactl unload-module 536870921 2>&1")
      keptMonitor = false;
    if (c == "pactl unload-module 536870922 2>&1")
      keptOtherApp = false;
  }

  return Expect(ok, "cleanup failed: " + error) &&
         Expect(unloads == want, "unexpected unload command lines") &&
         Expect(keptMonitor,
                "the microphone monitor loopback must survive the cleanup") &&
         Expect(keptOtherApp,
                "a null sink of another application must survive") &&
         Expect(removed.size() == 5,
                "the cleanup log should name every removed module") &&
         Expect(removed[0].find("module-loopback") != std::string::npos,
                "the log should name the module type: " + removed[0]);
}

bool TestStalePulseModuleCleanupIsQuietWhenNothingIsLoaded() {
  std::vector<std::string> commands;
  ScopedPactlHook hook([&commands](const std::string &command) {
    commands.push_back(command);
    if (command == "pactl --version 2>&1")
      return studiocast::util::ExecResult{0, false, "pactl 17.0\n"};
    if (command == "pactl list short modules 2>&1")
      return studiocast::util::ExecResult{
          0, false, "536870922\tmodule-null-sink\tsink_name=other_app_sink\n"};
    return studiocast::util::ExecResult{99, false, "unexpected command: " + command};
  });

  std::vector<std::string> removed;
  std::string error;
  const bool ok = studiocast::audio::pw_backend::UnloadStalePulseDeviceModules(
      &removed, &error);

  bool anyUnload = false;
  for (const auto &c : commands) {
    if (c.rfind("pactl unload-module ", 0) == 0)
      anyUnload = true;
  }

  return Expect(ok, "cleanup failed: " + error) &&
         Expect(!anyUnload, "nothing should be unloaded") &&
         Expect(removed.empty(), "the cleanup log should stay empty");
}

bool TestPulseBackendTearsDownNativeNodes() {
  auto &devices = studiocast::audio::pw_backend::NativeAudioDevices::Instance();

  // On a machine with a server this really creates a node; without one the
  // create fails and the teardown still has to be safe.
  std::string error;
  (void)devices.CreateVirtualMic(&error);
  (void)devices.CreateVirtualSpeaker(&error);

  studiocast::audio::pw_backend::ShutdownNativeAudioDevices();
  const bool firstClean =
      devices.MicNode() == nullptr && devices.SpeakerNode() == nullptr;

  // A second call must be a safe no-op, because the service calls it on every
  // start that resolves to PulseAudio.
  studiocast::audio::pw_backend::ShutdownNativeAudioDevices();

  return Expect(firstClean,
                "switching to PulseAudio must take the native nodes down") &&
         Expect(devices.MicNode() == nullptr && devices.SpeakerNode() == nullptr,
                "a repeated teardown must stay clean");
}

bool TestMicMonitorCleanupIgnoresNativePathNames() {
  // The native path names its streams after StudioCast too. None of them is a
  // Pulse module, but a name-only match would still be a trap, so pin that the
  // monitor needs its own stream tag before it removes anything.
  ScopedPactlHook hook([](const std::string &command) {
    if (command == "pactl --version 2>&1")
      return studiocast::util::ExecResult{0, false, "pactl 17.0\n"};
    if (command == "pactl list short modules 2>&1") {
      return studiocast::util::ExecResult{
          0, false,
          "10\tmodule-loopback\tsource=studiocast_mic "
          "sink=studiocast_speakers_route\n"
          "11\tmodule-loopback\tsource=studiocast_mic_capture "
          "sink=alsa_output.analog-stereo\n"
          "12\tmodule-null-sink\tsink_name=studiocast_speakers_out\n"
          "13\tmodule-loopback\tsource=studiocast_mic "
          "sink=alsa_output.analog-stereo "
          "source_output_properties=media.name=StudioCast_Microphone_Monitor\n"};
    }
    return studiocast::util::ExecResult{99, false,
                                        "unexpected command: " + command};
  });

  std::string error;
  const auto ids = studiocast::audio::DetectMicMonitorModuleIds(&error);
  const std::vector<int> want{13};
  return Expect(error.empty(), "detection reported an error: " + error) &&
         Expect(ids == want,
                "the monitor must claim only its own tagged loopback");
}

// A node that the server took down never comes back by itself, so the state
// rule must report it. Anything on the way up must not read as down.
bool TestStreamStateRuleReportsADownNode() {
  using studiocast::pw::StreamState;
  using studiocast::pw::StreamWentDown;

  bool ok = Expect(StreamWentDown(StreamState::kStreaming, StreamState::kError),
                   "an error on a streaming node should read as down");
  ok = Expect(StreamWentDown(StreamState::kPaused, StreamState::kError),
              "an error on a paused node should read as down") &&
       ok;
  ok = Expect(StreamWentDown(StreamState::kConnecting, StreamState::kError),
              "an error while connecting should read as down") &&
       ok;
  ok =
      Expect(StreamWentDown(StreamState::kStreaming, StreamState::kUnconnected),
             "a streaming node that the server disconnected should read as "
             "down") &&
      ok;
  ok = Expect(
           StreamWentDown(StreamState::kPaused, StreamState::kUnconnected),
           "a paused node that the server disconnected should read as down") &&
       ok;
  ok = Expect(
           StreamWentDown(StreamState::kConnecting, StreamState::kUnconnected),
           "a connection attempt that gave up should read as down") &&
       ok;

  ok = Expect(
           !StreamWentDown(StreamState::kUnconnected, StreamState::kConnecting),
           "the start of a connection should not read as down") &&
       ok;
  ok = Expect(!StreamWentDown(StreamState::kConnecting, StreamState::kPaused),
              "a connected node should not read as down") &&
       ok;
  ok = Expect(!StreamWentDown(StreamState::kPaused, StreamState::kStreaming),
              "a streaming node should not read as down") &&
       ok;
  ok = Expect(!StreamWentDown(StreamState::kStreaming, StreamState::kPaused),
              "a node that stopped streaming is still connected") &&
       ok;
  ok = Expect(!StreamWentDown(StreamState::kUnconnected,
                              StreamState::kUnconnected),
              "a node that never connected should not read as down") &&
       ok;
  ok = Expect(!StreamWentDown(StreamState::kError, StreamState::kUnconnected),
              "a node that is already down should not read as down again") &&
       ok;
  return ok;
}

// One frame of the ring stress check: the same counter twice, so a torn or
// overwritten frame does not read as a whole one.
struct RingFrame {
  std::uint32_t first = 0;
  std::uint32_t second = 0;
};

// The ring drops the data the producer holds when it is full, and never the
// data it already took. Only the consumer may move the read end, so a full
// ring has no other way to make room.
// A ring that nobody sized yet holds nothing, so both ends must say no
// instead of dividing by its zero size.
bool TestRingRefusesWorkBeforeItIsSized() {
  studiocast::pw::SpscByteRing ring;
  std::array<std::uint8_t, 4> data{1, 2, 3, 4};

  const bool pushedNothing = ring.Push(data.data(), 0);
  const bool pushedSomething = ring.Push(data.data(), data.size());
  const bool poppedNothing = ring.Pop(data.data(), 0);
  const bool poppedSomething = ring.Pop(data.data(), data.size());

  return Expect(!pushedNothing, "an unsized ring must refuse an empty push") &&
         Expect(!pushedSomething, "an unsized ring must refuse a push") &&
         Expect(!poppedNothing, "an unsized ring must refuse an empty pop") &&
         Expect(!poppedSomething, "an unsized ring must refuse a pop") &&
         Expect(ring.Capacity() == 0, "an unsized ring holds nothing");
}

bool TestRingDropsTheNewestWhenItIsFull() {
  studiocast::pw::SpscByteRing ring;
  ring.Reset(8);

  std::array<std::uint8_t, 8> oldest{};
  for (std::size_t i = 0; i < oldest.size(); ++i)
    oldest[i] = static_cast<std::uint8_t>(i + 1);

  bool ok = Expect(ring.Capacity() == 8, "the ring should hold 8 bytes");
  ok = Expect(ring.Push(oldest.data(), oldest.size()),
              "an empty ring should take a full write") &&
       ok;
  ok = Expect(ring.Writable() == 0, "a full ring should offer no room") && ok;

  const std::array<std::uint8_t, 4> newest = {0xEE, 0xEE, 0xEE, 0xEE};
  ok = Expect(!ring.Push(newest.data(), newest.size()),
              "a full ring should refuse the new bytes") &&
       ok;
  ok = Expect(ring.Readable() == oldest.size(),
              "a refused write should leave the ring as it was") &&
       ok;

  std::array<std::uint8_t, 8> back{};
  ok = Expect(ring.Pop(back.data(), back.size()),
              "the ring should give its bytes back") &&
       ok;
  ok = Expect(back == oldest, "the ring should keep the bytes it took") && ok;
  ok = Expect(ring.Readable() == 0, "the ring should be empty again") && ok;

  // Room comes back only from the consumer, and then a write fits again.
  ok = Expect(ring.Push(newest.data(), newest.size()),
              "an emptied ring should take a write again") &&
       ok;
  ok = Expect(ring.Readable() == newest.size(),
              "the ring should hold the new bytes") &&
       ok;
  return ok;
}

// A write and a read that both run over the end of the buffer must keep the
// byte order.
bool TestRingWrapsWithoutLosingOrder() {
  studiocast::pw::SpscByteRing ring;
  ring.Reset(8);

  std::array<std::uint8_t, 6> first{};
  for (std::size_t i = 0; i < first.size(); ++i)
    first[i] = static_cast<std::uint8_t>(0x10 + i);
  std::array<std::uint8_t, 6> second{};
  for (std::size_t i = 0; i < second.size(); ++i)
    second[i] = static_cast<std::uint8_t>(0x20 + i);

  bool ok = Expect(ring.Push(first.data(), first.size()), "first write");
  std::array<std::uint8_t, 6> back{};
  ok = Expect(ring.Pop(back.data(), back.size()), "first read") && ok;
  ok = Expect(back == first, "the first write should come back whole") && ok;

  // The write end now sits 6 bytes into a 9 byte buffer, so this write wraps.
  ok = Expect(ring.Push(second.data(), second.size()), "wrapped write") && ok;
  ok = Expect(ring.Readable() == second.size(),
              "the wrapped write should be readable whole") &&
       ok;
  ok = Expect(ring.Pop(back.data(), back.size()), "wrapped read") && ok;
  ok =
      Expect(back == second, "a wrapped write should come back in order") && ok;
  return ok;
}

// One producer and one consumer, with a ring far too small for the traffic, so
// the overflow path runs again and again. Every frame the consumer reads must
// be whole, and the frames must arrive in the order the producer wrote them.
// A second writer of the read end would break both.
bool TestRingSurvivesAProducerAndAConsumer() {
  studiocast::pw::SpscByteRing ring;
  ring.Reset(sizeof(RingFrame) * 4);

  constexpr std::uint32_t kFrames = 200000;
  std::atomic<std::uint32_t> dropped{0};
  std::atomic<bool> producer_done{false};

  std::thread producer([&] {
    for (std::uint32_t i = 1; i <= kFrames; ++i) {
      const RingFrame frame{i, i};
      if (!ring.Push(&frame, sizeof(frame)))
        dropped.fetch_add(1, std::memory_order_relaxed);
    }
    producer_done.store(true, std::memory_order_release);
  });

  std::uint32_t last = 0;
  std::uint32_t taken = 0;
  bool torn = false;
  bool out_of_order = false;
  bool too_full = false;

  while (true) {
    if (ring.Readable() > ring.Capacity())
      too_full = true;

    RingFrame frame{};
    if (!ring.Pop(&frame, sizeof(frame))) {
      if (producer_done.load(std::memory_order_acquire) &&
          ring.Readable() < sizeof(RingFrame)) {
        break;
      }
      std::this_thread::yield();
      continue;
    }
    ++taken;
    if (frame.first != frame.second)
      torn = true;
    if (frame.first <= last)
      out_of_order = true;
    last = frame.first;
  }

  producer.join();

  bool ok = Expect(!torn, "the consumer read a frame that was written twice");
  ok = Expect(!out_of_order,
              "the consumer read the frames out of the written order") &&
       ok;
  ok = Expect(!too_full, "the ring reported more bytes than it can hold") && ok;
  ok = Expect(taken + dropped.load(std::memory_order_relaxed) == kFrames,
              "every frame should be either read or counted as dropped") &&
       ok;
  ok = Expect(taken > 0, "the consumer should have read something") && ok;
  return ok;
}

} // namespace

int main() {
  const struct {
    const char *name;
    bool (*fn)();
  } tests[] = {
      {"auto prefers PipeWire when the server is reachable",
       &TestAutoPrefersPipeWireWhenTheServerIsReachable},
      {"auto uses Pulse when no server is reachable",
       &TestAutoUsesPulseWhenNoServerIsReachable},
      {"auto uses Pulse when PipeWire is not compiled in",
       &TestAutoUsesPulseWhenPipeWireIsNotCompiledIn},
      {"pulse preference never selects PipeWire",
       &TestPulsePreferenceNeverSelectsPipeWire},
      {"pipewire preference falls back and explains why",
       &TestPipeWirePreferenceFallsBackAndExplainsWhy},
      {"audio transport preference parsing",
       &TestAudioTransportPreferenceParsing},
      {"compiled-in flag matches the build option",
       &TestCompiledInFlagMatchesTheBuildOption},
      {"socket probe uses the runtime directory",
       &TestSocketProbeUsesTheRuntimeDirectory},
      {"socket probe prefers PIPEWIRE_RUNTIME_DIR",
       &TestSocketProbePrefersPipeWireRuntimeDir},
      {"socket probe honours PIPEWIRE_REMOTE",
       &TestSocketProbeHonoursPipeWireRemote},
      {"socket probe reports a missing socket",
       &TestSocketProbeReportsAMissingSocket},
      {"socket probe reports a missing runtime directory",
       &TestSocketProbeReportsAMissingRuntimeDirectory},
      {"stream state rule reports a down node",
       &TestStreamStateRuleReportsADownNode},
      {"ring refuses work before it is sized",
       &TestRingRefusesWorkBeforeItIsSized},
      {"ring drops the newest bytes when it is full",
       &TestRingDropsTheNewestWhenItIsFull},
      {"ring wraps without losing the byte order",
       &TestRingWrapsWithoutLosingOrder},
      {"ring survives a producer and a consumer",
       &TestRingSurvivesAProducerAndAConsumer},
      {"node property arithmetic", &TestNodePropertyArithmetic},
      {"link counter counts a link seen before the node id",
       &TestLinkCounterCountsALinkSeenBeforeTheNodeId},
      {"link counter forgets an early link that went away",
       &TestLinkCounterForgetsAnEarlyLinkThatWentAway},
      {"link counter ignores links of other nodes",
       &TestLinkCounterIgnoresLinksOfOtherNodes},
      {"link counter reads the input end for a sink",
       &TestLinkCounterReadsTheInputEndForASink},
      {"live virtual source node reaches the graph",
       &TestLiveVirtualSourceNodeReachesTheGraph},
      {"live virtual source accepts writes",
       &TestLiveVirtualSourceAcceptsWrites},
      {"live write to a full ring returns quickly",
       &TestLiveWriteToAFullRingReturnsQuickly},
      {"pipewire io refuses to open without the virtual mic",
       &TestPipeWireIoRefusesToOpenWithoutTheVirtualMic},
      {"live native virtual mic round trip",
       &TestLiveNativeVirtualMicRoundTrip},
      {"service transport follows the configured preference",
       &TestServiceTransportFollowsTheConfiguredPreference},
      {"service transport defaults to pulse",
       &TestServiceTransportDefaultsToPulse},
      {"daemon config round trips the audio backend key",
       &TestDaemonConfigRoundTripsTheAudioBackendKey},
      {"stale pulse module detection skips the monitor and other apps",
       &TestStalePulseModuleDetectionSkipsTheMonitorAndOtherApps},
      {"stale pulse module cleanup issues the right pactl calls",
       &TestStalePulseModuleCleanupIssuesTheRightPactlCalls},
      {"stale pulse module cleanup is quiet when nothing is loaded",
       &TestStalePulseModuleCleanupIsQuietWhenNothingIsLoaded},
      {"pulse backend tears down native nodes",
       &TestPulseBackendTearsDownNativeNodes},
      {"mic monitor cleanup ignores native path names",
       &TestMicMonitorCleanupIgnoresNativePathNames},
      {"canonical node names", &TestCanonicalNodeNames},
      {"video output backend parsing", &TestVideoOutputBackendParsing},
      {"video output backend selection", &TestVideoOutputBackendSelection},
      {"camera frame byte arithmetic", &TestCameraFrameByteArithmetic},
      {"camera node rejects a short frame", &TestCameraNodeRejectsAShortFrame},
      {"live virtual camera node reaches the graph",
       &TestLiveVirtualCameraNodeReachesTheGraph},
      {"live virtual camera feeds a GStreamer consumer",
       &TestLiveVirtualCameraFeedsAGstreamerConsumer},
  };

  int failed = 0;
  for (const auto &test : tests) {
    const bool ok = test.fn();
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << test.name << "\n";
    if (!ok)
      ++failed;
  }

  return failed == 0 ? 0 : 1;
}
