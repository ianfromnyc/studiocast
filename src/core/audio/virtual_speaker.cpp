#include "virtual_speaker.h"

#include <sstream>
#include <string>
#include <vector>

#include "core/audio/pulse/pactl.h"
#include "core/audio/virtual_speaker_state.h"
#include "core/util/strings.h"

namespace studiocast::audio {
namespace {

constexpr const char *kSpeakersSinkName = "studiocast_speakers";
constexpr const char *kVirtualMicSinkName = "studiocast_sink";

bool Contains(const std::string &hay, const std::string &needle) {
  return hay.find(needle) != std::string::npos;
}

std::string MonitorSourceName() {
  return std::string(kSpeakersSinkName) + ".monitor";
}

void BestEffortSetFriendlyNames() {
  // Best-effort; ignore failures (older servers may reject unknown props).
  std::string err;
  (void)pulse::UpdateSinkProplist(kSpeakersSinkName,
                                  {
                                      "device.description=StudioCast Speakers",
                                      "node.description=StudioCast Speakers",
                                  },
                                  &err);
}

VirtualSpeakerState DetectLoaded(std::string *error = nullptr) {
  if (error)
    error->clear();

  std::string err;
  const auto mods = pulse::ListModules(&err);

  VirtualSpeakerState s;
  if (!err.empty()) {
    if (error)
      *error = err;
    return s;
  }

  for (const auto &m : mods) {
    if (m.name == "module-null-sink" &&
        Contains(m.args, std::string("sink_name=") + kSpeakersSinkName)) {
      s.null_sink_module_id = m.id;
    }
    if (m.name == "module-loopback" &&
        Contains(m.args, std::string("source=") + MonitorSourceName())) {
      if (!s.loopback_module_id)
        s.loopback_module_id = m.id;
    }
  }
  return s;
}

std::vector<int>
DetectAllLoopbacksFromStudioCastSpeakersMonitor(std::string *error = nullptr) {
  if (error)
    error->clear();

  std::string err;
  const auto mods = pulse::ListModules(&err);

  std::vector<int> ids;
  if (!err.empty()) {
    if (error)
      *error = err;
    return ids;
  }

  for (const auto &m : mods) {
    if (m.name == "module-loopback" &&
        Contains(m.args, std::string("source=") + MonitorSourceName())) {
      ids.push_back(m.id);
    }
  }
  return ids;
}

} // namespace

std::string VirtualSpeakerMonitorSourceName() { return MonitorSourceName(); }

VirtualSpeakerState DetectVirtualSpeakerLoaded() { return DetectLoaded(); }
VirtualSpeakerState DetectVirtualSpeakerLoaded(std::string *error) {
  return DetectLoaded(error);
}

bool CreateVirtualSpeaker(std::string *error) {
  std::string details;
  if (!pulse::PactlAvailable(&details)) {
    if (error)
      *error = "pactl not available: " + details;
    return false;
  }

  std::string detectErr;
  auto loaded = DetectLoaded(&detectErr);
  if (!detectErr.empty()) {
    if (error)
      *error = "Failed to list virtual speakers before create: " + detectErr;
    return false;
  }
  auto state = LoadVirtualSpeakerState();

  if (!loaded.null_sink_module_id) {
    std::string err;
    auto id = pulse::LoadModule(
        "module-null-sink",
        {
            std::string("sink_name=") + kSpeakersSinkName,
            "sink_properties=device.description=\"StudioCast Speakers\"",
        },
        &err);
    if (!id) {
      std::string err2;
      id = pulse::LoadModule("module-null-sink",
                             {std::string("sink_name=") + kSpeakersSinkName},
                             &err2);
      if (!id) {
        if (error) {
          *error = "Failed to load module-null-sink for virtual speakers.\n"
                   "Attempt 1 (with description): " +
                   err +
                   "\n"
                   "Attempt 2 (minimal): " +
                   err2;
        }
        return false;
      }
    }
    loaded.null_sink_module_id = *id;
  }

  BestEffortSetFriendlyNames();

  state.null_sink_module_id = loaded.null_sink_module_id;
  // Loopback is an operational mode; don't auto-start it on create.
  std::string err;
  if (!SaveVirtualSpeakerState(state, &err)) {
    if (error)
      *error = err;
    return false;
  }

  return true;
}

bool StopSpeakerLoopback(std::string *error) {
  std::string details;
  if (!pulse::PactlAvailable(&details)) {
    if (error)
      *error = "pactl not available: " + details;
    return false;
  }

  std::string listErr;
  const auto ids = DetectAllLoopbacksFromStudioCastSpeakersMonitor(&listErr);
  if (!listErr.empty()) {
    if (error)
      *error = "Failed to list StudioCast speaker loopbacks: " + listErr;
    return false;
  }

  std::vector<std::string> unload_errors;
  for (int id : ids) {
    std::string err;
    if (!pulse::UnloadModule(id, &err)) {
      std::ostringstream oss;
      oss << "module " << id;
      if (!err.empty())
        oss << ": " << err;
      unload_errors.push_back(oss.str());
    }
  }

  if (!unload_errors.empty()) {
    std::ostringstream oss;
    oss << "Failed to unload StudioCast speaker loopback";
    if (unload_errors.size() > 1)
      oss << "s";
    oss << ": ";
    for (std::size_t i = 0; i < unload_errors.size(); ++i) {
      if (i)
        oss << "; ";
      oss << unload_errors[i];
    }
    if (error)
      *error = oss.str();
    return false;
  }

  auto state = LoadVirtualSpeakerState();
  state.loopback_module_id.reset();
  state.loopback_target_sink_name.reset();

  std::string err;
  if (!SaveVirtualSpeakerState(state, &err)) {
    if (error)
      *error = err;
    return false;
  }

  return true;
}

bool StartSpeakerLoopback(const std::string &target_sink_name, int latency_ms,
                          std::string *error) {
  std::string details;
  if (!pulse::PactlAvailable(&details)) {
    if (error)
      *error = "pactl not available: " + details;
    return false;
  }

  auto isVirtualTarget = [](const std::string &name) {
    return name == kSpeakersSinkName || name == kVirtualMicSinkName;
  };

  std::string chosen = util::TrimCopy(target_sink_name);
  if (isVirtualTarget(chosen)) {
    if (error)
      *error = "Refusing to loop back to '" + chosen + "' (feedback loop).";
    return false;
  }

  if (chosen.empty()) {
    std::string err;
    auto def = pulse::GetDefaultSinkName(&err);
    if (def && !isVirtualTarget(*def)) {
      chosen = *def;
    } else {
      // If the user's default sink is our virtual device (common when testing),
      // pick the first non-virtual sink as a best-effort physical target.
      const auto sinks = pulse::ListSinks(&err);
      for (const auto &s : sinks) {
        if (!isVirtualTarget(s.name)) {
          chosen = s.name;
          break;
        }
      }
      if (chosen.empty()) {
        if (error) {
          *error = "Failed to choose a target sink. Default sink is virtual or "
                   "missing.";
          if (!err.empty())
            *error += " (note) " + err;
        }
        return false;
      }
    }
  }

  {
    std::string err;
    if (!CreateVirtualSpeaker(&err)) {
      if (error)
        *error = err;
      return false;
    }
  }

  // Avoid duplicates only after the new target has been validated. This
  // preserves any active route when the requested target would create feedback.
  {
    std::string err;
    if (!StopSpeakerLoopback(&err)) {
      if (error)
        *error = "Failed to stop existing speaker loopback: " + err;
      return false;
    }
  }

  std::string err;
  auto id = pulse::LoadModule("module-loopback",
                              {
                                  "source=" + MonitorSourceName(),
                                  "sink=" + chosen,
                                  "latency_msec=" + std::to_string(latency_ms),
                              },
                              &err);
  if (!id) {
    if (error)
      *error = "Failed to load module-loopback: " + err;
    return false;
  }

  auto state = LoadVirtualSpeakerState();
  state.loopback_module_id = *id;
  state.loopback_target_sink_name = chosen;

  if (!SaveVirtualSpeakerState(state, &err)) {
    if (error)
      *error = err;
    return false;
  }

  return true;
}

bool DestroyVirtualSpeaker(std::string *error) {
  std::string details;
  if (!pulse::PactlAvailable(&details)) {
    if (error)
      *error = "pactl not available: " + details;
    return false;
  }

  {
    std::string err;
    if (!StopSpeakerLoopback(&err)) {
      if (error)
        *error = "Failed to stop speaker loopback before destroy: " + err;
      return false;
    }
  }

  std::string listErr;
  const auto loaded = DetectLoaded(&listErr);
  if (!listErr.empty()) {
    if (error)
      *error = "Failed to list virtual speakers before destroy: " + listErr;
    return false;
  }

  if (loaded.null_sink_module_id) {
    std::string err;
    if (!pulse::UnloadModule(*loaded.null_sink_module_id, &err)) {
      if (error) {
        *error = "Failed to unload virtual speakers null sink module " +
                 std::to_string(*loaded.null_sink_module_id);
        if (!err.empty())
          *error += ": " + err;
      }
      return false;
    }
  }

  std::string err;
  if (!ClearVirtualSpeakerState(&err)) {
    if (error)
      *error = err;
    return false;
  }

  return true;
}

} // namespace studiocast::audio
