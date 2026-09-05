#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sys/resource.h>
#include <unistd.h>

#include "core/audio/pipewire/pipewire_audio_devices.h"
#include "core/audio/mic_monitor.h"
#include "core/audio/pulse/pactl.h"
#include "core/audio/virtual_audio_service.h"
#include "core/config/daemon_config.h"
#include "core/pipewire/pipewire_audio_node.h"
#include "core/video/pipewire/pipewire_camera_node.h"

#include "core/pipewire/pipewire_support.h"
#include "core/pipewire/spsc_byte_ring.h"
#include "core/pipewire/triple_frame_buffer.h"

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
  env.path_exists = [existing = existing_path](const std::string &path) {
    return path == existing;
  };
  env.socket_answers = [existing = std::move(existing_path)](
                           const std::string &path) {
    return path == existing;
  };
  return env;
}

// A server that died leaves its socket file behind. StudioCast must not take
// that file for a running server, or it commits to the native backend and the
// fallback to PulseAudio never happens.
bool TestSocketProbeReportsASocketNobodyAnswers() {
  auto env = FakeEnv({{"XDG_RUNTIME_DIR", "/run/user/1000"}},
                     "/run/user/1000/pipewire-0");
  env.socket_answers = [](const std::string &) { return false; };

  const auto p = ProbePipeWireSocket(env);
  return Expect(!p.found, "a socket that nobody answers must not count") &&
         Expect(p.reason.find("no server answers") != std::string::npos,
                "the reason must say nobody answered: " + p.reason) &&
         Expect(p.reason.find("/run/user/1000/pipewire-0") != std::string::npos,
                "the reason must name the socket: " + p.reason);
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

// The pump reads the virtual speakers, and a read comes back with nothing
// whenever no application plays into them. It must wait before it asks again,
// or the thread burns a whole core for as long as the route is up.
bool TestSpeakerLoopbackPumpWaitsAfterAnEmptyRead() {
  int checks = 0;
  int reads = 0;
  int writes = 0;
  int waits = 0;

  studiocast::audio::pw_backend::internal::SpeakerLoopbackPumpHooks hooks;
  hooks.cancelled = [&checks] { return ++checks > 3; };
  hooks.read_frame = [&reads] {
    ++reads;
    return false;
  };
  hooks.write_frame = [&writes] {
    ++writes;
    return true;
  };
  hooks.backoff = [&waits] { ++waits; };

  studiocast::audio::pw_backend::internal::RunSpeakerLoopbackPump(hooks);

  return Expect(reads == 3, "the pump should have read on every pass") &&
         Expect(waits == 3, "a read that found nothing must be followed by a "
                            "wait, otherwise the pump spins") &&
         Expect(writes == 0, "the pump must not write a frame it never read");
}

// A frame that arrived goes straight on, with no wait in between, so the route
// keeps the latency the node was configured for.
bool TestSpeakerLoopbackPumpPassesOnTheFrameItRead() {
  int checks = 0;
  int reads = 0;
  int writes = 0;
  int waits = 0;

  studiocast::audio::pw_backend::internal::SpeakerLoopbackPumpHooks hooks;
  hooks.cancelled = [&checks] { return ++checks > 3; };
  hooks.read_frame = [&reads] {
    ++reads;
    return true;
  };
  hooks.write_frame = [&writes] {
    ++writes;
    return true;
  };
  hooks.backoff = [&waits] { ++waits; };

  studiocast::audio::pw_backend::internal::RunSpeakerLoopbackPump(hooks);

  return Expect(reads == 3, "the pump should have read on every pass") &&
         Expect(writes == 3, "every frame that arrived must be written") &&
         Expect(waits == 0, "a pump that moves frames must not wait");
}

// A read or a write that fails must say why. Start clears the last error, and
// a stop request sets none, so both used to fail with an empty message and the
// caller logged nothing.
bool TestLiveStoppedNodeSaysWhyItRefusesWork() {
  if (!LiveServerAvailable("live stopped node says why it refuses work"))
    return true;

  studiocast::pw::AudioNodeConfig cfg;
  cfg.role = studiocast::pw::AudioNodeRole::kVirtualSource;
  cfg.node_name = "studiocast_pipewire_selftest_stopped";
  cfg.channels = 1;

  studiocast::pw::PipeWireAudioNode node;
  std::string error;
  if (!Expect(node.Start(cfg, &error), "node start failed: " + error))
    return false;

  node.RequestStop();

  std::vector<float> frame(cfg.frame_samples, 0.0f);
  const std::size_t bytes = frame.size() * sizeof(float);
  std::string readError;
  std::string writeError;
  const bool read = node.Read(frame.data(), bytes, &readError);
  const bool wrote = node.Write(frame.data(), bytes, &writeError);
  node.Stop();

  return Expect(!read, "a stopped node must refuse a read") &&
         Expect(!wrote, "a stopped node must refuse a write") &&
         Expect(!readError.empty(), "the refused read explained nothing") &&
         Expect(!writeError.empty(), "the refused write explained nothing");
}

// The service pins the format of its virtual devices, and the pipeline brings
// its own. A mismatch would push stereo bytes through a mono ring and produce
// garbled audio with no complaint, so the pipeline must be refused instead.
bool TestAudioFormatMismatchNamesTheDifference() {
  studiocast::pw::AudioNodeConfig node;
  node.sample_rate = 48000;
  node.channels = 1;
  node.frame_samples = 480;

  const std::string same =
      studiocast::audio::pw_backend::internal::AudioFormatMismatch(
          "virtual microphone", node, 48000, 1, 480);
  const std::string rate =
      studiocast::audio::pw_backend::internal::AudioFormatMismatch(
          "virtual microphone", node, 44100, 1, 480);
  const std::string channels =
      studiocast::audio::pw_backend::internal::AudioFormatMismatch(
          "virtual microphone", node, 48000, 2, 480);
  const std::string frame =
      studiocast::audio::pw_backend::internal::AudioFormatMismatch(
          "virtual microphone", node, 48000, 1, 960);

  auto names = [](const std::string &message, const char *a, const char *b) {
    return message.find(a) != std::string::npos &&
           message.find(b) != std::string::npos;
  };

  return Expect(same.empty(), "a matching format must be accepted: " + same) &&
         Expect(names(rate, "44100", "48000"),
                "a rate mismatch must name both rates: " + rate) &&
         Expect(names(channels, "2", "1"),
                "a channel mismatch must name both counts: " + channels) &&
         Expect(names(frame, "960", "480"),
                "a frame mismatch must name both sizes: " + frame) &&
         Expect(rate.find("virtual microphone") != std::string::npos,
                "the message must name the device: " + rate);
}

// Starting a node runs pw_context_connect and pw_stream_connect, which is a
// full round trip to the PipeWire server. The daemon polls the device status
// on every tick and takes the device lock for it, so a create that held that
// lock for the trip would stall the status for as long as the server takes.
bool TestDeviceCreateLeavesTheDeviceLockFreeWhileItBuilds() {
  using studiocast::audio::pw_backend::internal::CreateDeviceOutsideLock;

  std::mutex create_mu;
  std::mutex device_mu;
  bool lock_was_free = false;
  bool built = false;
  bool published = false;

  const bool made = CreateDeviceOutsideLock(
      create_mu, device_mu, [] { return false; },
      [&] {
        built = true;
        // Another thread stands for the daemon status poll. It must be able
        // to take the device lock while the server is answering.
        std::thread poll([&] {
          if (device_mu.try_lock()) {
            lock_was_free = true;
            device_mu.unlock();
          }
        });
        poll.join();
        return true;
      },
      [&] { published = true; });

  // A device that is already there answers without building anything.
  bool built_again = false;
  const bool had = CreateDeviceOutsideLock(
      create_mu, device_mu, [] { return true; },
      [&] {
        built_again = true;
        return true;
      },
      [] {});

  return Expect(made, "a create that builds a device should answer true") &&
         Expect(built, "the build step should run") &&
         Expect(lock_was_free,
                "the device lock must be free while the node starts") &&
         Expect(published, "the new device should be published") &&
         Expect(had, "a device that is already there is the answer") &&
         Expect(!built_again,
                "a device that is already there must not be built again");
}

// The create gives the device lock up while the server answers. A destroy that
// took the device lock alone could run in that window: it would move a null
// pointer out, answer true, and then be undone by the publish step, which
// installs the node the caller was told had gone.
bool TestDeviceDestroyWaitsForACreateInFlight() {
  using studiocast::audio::pw_backend::internal::CreateDeviceOutsideLock;
  using studiocast::audio::pw_backend::internal::DestroyDeviceOutsideLock;

  std::mutex create_mu;
  std::mutex device_mu;
  std::mutex order_mu;
  std::string order;
  const auto note = [&](const char *what) {
    std::lock_guard<std::mutex> lock(order_mu);
    order += what;
  };

  bool device = false;
  std::atomic<bool> entered{false};
  std::thread destroyer;

  const bool made = CreateDeviceOutsideLock(
      create_mu, device_mu, [&] { return device; },
      [&] {
        // Stands for the server round trip of a node start. A destroy that
        // arrives now must not finish before the create it interleaves with.
        destroyer = std::thread([&] {
          entered.store(true, std::memory_order_release);
          DestroyDeviceOutsideLock(create_mu, device_mu, [&] {
            device = false;
            note("destroy ");
          });
        });
        while (!entered.load(std::memory_order_acquire))
          std::this_thread::yield();
        // The window the destroy needs to get in. It gives the other thread
        // time; it is not a bound on how long any step may take.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        note("build ");
        return true;
      },
      [&] {
        device = true;
        note("publish ");
      });

  destroyer.join();

  return Expect(made, "a create that builds a device should answer true") &&
         Expect(order == "build publish destroy ",
                "a destroy must not land inside a create; the order was: " +
                    order) &&
         Expect(!device, "the destroy must leave the device gone");
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

// The suffix every device-level test gives its nodes. A daemon on this machine
// owns `studiocast_mic` and `studiocast_speakers`, so a test must never create
// a second node under those names, and must never assert on them: the daemon's
// node would answer for it.
constexpr const char *kTestDeviceSuffix = "_devtest";

// Gives the process device owner the test identities and takes the daemon's
// sound server out of its reach, then puts the defaults back.
class ScopedTestDeviceOptions {
public:
  ScopedTestDeviceOptions()
      : previous_(
            studiocast::audio::pw_backend::NativeAudioDevices::Instance()
                .Options()) {
    studiocast::audio::pw_backend::NativeAudioDeviceOptions options;
    options.node_name_suffix = kTestDeviceSuffix;
    options.remove_stale_pulse_devices = false;
    studiocast::audio::pw_backend::NativeAudioDevices::Instance().SetOptions(
        options);
  }

  ~ScopedTestDeviceOptions() {
    studiocast::audio::pw_backend::NativeAudioDevices::Instance().SetOptions(
        previous_);
  }

  ScopedTestDeviceOptions(const ScopedTestDeviceOptions &) = delete;
  ScopedTestDeviceOptions &operator=(const ScopedTestDeviceOptions &) = delete;

private:
  studiocast::audio::pw_backend::NativeAudioDeviceOptions previous_;
};

// Waits until the server has given the node an id.
std::uint32_t WaitForNodeId(const studiocast::pw::PipeWireAudioNode &node) {
  for (int i = 0; i < 100; ++i) {
    const std::uint32_t id = node.NodeId();
    if (id != 0)
      return id;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return 0;
}

// A server that goes away takes every node with it, and the daemon then tries
// to put them back. The wait between attempts grows, so a server that stays
// away is not asked again on every supervisor poll.
bool TestNodeRestartDelayBacksOff() {
  using studiocast::pw::NodeRestartDelay;
  const auto ms = [](int attempts) {
    return NodeRestartDelay(attempts).count();
  };

  return Expect(ms(0) == 0, "the first attempt must not wait") &&
         Expect(ms(1) == 500, "the second attempt waits half a second") &&
         Expect(ms(2) == 1000, "the wait doubles") &&
         Expect(ms(3) == 2000, "the wait doubles again") &&
         Expect(ms(20) == 8000, "the wait must stop at the cap") &&
         Expect(ms(-3) == 0, "a negative count must not wait");
}

// A node the server destroys never comes back by itself. The device owner has
// to notice and make a new one, or the daemon reports a microphone that
// carries nothing until it restarts.
bool TestLiveDeviceComesBackAfterTheServerDropsIt() {
  if (!LiveServerAvailable("live device comes back after the server drops it"))
    return true;
  if (RunCapture("command -v pw-cli 2>/dev/null").empty()) {
    std::cout << "[SKIP] live device comes back after the server drops it: "
                 "pw-cli is not installed\n";
    return true;
  }

  ScopedTestDeviceOptions options;
  auto &devices = studiocast::audio::pw_backend::NativeAudioDevices::Instance();
  std::string error;
  if (!Expect(devices.CreateVirtualMic(&error),
              "creating the native virtual microphone failed: " + error))
    return false;

  const std::uint32_t first = WaitForNodeId(*devices.MicNode());
  const bool healthyAtFirst = !devices.MicWentDown();

  // Take the node out of the graph the way a server restart would.
  (void)RunCapture("pw-cli destroy " + std::to_string(first) +
                   " 2>/dev/null");

  bool noticed = false;
  for (int i = 0; i < 200 && !noticed; ++i) {
    noticed = devices.MicWentDown();
    if (!noticed)
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }

  const bool recreated = devices.CreateVirtualMic(&error);
  const std::uint32_t second =
      recreated && devices.MicNode() ? WaitForNodeId(*devices.MicNode()) : 0;
  const bool healthyAgain = recreated && !devices.MicWentDown();

  (void)devices.DestroyVirtualMic(&error);

  return Expect(first != 0, "the first node never reached the graph") &&
         Expect(healthyAtFirst, "a fresh node must not read as down") &&
         Expect(noticed, "the owner never noticed the node had gone") &&
         Expect(recreated, "creating the device again failed: " + error) &&
         Expect(second != 0, "the new node never reached the graph") &&
         Expect(second != first,
                "the owner kept the node the server dropped") &&
         Expect(healthyAgain, "the new node reads as down");
}

bool TestLiveNativeVirtualMicRoundTrip() {
  if (!LiveServerAvailable("live native virtual mic round trip"))
    return true;

  ScopedTestDeviceOptions options;
  auto &devices = studiocast::audio::pw_backend::NativeAudioDevices::Instance();
  std::string error;
  if (!Expect(devices.CreateVirtualMic(&error),
              "creating the native virtual microphone failed: " + error))
    return false;

  const bool created = devices.MicNode() != nullptr;
  std::uint32_t id = 0;
  bool wrote = false;
  if (created) {
    id = WaitForNodeId(*devices.MicNode());
    std::vector<float> frame(480, 0.0f);
    wrote = devices.MicNode()->Write(frame.data(), frame.size() * sizeof(float),
                                     &error);
  }

  // Ask the server about the one node this test made, by the id it got. A
  // grep of the whole graph would be answered by the daemon's own node.
  const std::string dump =
      id == 0 ? std::string()
              : RunCapture("pw-dump " + std::to_string(id) + " 2>/dev/null");
  const auto consumers = devices.DetectMicrophoneConsumers();

  (void)devices.DestroyVirtualMic(&error);

  const std::string wantName =
      std::string(studiocast::pw::kVirtualMicNodeName) + kTestDeviceSuffix;
  const std::string wantDescription =
      std::string(studiocast::pw::kVirtualMicDescription) + kTestDeviceSuffix;
  return Expect(created, "the virtual microphone node was not created") &&
         Expect(id != 0, "the virtual microphone never reached the graph") &&
         Expect(dump.find(wantName) != std::string::npos,
                "pw-dump of the node id did not list " + wantName) &&
         Expect(dump.find(wantDescription) != std::string::npos,
                "pw-dump of the node id did not list " + wantDescription) &&
         Expect(wrote,
                "writing into the virtual microphone failed: " + error) &&
         Expect(consumers.error.empty(),
                "consumer detection reported an error: " + consumers.error);
}

// Processor time this process has used, in milliseconds. A pump that spins
// shows up here and nowhere else.
long ProcessCpuMs() {
  struct rusage usage {};
  if (::getrusage(RUSAGE_SELF, &usage) != 0)
    return 0;
  const auto ms = [](const struct timeval &t) {
    return static_cast<long>(t.tv_sec) * 1000 + t.tv_usec / 1000;
  };
  return ms(usage.ru_utime) + ms(usage.ru_stime);
}

// The virtual speakers belong to the service and outlive every route. A stop
// and a second start must leave them able to carry samples, and the pump they
// feed must sit still while no application plays into them.
bool TestLiveSpeakerLoopbackCycleLeavesTheSpeakersUsable() {
  if (!LiveServerAvailable("live speaker loopback cycle leaves the speakers "
                           "usable"))
    return true;

  ScopedTestDeviceOptions options;
  auto &devices = studiocast::audio::pw_backend::NativeAudioDevices::Instance();
  std::string error;
  if (!Expect(devices.CreateVirtualSpeaker(&error),
              "creating the native virtual speakers failed: " + error))
    return false;

  // Nothing plays into the virtual speakers, so a read must wait out its
  // timeout and say that it timed out. A node that carries a stop request
  // comes back at once with nothing to say.
  std::vector<float> frame(480 * 2, 0.0f);
  const std::size_t bytes = frame.size() * sizeof(float);
  auto readOnce = [&](std::string *err) {
    const auto started = std::chrono::steady_clock::now();
    (void)devices.SpeakerNode()->Read(frame.data(), bytes, err);
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now() - started)
        .count();
  };

  std::string firstError;
  const auto firstMs = readOnce(&firstError);

  // The route the daemon starts for a pass-through: an empty target sink name
  // means the default device.
  bool started = devices.StartSpeakerLoopback("", &error);
  const std::string firstStartError = error;
  bool stopped = started && devices.StopSpeakerLoopback(&error);
  const std::string stopError = error;
  bool restarted = stopped && devices.StartSpeakerLoopback("", &error);
  const std::string secondStartError = error;

  // With the route up again and nothing playing, the pump must be waiting,
  // not asking the node millions of times a second.
  long cpuMs = 0;
  if (restarted) {
    const long before = ProcessCpuMs();
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    cpuMs = ProcessCpuMs() - before;
  }

  (void)devices.StopSpeakerLoopback(&error);

  std::string secondError;
  const auto secondMs = readOnce(&secondError);

  (void)devices.DestroyVirtualSpeaker(&error);

  return Expect(started,
                "starting the speaker route failed: " + firstStartError) &&
         Expect(stopped, "stopping the speaker route failed: " + stopError) &&
         Expect(restarted,
                "starting the speaker route again failed: " +
                    secondStartError) &&
         Expect(firstMs >= 100,
                "a read of an idle node must wait; it took " +
                    std::to_string(firstMs) + " ms") &&
         Expect(secondMs >= 100,
                "a read after a route cycle must still wait; it took " +
                    std::to_string(secondMs) + " ms") &&
         Expect(secondError == firstError,
                "a route cycle changed what a read reports: '" + firstError +
                    "' became '" + secondError + "'") &&
         Expect(cpuMs < 100,
                "the pump used " + std::to_string(cpuMs) +
                    " ms of processor time in 400 ms; it is spinning");
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

// A frame of `value` bytes, so a consumer can tell one frame from another and
// can see a copy that two writers tore apart.
std::vector<std::uint8_t> FrameOf(std::size_t bytes, std::uint8_t value) {
  return std::vector<std::uint8_t>(bytes, value);
}

bool TestFrameBufferHandsTheNewestFrameOver() {
  studiocast::pw::TripleFrameBuffer buffer;
  buffer.Reset(4);

  const auto first = FrameOf(4, 0x11);
  const auto second = FrameOf(4, 0x22);

  const bool nothingBefore = buffer.Acquire() == nullptr;
  (void)buffer.Publish(first.data(), first.size());
  const std::uint8_t after_first = *buffer.Acquire();
  (void)buffer.Publish(second.data(), second.size());
  const std::uint8_t after_second = *buffer.Acquire();

  return Expect(nothingBefore,
                "a buffer with no frame must hand out nothing") &&
         Expect(after_first == 0x11, "the first frame did not come back") &&
         Expect(after_second == 0x22, "the second frame did not come back");
}

// A cycle without a new frame repeats the last one, so a consumer keeps its
// timing instead of receiving black.
bool TestFrameBufferRepeatsTheLastFrame() {
  studiocast::pw::TripleFrameBuffer buffer;
  buffer.Reset(4);

  const auto frame = FrameOf(4, 0x33);
  (void)buffer.Publish(frame.data(), frame.size());

  const std::uint8_t first = *buffer.Acquire();
  const std::uint8_t again = *buffer.Acquire();
  const std::uint8_t once_more = *buffer.Acquire();

  return Expect(first == 0x33 && again == 0x33 && once_more == 0x33,
                "a repeated read must give the last frame again");
}

// The newest frame wins. A publish over a frame nobody took is the only case
// that loses one, and that is the case the camera counts as a drop.
bool TestFrameBufferReportsAFrameThatWasNeverTaken() {
  studiocast::pw::TripleFrameBuffer buffer;
  buffer.Reset(4);

  const auto first = FrameOf(4, 0x44);
  const auto second = FrameOf(4, 0x55);
  const auto third = FrameOf(4, 0x66);

  const bool lostOnFirst = buffer.Publish(first.data(), first.size());
  const bool lostOnSecond = buffer.Publish(second.data(), second.size());
  const std::uint8_t taken = *buffer.Acquire();
  const bool lostOnThird = buffer.Publish(third.data(), third.size());

  return Expect(!lostOnFirst, "the first frame replaced nothing") &&
         Expect(lostOnSecond,
                "a frame published over an untaken one must be reported") &&
         Expect(taken == 0x55, "the newest frame must win") &&
         Expect(!lostOnThird,
                "a publish after the consumer caught up loses nothing");
}

// The hand-off carries whole frames between two threads with no lock. A torn
// frame would hold the bytes of two writes at once.
bool TestFrameBufferSurvivesAProducerAndAConsumer() {
  constexpr std::size_t kFrameBytes = 4096;
  constexpr int kFrames = 20000;

  studiocast::pw::TripleFrameBuffer buffer;
  buffer.Reset(kFrameBytes);

  std::atomic<bool> done{false};
  std::atomic<std::uint64_t> dropped{0};
  std::thread producer([&] {
    std::vector<std::uint8_t> frame(kFrameBytes, 0);
    for (int i = 0; i < kFrames; ++i) {
      const auto value = static_cast<std::uint8_t>(i % 251);
      std::fill(frame.begin(), frame.end(), value);
      if (buffer.Publish(frame.data(), frame.size()))
        dropped.fetch_add(1, std::memory_order_relaxed);
    }
    done.store(true, std::memory_order_release);
  });

  bool torn = false;
  int taken = 0;
  while (!done.load(std::memory_order_acquire)) {
    const std::uint8_t *frame = buffer.Acquire();
    if (!frame)
      continue;
    ++taken;
    for (std::size_t i = 1; i < kFrameBytes; ++i) {
      if (frame[i] != frame[0]) {
        torn = true;
        break;
      }
    }
    if (torn)
      break;
  }

  producer.join();

  return Expect(!torn,
                "the consumer read a frame that two writes tore apart") &&
         Expect(taken > 0, "the consumer should have read something");
}

// The pipeline hands the node its own output buffer, whose rows sit
// bytes_per_line apart. A copy that took the bytes in one run would read every
// row after the first from the wrong offset, and the error grows down the
// frame: the picture a consumer sees is sheared.
bool TestFrameBufferDropsTheRowPadding() {
  constexpr std::size_t kRowBytes = 6;
  constexpr std::size_t kRows = 4;
  constexpr std::size_t kSourceStride = kRowBytes + 5;

  // Row y holds the byte y + 1, and the padding holds a byte no row uses. A
  // copy that ignored the padding would shift row 1 onwards.
  std::vector<std::uint8_t> source(kSourceStride * kRows, 0xee);
  for (std::size_t y = 0; y < kRows; ++y) {
    for (std::size_t x = 0; x < kRowBytes; ++x)
      source[y * kSourceStride + x] = static_cast<std::uint8_t>(y + 1);
  }

  studiocast::pw::TripleFrameBuffer buffer;
  buffer.Reset(kRowBytes * kRows);
  (void)buffer.PublishRows(source.data(), source.size(), kSourceStride,
                           kRowBytes, kRows);

  const std::uint8_t *frame = buffer.Acquire();
  if (!Expect(frame != nullptr, "the consumer should have a frame"))
    return false;

  for (std::size_t y = 0; y < kRows; ++y) {
    for (std::size_t x = 0; x < kRowBytes; ++x) {
      const std::uint8_t got = frame[y * kRowBytes + x];
      if (!Expect(got == static_cast<std::uint8_t>(y + 1),
                  "row " + std::to_string(y) + " byte " + std::to_string(x) +
                      " is " + std::to_string(static_cast<int>(got)) +
                      ", so the padding was copied as picture"))
        return false;
    }
  }

  // A frame too short for the last row must not be read past its end. A
  // refused frame offers nothing, so the consumer still holds the frame above.
  std::vector<std::uint8_t> other(source.size(), 0x77);
  (void)buffer.PublishRows(other.data(),
                           kSourceStride * (kRows - 1) + kRowBytes - 1,
                           kSourceStride, kRowBytes, kRows);
  return Expect(buffer.Acquire()[0] == 1,
                "a frame shorter than the last row must be refused");
}

// The node offers one format only, so a negotiated format that differs is a
// dead node: the callback stops before it answers SPA_PARAM_Buffers, the
// stream never gets data ports and no consumer ever receives a frame.
bool TestCameraNegotiatedFormatMismatchNamesTheDifference() {
  using studiocast::video::pw_backend::internal::CameraFormatMismatch;
  constexpr std::uint32_t kOffered = 7;
  constexpr std::uint32_t kOther = 9;

  if (!Expect(CameraFormatMismatch(kOffered, 1280, 720, kOffered, 1280u, 720u)
                  .empty(),
              "the format the node offered is not a mismatch"))
    return false;

  const std::string size =
      CameraFormatMismatch(kOffered, 1280, 720, kOffered, 640u, 480u);
  const std::string format =
      CameraFormatMismatch(kOffered, 1280, 720, kOther, 1280u, 720u);
  return Expect(size.find("640x480") != std::string::npos,
                "a different size must name what the server picked: " + size) &&
         Expect(size.find("1280x720") != std::string::npos,
                "a different size must name what the node offered: " + size) &&
         Expect(format.find(std::to_string(kOther)) != std::string::npos,
                "a different format must name what the server picked: " +
                    format);
}

bool TestCameraFrameByteArithmetic() {
  using studiocast::video::PixelFormat;
  using studiocast::video::pw_backend::CameraFrameBytes;
  using studiocast::video::pw_backend::CameraStrideBytes;
  return Expect(CameraFrameBytes(1280, 720, PixelFormat::rgb24) ==
                    1280u * 720u * 3u,
                "rgb24 is three bytes a pixel") &&
         Expect(CameraFrameBytes(1280, 720, PixelFormat::yuyv) ==
                    1280u * 720u * 2u,
                "yuyv is two bytes a pixel") &&
         // YUYV writes the last pixel pair whole, so an odd width fills two
         // more bytes than the pixel count asks for. The node must count the
         // row the same way the loopback writer does, or a frame of an odd
         // width reaches a consumer sheared.
         Expect(CameraStrideBytes(1281, PixelFormat::yuyv) ==
                    studiocast::video::MinBytesPerLine(1281, PixelFormat::yuyv),
                "an odd yuyv row must match the writer row") &&
         Expect(CameraFrameBytes(641, 480, PixelFormat::yuyv) ==
                    studiocast::video::MinBytesPerLine(641, PixelFormat::yuyv) *
                        480u,
                "an odd yuyv frame is its row size times its height");
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
  // Before a consumer links there is no callback at all, so every frame the
  // writer stages while GStreamer starts up is a drop and says nothing about
  // the hand-off. Only the drops between the first frame the node handed out
  // and a few frames later are counted.
  std::atomic<std::uint64_t> drops_at_first_frame{0};
  std::atomic<std::uint64_t> drops_while_streaming{0};
  std::atomic<bool> drops_started{false};
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
      const std::uint64_t sent_now = node.FramesSent();
      if (!drops_started.load(std::memory_order_relaxed) && sent_now >= 1) {
        drops_at_first_frame.store(node.FramesDropped(),
                                   std::memory_order_relaxed);
        drops_started.store(true, std::memory_order_relaxed);
      } else if (drops_started.load(std::memory_order_relaxed) &&
                 !drops_sampled.load(std::memory_order_relaxed) &&
                 sent_now >= 4) {
        drops_while_streaming.store(
            node.FramesDropped() -
                drops_at_first_frame.load(std::memory_order_relaxed),
            std::memory_order_relaxed);
        drops_sampled.store(true, std::memory_order_relaxed);
      }
      // One frame period. A faster writer would drop frames the consumer
      // never asked for, which says nothing about the hand-off.
      std::this_thread::sleep_for(std::chrono::milliseconds(1000 / cfg.fps));
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

// Proves the frame layout survives the hand-off, which the test above cannot
// see: it fills every byte alike, so a row read from the wrong offset looks the
// same as a correct one.
//
// The pipeline gives the node its own output buffer, whose rows are
// bytes_per_line apart. Here the source rows carry a byte each and the padding
// carries a byte no row uses, so a consumer that receives a sheared frame
// fails this.
bool TestLiveVirtualCameraKeepsTheRowLayout() {
  static constexpr const char *kName = "studiocast_camera_layout_probe";
  static constexpr int kWidth = 160;
  static constexpr int kHeight = 120;
  static constexpr int kFrames = 8;
  // 160 * 3 is a multiple of four, so GStreamer keeps the rows packed too and
  // the bytes in the file are the bytes the node handed out.
  static constexpr std::size_t kRowBytes =
      static_cast<std::size_t>(kWidth) * 3u;
  static constexpr std::size_t kFrameBytes =
      kRowBytes * static_cast<std::size_t>(kHeight);
  static constexpr std::size_t kPadding = 16u;
  static constexpr std::uint8_t kPaddingByte = 0xee;

  if (!LiveServerAvailable("live virtual camera keeps the row layout"))
    return true;
  if (RunCapture("command -v gst-launch-1.0 2>/dev/null").empty()) {
    std::cout << "[SKIP] live virtual camera keeps the row layout: "
                 "gst-launch-1.0 is not installed\n";
    return true;
  }

  studiocast::video::pw_backend::CameraNodeConfig cfg;
  cfg.node_name = kName;
  cfg.width = kWidth;
  cfg.height = kHeight;
  cfg.fps = 30;
  cfg.format = studiocast::video::PixelFormat::rgb24;
  cfg.stride_bytes = kRowBytes + kPadding;

  studiocast::video::pw_backend::PipeWireCameraNode node;
  std::string error;
  if (!Expect(node.Start(cfg, &error), "camera node start failed: " + error))
    return false;

  // Row y holds the byte y + 1 everywhere, so one byte says which row a
  // consumer really got.
  std::vector<std::uint8_t> source(cfg.stride_bytes *
                                       static_cast<std::size_t>(kHeight),
                                   kPaddingByte);
  for (int y = 0; y < kHeight; ++y) {
    std::uint8_t *row = source.data() + static_cast<std::size_t>(y) *
                                            cfg.stride_bytes;
    std::fill(row, row + kRowBytes, static_cast<std::uint8_t>(y + 1));
  }

  std::atomic<bool> stop{false};
  std::thread feeder([&] {
    std::string err;
    while (!stop.load(std::memory_order_acquire)) {
      (void)node.WriteFrame(source.data(), source.size(), &err);
      std::this_thread::sleep_for(std::chrono::milliseconds(1000 / cfg.fps));
    }
  });

  const std::filesystem::path out_file =
      std::filesystem::temp_directory_path() /
      ("studiocast_camera_layout_probe_" + std::to_string(::getpid()) + ".raw");
  const std::string cmd =
      std::string("timeout 20 gst-launch-1.0 pipewiresrc target-object=") +
      kName + " num-buffers=" + std::to_string(kFrames) +
      " ! video/x-raw,format=RGB ! filesink location=" + out_file.string() +
      " 2>&1; echo rc=$?";
  const std::string gst_out = RunCapture(cmd);

  stop.store(true, std::memory_order_release);
  feeder.join();
  node.Stop();

  std::vector<std::uint8_t> got;
  {
    std::ifstream in(out_file, std::ios::binary);
    got.assign(std::istreambuf_iterator<char>(in),
               std::istreambuf_iterator<char>());
  }
  std::error_code ec;
  std::filesystem::remove(out_file, ec);

  if (!Expect(gst_out.find("rc=0") != std::string::npos,
              "the GStreamer consumer did not finish cleanly: " + gst_out))
    return false;
  if (!Expect(got.size() >= kFrameBytes,
              "the consumer wrote " + std::to_string(got.size()) +
                  " bytes, less than one frame"))
    return false;

  // The first cycles can run before the feeder stages anything, and those
  // frames are black by design. One frame that carries the pattern whole is
  // what proves the layout.
  std::string first_bad;
  for (std::size_t f = 0; f + kFrameBytes <= got.size(); f += kFrameBytes) {
    const std::uint8_t *frame = got.data() + f;
    bool matches = true;
    for (int y = 0; y < kHeight && matches; ++y) {
      for (std::size_t x = 0; x < kRowBytes; ++x) {
        if (frame[static_cast<std::size_t>(y) * kRowBytes + x] !=
            static_cast<std::uint8_t>(y + 1)) {
          matches = false;
          if (first_bad.empty()) {
            first_bad = "row " + std::to_string(y) + " byte " +
                        std::to_string(x) + " is " +
                        std::to_string(static_cast<int>(
                            frame[static_cast<std::size_t>(y) * kRowBytes + x]));
          }
          break;
        }
      }
    }
    if (matches)
      return true;
  }

  return Expect(false, "no frame reached the consumer with its rows in place: " +
                           first_bad);
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

// `--audio-backend` is a flag for one run. Saving an audio setting from the
// GUI must not turn it into the value in the config file, the way the matching
// video flag never does.
bool TestSavingAudioSettingsLeavesTheBackendKeyAlone() {
  studiocast::config::DaemonConfig cfg;
  cfg.audio_backend = "pulse";

  studiocast::audio::VirtualAudioServiceConfig service;
  service.transport = studiocast::pw::AudioTransportPreference::kPipeWire;
  service.source_name = "physical_test_mic";

  studiocast::config::ApplyAudioServiceConfigToDaemonConfig(service, &cfg);

  return Expect(cfg.audio_backend == "pulse",
                "a run-time transport must not reach the config file; got " +
                    cfg.audio_backend) &&
         Expect(cfg.audio_source == "physical_test_mic",
                "the settings the user really changed must be saved");
}

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

// The clean-up unloads modules, so a name that only starts the same must not
// match. Every module below belongs to another application whose names begin
// with the StudioCast ones.
bool TestStalePulseModuleDetectionMatchesWholeArgumentValues() {
  ScopedPactlHook hook([](const std::string &command) {
    if (command == "pactl --version 2>&1")
      return studiocast::util::ExecResult{0, false, "pactl 17.0\n"};
    if (command == "pactl list short modules 2>&1") {
      return studiocast::util::ExecResult{
          0, false,
          "20\tmodule-null-sink\tsink_name=studiocast_sink_backup\n"
          "21\tmodule-null-sink\tsink_name=studiocast_speakersfoo\n"
          "22\tmodule-remap-source\tsource_name=studiocast_mic2\n"
          "23\tmodule-loopback\tsink=studiocast_sinkfoo latency_msec=10\n"
          "24\tmodule-loopback\tsource=studiocast_speakers.monitor.other\n"};
    }
    return studiocast::util::ExecResult{99, false,
                                        "unexpected command: " + command};
  });

  std::string error;
  const auto stale =
      studiocast::audio::pw_backend::DetectStalePulseDeviceModules(&error);

  std::string ids;
  for (const auto &m : stale)
    ids += std::to_string(m.id) + " ";

  return Expect(error.empty(), "detection reported an error: " + error) &&
         Expect(stale.empty(),
                "a name that only starts the same must not match; got " + ids);
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
  if (!LiveServerAvailable("pulse backend tears down native nodes"))
    return true;

  ScopedTestDeviceOptions options;
  auto &devices = studiocast::audio::pw_backend::NativeAudioDevices::Instance();

  // The nodes carry the test suffix, so this creates and removes nodes of its
  // own, never the ones a running daemon owns.
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
// A flush on a node that StudioCast writes into is applied by the real-time
// callback, which may be a long time later. It must drop what the ring held
// when the flush was asked for and nothing that arrived after it.
bool TestRingDiscardsOnlyWhatItWasAskedFor() {
  studiocast::pw::SpscByteRing ring;
  ring.Reset(8);

  const std::array<std::uint8_t, 4> older{1, 2, 3, 4};
  const std::array<std::uint8_t, 4> newer{5, 6, 7, 8};
  (void)ring.Push(older.data(), older.size());
  const std::size_t at_flush = ring.Readable();
  (void)ring.Push(newer.data(), newer.size());

  ring.Discard(at_flush);
  const std::size_t left = ring.Readable();

  std::array<std::uint8_t, 4> out{};
  const bool popped = ring.Pop(out.data(), out.size());

  ring.Discard(1000);
  const std::size_t emptied = ring.Readable();

  return Expect(left == newer.size(),
                "a discard must leave what arrived after the flush") &&
         Expect(popped && out == newer,
                "the bytes left must be the ones written after the flush") &&
         Expect(emptied == 0,
                "a discard of more than the ring holds empties it");
}

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
      {"socket probe reports a socket nobody answers",
       &TestSocketProbeReportsASocketNobodyAnswers},
      {"socket probe reports a missing socket",
       &TestSocketProbeReportsAMissingSocket},
      {"socket probe reports a missing runtime directory",
       &TestSocketProbeReportsAMissingRuntimeDirectory},
      {"stream state rule reports a down node",
       &TestStreamStateRuleReportsADownNode},
      {"ring discards only what it was asked for",
       &TestRingDiscardsOnlyWhatItWasAskedFor},
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
      {"speaker loopback pump waits after an empty read",
       &TestSpeakerLoopbackPumpWaitsAfterAnEmptyRead},
      {"speaker loopback pump passes on the frame it read",
       &TestSpeakerLoopbackPumpPassesOnTheFrameItRead},
      {"live stopped node says why it refuses work",
       &TestLiveStoppedNodeSaysWhyItRefusesWork},
      {"audio format mismatch names the difference",
       &TestAudioFormatMismatchNamesTheDifference},
      {"device create leaves the device lock free while it builds",
       &TestDeviceCreateLeavesTheDeviceLockFreeWhileItBuilds},
      {"device destroy waits for a create in flight",
       &TestDeviceDestroyWaitsForACreateInFlight},
      {"pipewire io refuses to open without the virtual mic",
       &TestPipeWireIoRefusesToOpenWithoutTheVirtualMic},
      {"node restart delay backs off", &TestNodeRestartDelayBacksOff},
      {"live device comes back after the server drops it",
       &TestLiveDeviceComesBackAfterTheServerDropsIt},
      {"live native virtual mic round trip",
       &TestLiveNativeVirtualMicRoundTrip},
      {"live speaker loopback cycle leaves the speakers usable",
       &TestLiveSpeakerLoopbackCycleLeavesTheSpeakersUsable},
      {"service transport follows the configured preference",
       &TestServiceTransportFollowsTheConfiguredPreference},
      {"service transport defaults to pulse",
       &TestServiceTransportDefaultsToPulse},
      {"daemon config round trips the audio backend key",
       &TestDaemonConfigRoundTripsTheAudioBackendKey},
      {"saving audio settings leaves the backend key alone",
       &TestSavingAudioSettingsLeavesTheBackendKeyAlone},
      {"stale pulse module detection skips the monitor and other apps",
       &TestStalePulseModuleDetectionSkipsTheMonitorAndOtherApps},
      {"stale pulse module detection matches whole argument values",
       &TestStalePulseModuleDetectionMatchesWholeArgumentValues},
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
      {"frame buffer hands the newest frame over",
       &TestFrameBufferHandsTheNewestFrameOver},
      {"frame buffer repeats the last frame",
       &TestFrameBufferRepeatsTheLastFrame},
      {"frame buffer reports a frame that was never taken",
       &TestFrameBufferReportsAFrameThatWasNeverTaken},
      {"frame buffer survives a producer and a consumer",
       &TestFrameBufferSurvivesAProducerAndAConsumer},
      {"frame buffer drops the row padding",
       &TestFrameBufferDropsTheRowPadding},
      {"camera negotiated format mismatch names the difference",
       &TestCameraNegotiatedFormatMismatchNamesTheDifference},
      {"camera frame byte arithmetic", &TestCameraFrameByteArithmetic},
      {"camera node rejects a short frame", &TestCameraNodeRejectsAShortFrame},
      {"live virtual camera node reaches the graph",
       &TestLiveVirtualCameraNodeReachesTheGraph},
      {"live virtual camera feeds a GStreamer consumer",
       &TestLiveVirtualCameraFeedsAGstreamerConsumer},
      {"live virtual camera keeps the row layout",
       &TestLiveVirtualCameraKeepsTheRowLayout},
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
