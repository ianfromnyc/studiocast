#include <iostream>
#include <map>
#include <string>

#include "core/pipewire/pipewire_support.h"

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
                "the note should carry the availability reason");
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
  return Expect(!p.found, "no runtime directory means no socket") &&
         Expect(p.reason.find("XDG_RUNTIME_DIR") != std::string::npos,
                "the reason should name the missing variable: " + p.reason);
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
      {"node property arithmetic", &TestNodePropertyArithmetic},
      {"canonical node names", &TestCanonicalNodeNames},
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
