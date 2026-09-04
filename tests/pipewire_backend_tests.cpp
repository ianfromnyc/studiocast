#include <iostream>
#include <string>

#include "core/pipewire/pipewire_support.h"

namespace {

using studiocast::pw::AudioTransport;
using studiocast::pw::AudioTransportPreference;
using studiocast::pw::ParseAudioTransportPreference;
using studiocast::pw::PipeWireAvailability;
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
