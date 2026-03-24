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

VirtualSpeakerState DetectLoaded() {
  std::string err;
  const auto mods = pulse::ListModules(&err);

  VirtualSpeakerState s;
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

std::vector<int> DetectAllLoopbacksFromStudioCastSpeakersMonitor() {
  std::string err;
  const auto mods = pulse::ListModules(&err);

  std::vector<int> ids;
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

bool CreateVirtualSpeaker(std::string *error) {
  std::string details;
  if (!pulse::PactlAvailable(&details)) {
    if (error)
      *error = "pactl not available: " + details;
    return false;
  }

  auto loaded = DetectLoaded();
  auto state = LoadVirtualSpeakerState();

  if (!loaded.null_sink_module_id) {
    // NOTE: Keep quotes *inside* the value so Pulse/PipeWire module parsing can
    // handle spaces. The outer single quotes are for the shell; the inner
    // double quotes survive into pactl.
    std::string err;
    const std::string argsWithDesc =
        std::string("sink_name=") + kSpeakersSinkName +
        " sink_properties='device.description=\"StudioCast Speakers\"'";

    auto id = pulse::LoadModule("module-null-sink", argsWithDesc, &err);
    if (!id) {
      std::string err2;
      const std::string argsMinimal =
          std::string("sink_name=") + kSpeakersSinkName;
      id = pulse::LoadModule("module-null-sink", argsMinimal, &err2);
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

  const auto ids = DetectAllLoopbacksFromStudioCastSpeakersMonitor();
  for (int id : ids) {
    std::string err;
    (void)pulse::UnloadModule(id, &err);
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

  {
    std::string err;
    if (!CreateVirtualSpeaker(&err)) {
      if (error)
        *error = err;
      return false;
    }
  }

  // Avoid duplicates.
  {
    std::string err;
    (void)StopSpeakerLoopback(&err);
  }

  std::string chosen = util::TrimCopy(target_sink_name);
  if (chosen.empty()) {
    std::string err;
    auto def = pulse::GetDefaultSinkName(&err);
    if (def && *def != kSpeakersSinkName && *def != kVirtualMicSinkName) {
      chosen = *def;
    } else {
      // If the user's default sink is our virtual device (common when testing),
      // pick the first non-virtual sink as a best-effort physical target.
      const auto sinks = pulse::ListSinks(&err);
      for (const auto &s : sinks) {
        if (s.name != kSpeakersSinkName && s.name != kVirtualMicSinkName) {
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

  if (chosen == kSpeakersSinkName || chosen == kVirtualMicSinkName) {
    if (error)
      *error = "Refusing to loop back to '" + chosen + "' (feedback loop).";
    return false;
  }

  std::ostringstream args;
  args << "source=" << MonitorSourceName() << " "
       << "sink=" << chosen << " "
       << "latency_msec=" << latency_ms;

  std::string err;
  auto id = pulse::LoadModule("module-loopback", args.str(), &err);
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
    (void)StopSpeakerLoopback(&err);
  }

  const auto loaded = DetectLoaded();
  if (loaded.null_sink_module_id) {
    std::string err;
    (void)pulse::UnloadModule(*loaded.null_sink_module_id, &err);
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
