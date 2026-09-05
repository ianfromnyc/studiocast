#pragma once

#include <optional>
#include <string>
#include <vector>

namespace studiocast::audio {

// Latency range accepted for the microphone monitor loopback.
inline constexpr int kMicMonitorMinLatencyMs = 1;
inline constexpr int kMicMonitorMaxLatencyMs = 500;

// User intent for the microphone monitor: play the processed StudioCast
// microphone feed on a selected output sink so the user can hear the effects.
struct MicMonitorConfig {
  bool enabled = false;

  // "auto" (or empty) selects the Pulse default sink.
  std::string sink = "auto";

  // Loopback latency target in milliseconds.
  int latency_ms = 20;

  // Playback volume of the monitor stream, 0..100 percent.
  int volume = 100;
};

// Runtime state of the monitor loopback.
struct MicMonitorState {
  bool active = false;
  int module_id = -1;
  std::string sink; // resolved sink name
  int latency_ms = 0;
  int volume = 100;

  // Set when the monitor runs but something optional did not apply, such as
  // the requested volume.
  std::string warning;
};

// Trims the sink name and clamps latency and volume into the supported range.
MicMonitorConfig NormalizeMicMonitorConfig(MicMonitorConfig cfg);

// Pulse source that carries the processed StudioCast microphone feed.
std::string MicMonitorSourceName();

// True when playing the monitor into `sink` would feed StudioCast audio back
// into itself. `mic_source_name` is the resolved capture source; pass an empty
// string when it is not known yet.
bool IsUnsafeMicMonitorSinkName(const std::string &sink,
                                const std::string &mic_source_name,
                                std::string *reason);

// Resolves the configured sink ("auto" = Pulse default) to a safe output sink.
// Returns no value when no safe sink is available; `error` says why.
std::optional<std::string>
ChooseSafeMicMonitorSinkName(const std::string &configured_sink,
                             const std::string &mic_source_name,
                             std::string *error);

// Property value that tags the monitor loopback streams. It carries no space
// because the Pulse module argument parser keeps only the text before the
// first space.
std::string MicMonitorStreamName();

// module-loopback arguments for a monitor into `sink`.
std::vector<std::string> BuildMicMonitorLoadModuleArgs(const std::string &sink,
                                                       int latency_ms);

// Module ids of the loopbacks that StudioCast loaded for the monitor. They are
// found by their module arguments, so they survive a daemon restart.
std::vector<int> DetectMicMonitorModuleIds(std::string *error);

// Reports the monitor loopback that is loaded now, if any.
MicMonitorState DetectMicMonitor(std::string *error);

// Unloads every monitor loopback. Succeeds when there is nothing to unload.
bool StopMicMonitor(std::string *error);

// Sets the playback volume of the running monitor stream, 0..100 percent. The
// volume belongs to the loopback sink input, so the module need not reload.
bool SetMicMonitorVolume(int module_id, int volume, std::string *error);

// Starts the monitor: studiocast_mic -> the configured (or default) sink.
// Any monitor loopback that is already loaded is unloaded first.
bool StartMicMonitor(const MicMonitorConfig &cfg,
                     const std::string &mic_source_name, MicMonitorState *out,
                     std::string *error);

} // namespace studiocast::audio
