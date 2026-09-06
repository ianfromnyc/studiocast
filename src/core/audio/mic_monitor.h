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

// First sentence of the message `ChooseSafeMicMonitorSinkName` writes when it
// finds no safe output at all. The GUI matches on it to show what to do about
// it instead of asking the user to open Support, so both sides use this one
// text and cannot drift apart.
inline constexpr char kNoSafeMicMonitorSinkMessage[] =
    "No safe output sink was found for the microphone monitor.";

// Opening words of the messages the daemon writes when the sound server gave
// no answer to a request to start the monitor. That state clears itself and
// asks nothing of the user, so the GUI matches on this text to say the monitor
// waits instead of asking the user to open Support. Both sides use this one
// text and cannot drift apart.
inline constexpr char kSoundServerNoAnswerMessage[] =
    "The sound server did not answer";

// The whole message the daemon writes when the sound server gave no answer to
// a request to stop the monitor. It needs a text of its own because the two
// states are opposites: a start that got no answer plays nothing, while a stop
// that got no answer can leave the loopback playing the microphone into the
// speakers. It opens with `kSoundServerNoAnswerMessage`, so the GUI must match
// on this text first, or a failed stop reads as a monitor that starts again on
// its own.
inline constexpr char kSoundServerNoAnswerOnStopMessage[] =
    "The sound server did not answer in time, so the microphone monitor "
    "loopback was not removed.";

// Resolves the configured sink ("auto" = Pulse default) to a safe output sink.
// Returns no value when no safe sink is available; `error` says why.
std::optional<std::string>
ChooseSafeMicMonitorSinkName(const std::string &configured_sink,
                             const std::string &mic_source_name,
                             std::string *error);

// Says whether `sink_name` is still one of the output sinks the sound server
// offers. No value means the answer is not known — there is no sound server to
// ask, the sink list could not be read, or the name is "auto", which names no
// sink. A caller must not read "not known" as "gone".
std::optional<bool> MicMonitorSinkPresent(const std::string &sink_name,
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
