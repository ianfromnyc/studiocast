#include "core/audio/mic_monitor.h"

#include <algorithm>
#include <cstdlib>
#include <vector>

#include "core/audio/audio_device_safety.h"
#include "core/audio/pulse/pactl.h"
#include "core/util/strings.h"

namespace studiocast::audio {
namespace {

constexpr const char *kVirtualMicSourceName = "studiocast_mic";

// Pulse keeps only the text before the first space of a module property value,
// so the monitor stream name must not contain spaces.
constexpr const char *kMonitorStreamName = "StudioCast_Microphone_Monitor";

bool Contains(const std::string &hay, const std::string &needle) {
  return hay.find(needle) != std::string::npos;
}

// A loopback belongs to the monitor when it reads the StudioCast microphone
// and carries the monitor stream name.
bool IsMonitorLoopbackModule(const pulse::PactlModule &m) {
  if (m.name != "module-loopback")
    return false;
  if (!Contains(m.args, std::string("source=") + kVirtualMicSourceName))
    return false;
  return Contains(m.args, kMonitorStreamName);
}

std::string Trimmed(std::string s) { return studiocast::util::TrimCopy(s); }

// The monitor source is the sink monitor of `sink`, so capturing from that
// monitor while playing into the sink would create a feedback loop.
//
// This rule is belt and braces: `IsUnsafeInputSourceName` already refuses
// every source name that carries ".monitor", so a microphone the daemon
// resolved can never be "<sink>.monitor". Only a caller that passes an
// unchecked source name, such as a test, reaches it. Keep it anyway, because
// it is the rule that names the failure.
bool SinkFeedsCaptureSource(const std::string &sink,
                            const std::string &mic_source_name) {
  if (sink.empty() || mic_source_name.empty())
    return false;
  return mic_source_name == sink + ".monitor";
}

} // namespace

MicMonitorConfig NormalizeMicMonitorConfig(MicMonitorConfig cfg) {
  cfg.sink = studiocast::util::TrimCopy(cfg.sink);
  if (cfg.sink.empty())
    cfg.sink = "auto";
  cfg.latency_ms = std::clamp(cfg.latency_ms, kMicMonitorMinLatencyMs,
                              kMicMonitorMaxLatencyMs);
  cfg.volume = std::clamp(cfg.volume, 0, 100);
  return cfg;
}

std::string MicMonitorSourceName() { return kVirtualMicSourceName; }

bool IsUnsafeMicMonitorSinkName(const std::string &sink,
                                const std::string &mic_source_name,
                                std::string *reason) {
  if (reason)
    reason->clear();

  const std::string s = Trimmed(sink);
  if (s.empty() || s == "auto")
    return false;

  // StudioCast's own sinks and any monitor endpoint are already covered by the
  // shared speaker-target rules.
  if (IsUnsafeSpeakerTargetSinkName(s, reason))
    return true;

  if (SinkFeedsCaptureSource(s, Trimmed(mic_source_name))) {
    if (reason) {
      *reason = "Pulse sink '" + s +
                "' is monitored by the selected microphone input '" +
                Trimmed(mic_source_name) +
                "'. Monitoring into it would create a feedback loop.";
    }
    return true;
  }

  return false;
}

std::optional<std::string>
ChooseSafeMicMonitorSinkName(const std::string &configured_sink,
                             const std::string &mic_source_name,
                             std::string *error) {
  if (error)
    error->clear();

  const std::string mic = Trimmed(mic_source_name);
  std::string chosen = Trimmed(configured_sink);
  if (chosen == "auto")
    chosen.clear();

  std::string reason;
  if (!chosen.empty()) {
    if (IsUnsafeMicMonitorSinkName(chosen, mic, &reason)) {
      if (error)
        *error = reason;
      return std::nullopt;
    }
    return chosen;
  }

  std::string defaultErr;
  auto def = pulse::GetDefaultSinkName(&defaultErr);
  std::string rejected;
  if (def) {
    const std::string candidate = Trimmed(*def);
    if (!IsUnsafeMicMonitorSinkName(candidate, mic, &reason)) {
      return candidate;
    }
    rejected = candidate;
  }

  // The Pulse default is unusable. Fall back to the first safe sink.
  std::string listErr;
  const auto sinks = pulse::ListSinks(&listErr);
  for (const auto &sink : sinks) {
    std::string candidateReason;
    if (!sink.name.empty() &&
        !IsUnsafeMicMonitorSinkName(sink.name, mic, &candidateReason)) {
      return sink.name;
    }
  }

  if (error) {
    *error = "No safe output sink was found for the microphone monitor. ";
    if (!rejected.empty()) {
      *error += "The Pulse default sink '" + rejected +
                "' is unsafe: " + reason + " ";
    }
    if (!defaultErr.empty())
      *error += "Default sink note: " + defaultErr + ". ";
    if (!listErr.empty())
      *error += "Sink list note: " + listErr + ". ";
    *error += "Choose a physical output sink.";
  }
  return std::nullopt;
}

std::string MicMonitorStreamName() { return kMonitorStreamName; }

std::vector<std::string> BuildMicMonitorLoadModuleArgs(const std::string &sink,
                                                       int latency_ms) {
  const std::string tag = std::string("media.name=") + kMonitorStreamName;
  return {
      std::string("source=") + kVirtualMicSourceName,
      "sink=" + sink,
      "latency_msec=" +
          std::to_string(std::clamp(latency_ms, kMicMonitorMinLatencyMs,
                                    kMicMonitorMaxLatencyMs)),
      "source_output_properties=" + tag,
      "sink_input_properties=" + tag,
  };
}

std::vector<int> DetectMicMonitorModuleIds(std::string *error) {
  if (error)
    error->clear();

  std::string err;
  const auto modules = pulse::ListModules(&err);
  std::vector<int> ids;
  if (!err.empty()) {
    if (error)
      *error = err;
    return ids;
  }

  for (const auto &m : modules) {
    if (IsMonitorLoopbackModule(m))
      ids.push_back(m.id);
  }
  return ids;
}

MicMonitorState DetectMicMonitor(std::string *error) {
  MicMonitorState out;

  std::string listErr;
  const auto modules = pulse::ListModules(&listErr);
  if (!listErr.empty()) {
    if (error)
      *error = listErr;
    return out;
  }
  if (error)
    error->clear();

  for (const auto &m : modules) {
    if (!IsMonitorLoopbackModule(m))
      continue;
    out.active = true;
    out.module_id = m.id;
    for (const auto &arg : studiocast::util::Split(m.args, ' ')) {
      if (arg.rfind("sink=", 0) == 0)
        out.sink = arg.substr(std::string("sink=").size());
      else if (arg.rfind("latency_msec=", 0) == 0)
        out.latency_ms =
            std::atoi(arg.c_str() + std::string("latency_msec=").size());
    }
    break;
  }
  return out;
}

bool StopMicMonitor(std::string *error) {
  std::string details;
  bool pactlTimedOut = false;
  if (!pulse::PactlAvailable(&details, &pactlTimedOut)) {
    if (pactlTimedOut) {
      // A sound server that did not answer is not a sound server that is
      // absent. The loopback can still be loaded and still play the
      // microphone into the speakers, so this is a failed stop: the caller
      // must keep the route in mind and try again.
      if (error) {
        *error = "The sound server did not answer in time, so the microphone "
                 "monitor loopback was not removed.";
        if (!details.empty())
          *error += " Details: " + details;
      }
      return false;
    }
    // No pactl means no Pulse, and no Pulse means there is no loopback to
    // remove. That is nothing to clean, not a failure: a failure would tell a
    // user who never turned the monitor on that it needs attention, and would
    // keep the stop retry running for ever.
    if (error)
      error->clear();
    return true;
  }

  std::string listErr;
  const auto ids = DetectMicMonitorModuleIds(&listErr);
  if (!listErr.empty()) {
    if (error)
      *error = "Failed to list microphone monitor modules: " + listErr;
    return false;
  }

  std::vector<std::string> failures;
  for (const int id : ids) {
    std::string err;
    if (!pulse::UnloadModule(id, &err)) {
      failures.push_back("module " + std::to_string(id) +
                         (err.empty() ? std::string() : ": " + err));
    }
  }

  if (!failures.empty()) {
    if (error) {
      *error = "Failed to unload the microphone monitor loopback: ";
      for (std::size_t i = 0; i < failures.size(); ++i) {
        if (i)
          *error += "; ";
        *error += failures[i];
      }
    }
    return false;
  }

  if (error)
    error->clear();
  return true;
}

// The loopback sink input is found by the owner module id that
// `pactl load-module` reported.
bool SetMicMonitorVolume(int module_id, int volume, std::string *error) {
  if (error)
    error->clear();

  std::string listErr;
  const auto inputs = pulse::ListSinkInputsDetailed(&listErr);
  if (!listErr.empty()) {
    if (error)
      *error = "Monitor volume was not applied: " + listErr;
    return false;
  }

  // The owner module is the one `pactl load-module` reported, so it names the
  // stream exactly. The stream name is only a fallback, because a foreign
  // stream can carry it: pipewire-pulse keeps it in the stream-restore
  // database under `sink-input-by-media-name`.
  const pulse::PactlSinkInputInfo *chosen = nullptr;
  for (const auto &input : inputs) {
    if (input.owner_module == module_id) {
      chosen = &input;
      break;
    }
  }
  if (!chosen) {
    for (const auto &input : inputs) {
      if (input.media_name == kMonitorStreamName) {
        chosen = &input;
        break;
      }
    }
  }

  if (!chosen) {
    if (error) {
      *error = "Monitor volume was not applied: the monitor stream was not "
               "found.";
    }
    return false;
  }

  std::string err;
  if (!pulse::SetSinkInputVolumePercent(chosen->id, volume, &err)) {
    if (error)
      *error = "Monitor volume was not applied: " + err;
    return false;
  }
  return true;
}

bool StartMicMonitor(const MicMonitorConfig &raw_cfg,
                     const std::string &mic_source_name, MicMonitorState *out,
                     std::string *error) {
  if (out)
    *out = MicMonitorState{};
  if (error)
    error->clear();

  std::string details;
  if (!pulse::PactlAvailable(&details)) {
    if (error)
      *error = "pactl not available: " + details;
    return false;
  }

  const auto cfg = NormalizeMicMonitorConfig(raw_cfg);

  std::string sinkErr;
  const auto chosen =
      ChooseSafeMicMonitorSinkName(cfg.sink, mic_source_name, &sinkErr);
  if (!chosen) {
    if (error) {
      *error = sinkErr.empty()
                   ? "Failed to choose an output sink for the microphone "
                     "monitor."
                   : sinkErr;
    }
    return false;
  }

  // Never leave a second monitor loopback behind.
  {
    std::string stopErr;
    if (!StopMicMonitor(&stopErr)) {
      if (error)
        *error = "Failed to stop the existing microphone monitor: " + stopErr;
      return false;
    }
  }

  std::string loadErr;
  auto id = pulse::LoadModule(
      "module-loopback", BuildMicMonitorLoadModuleArgs(*chosen, cfg.latency_ms),
      &loadErr);
  if (!id) {
    if (error)
      *error = "Failed to load the microphone monitor loopback: " + loadErr;
    return false;
  }

  // A volume that does not apply leaves the monitor playing, so it is a
  // warning rather than a failed start.
  std::string volumeWarning;
  SetMicMonitorVolume(*id, cfg.volume, &volumeWarning);

  if (out) {
    out->active = true;
    out->module_id = *id;
    out->sink = *chosen;
    out->latency_ms = cfg.latency_ms;
    out->volume = cfg.volume;
    out->warning = volumeWarning;
  }
  return true;
}

} // namespace studiocast::audio
