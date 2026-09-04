#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace studiocast::pw {

// Which audio transport carries capture and playback for the real-time
// pipeline.
enum class AudioTransport {
  kPulse,
  kPipeWire,
};

// What the user asked for. `kAuto` lets StudioCast decide.
enum class AudioTransportPreference {
  kAuto,
  kPulse,
  kPipeWire,
};

// Whether the native PipeWire code is in this build, and whether a server
// answers. `reason` explains a negative result.
struct PipeWireAvailability {
  bool compiled_in = false;
  bool server_reachable = false;
  std::string reason;

  bool Usable() const { return compiled_in && server_reachable; }
};

struct AudioTransportDecision {
  AudioTransport transport = AudioTransport::kPulse;

  // True when the user asked for a transport that StudioCast could not give.
  bool used_fallback = false;

  // Human-friendly note for the daemon status and the GUI banners.
  std::string note;
};

std::string_view ToString(AudioTransport t);

// Parses a config value or a command-line value. Letter case and surrounding
// spaces do not matter. Returns nothing for an unknown name.
std::optional<AudioTransportPreference>
ParseAudioTransportPreference(std::string_view s);

std::string_view ToString(AudioTransportPreference p);

// Chooses the transport. `kAuto` prefers native PipeWire when it is usable,
// and uses PulseAudio in every other case.
AudioTransportDecision ResolveAudioTransport(AudioTransportPreference pref,
                                             const PipeWireAvailability &avail);

} // namespace studiocast::pw
