#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

// Set by CMake from the STUDIOCAST_ENABLE_PIPEWIRE option. Guard every
// PipeWire header and every PipeWire call with it.
#ifndef STUDIOCAST_HAVE_PIPEWIRE
#define STUDIOCAST_HAVE_PIPEWIRE 0
#endif

namespace studiocast::pw {

// True when this build has the native PipeWire code.
constexpr bool PipeWireCompiledIn() { return STUDIOCAST_HAVE_PIPEWIRE != 0; }

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

// Canonical device identities. They are the same on every backend, so a user
// sees one name whatever StudioCast runs on.
inline constexpr std::string_view kVirtualMicNodeName = "studiocast_mic";
inline constexpr std::string_view kVirtualMicDescription =
    "StudioCast Microphone";
inline constexpr std::string_view kVirtualSpeakerNodeName =
    "studiocast_speakers";
inline constexpr std::string_view kVirtualSpeakerDescription =
    "StudioCast Speakers";
inline constexpr std::string_view kVirtualCameraNodeName = "studiocast_camera";
inline constexpr std::string_view kVirtualCameraDescription =
    "StudioCast Camera";

// Value for the `node.latency` property. It asks the server for a quantum of
// `frame_samples` at `sample_rate`. The server may still give a smaller
// quantum, which the ring buffer absorbs.
std::string NodeLatencyProperty(std::uint32_t frame_samples, int sample_rate);

// Value for the `node.rate` property.
std::string NodeRateProperty(int sample_rate);

// Size in bytes of one pipeline frame of interleaved float32 samples.
std::size_t AudioFrameBytes(std::uint32_t frame_samples,
                            std::uint32_t channels);

// Size in bytes of the ring that joins the real-time callback to the pipeline
// thread. It always holds at least two frames, so a write and a read never
// meet on the same frame.
std::size_t AudioRingCapacityBytes(std::uint32_t frame_samples,
                                   std::uint32_t channels, int frames);

// Injection points for the socket probe, so the rules are testable without a
// server. `get_env` returns an empty string for a variable that is not set.
struct PipeWireProbeEnv {
  std::function<std::string(const char *)> get_env;
  std::function<bool(const std::string &)> path_exists;
};

// Where the PipeWire socket should be, and whether it is there.
struct PipeWireSocketProbe {
  bool found = false;
  std::string path;
  std::string reason;
};

// Looks for the server socket. It reads PIPEWIRE_RUNTIME_DIR, then
// XDG_RUNTIME_DIR, then USERPROFILE, and takes the socket name from
// PIPEWIRE_REMOTE, or "pipewire-0" when that variable is not set.
PipeWireSocketProbe ProbePipeWireSocket(const PipeWireProbeEnv &env);

// The same probe against the real process environment and file system.
PipeWireSocketProbe ProbePipeWireSocket();

// Combines the build option with the socket probe.
PipeWireAvailability ProbePipeWire();

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
