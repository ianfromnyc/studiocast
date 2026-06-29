#include "virtual_mic.h"

#include <sstream>
#include <string>
#include <vector>

#include "core/audio/pulse/pactl.h"
#include "core/audio/virtual_mic_state.h"
#include "core/audio/virtual_speaker.h"
#include "core/util/strings.h"

namespace studiocast::audio {
namespace {

constexpr const char *kSinkName = "studiocast_sink";
constexpr const char *kSourceName = "studiocast_mic";

bool Contains(const std::string &hay, const std::string &needle) {
  return hay.find(needle) != std::string::npos;
}

void BestEffortSetFriendlyNames() {
  // Best-effort; ignore failures (older servers may reject unknown props).
  std::string err;
  (void)pulse::UpdateSinkProplist(kSinkName,
                                  {
                                      "device.description=StudioCast Sink",
                                      "node.description=StudioCast Sink",
                                  },
                                  &err);

  err.clear();
  (void)pulse::UpdateSourceProplist(
      kSourceName,
      {
          "device.description=StudioCast Microphone",
          "node.description=StudioCast Microphone",
      },
      &err);
}

VirtualMicState DetectLoaded(std::string *error = nullptr) {
  if (error)
    error->clear();

  std::string err;
  const auto mods = pulse::ListModules(&err);

  VirtualMicState s;
  if (!err.empty()) {
    if (error)
      *error = err;
    return s;
  }

  for (const auto &m : mods) {
    if (m.name == "module-null-sink" &&
        Contains(m.args, std::string("sink_name=") + kSinkName)) {
      s.null_sink_module_id = m.id;
    }
    if (m.name == "module-remap-source" &&
        Contains(m.args, std::string("source_name=") + kSourceName)) {
      s.remap_source_module_id = m.id;
    }
    // loopback can be multiple; we track one in state, but can also clean by
    // scanning.
    if (m.name == "module-loopback" &&
        Contains(m.args, std::string("sink=") + kSinkName)) {
      if (!s.loopback_module_id)
        s.loopback_module_id = m.id;
    }
  }
  return s;
}

std::vector<int>
DetectAllLoopbacksToStudioCastSink(std::string *error = nullptr) {
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
        Contains(m.args, std::string("sink=") + kSinkName)) {
      ids.push_back(m.id);
    }
  }
  return ids;
}

} // namespace

bool CreateVirtualMic(std::string *error) {
  std::string details;
  if (!pulse::PactlAvailable(&details)) {
    if (error)
      *error = "pactl not available: " + details;
    return false;
  }

  // Prefer existing loaded modules (idempotent behavior). If Pulse cannot be
  // queried, do not treat that as "not loaded"; loading another copy can create
  // duplicates or report a misleading follow-up failure.
  std::string detectErr;
  auto loaded = DetectLoaded(&detectErr);
  if (!detectErr.empty()) {
    if (error)
      *error = "Failed to list virtual mic modules before create: " + detectErr;
    return false;
  }
  auto state = LoadVirtualMicState();

  // Ensure null sink
  if (!loaded.null_sink_module_id) {
    std::string err;
    auto id = pulse::LoadModule(
        "module-null-sink",
        {
            std::string("sink_name=") + kSinkName,
            "sink_properties=device.description=\"StudioCast Sink\"",
        },
        &err);
    if (!id) {
      // Fallback: try without sink_properties (some servers are picky)
      std::string err2;
      id = pulse::LoadModule("module-null-sink",
                             {std::string("sink_name=") + kSinkName}, &err2);

      if (!id) {
        if (error) {
          *error = "Failed to load module-null-sink.\n"
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

  // Ensure remap source (virtual mic)
  if (!loaded.remap_source_module_id) {
    std::string err;
    auto id = pulse::LoadModule(
        "module-remap-source",
        {
            std::string("master=") + kSinkName + ".monitor",
            std::string("source_name=") + kSourceName,
            "source_properties=device.description=\"StudioCast Microphone\"",
        },
        &err);
    if (!id) {
      // Fallback: try without source_properties
      std::string err2;
      id =
          pulse::LoadModule("module-remap-source",
                            {
                                std::string("master=") + kSinkName + ".monitor",
                                std::string("source_name=") + kSourceName,
                            },
                            &err2);

      if (!id) {
        if (error) {
          *error = "Failed to load module-remap-source.\n"
                   "Attempt 1 (with description): " +
                   err +
                   "\n"
                   "Attempt 2 (minimal): " +
                   err2;
        }
        return false;
      }
    }
    loaded.remap_source_module_id = *id;
  }

  // ✅ Make naming deterministic for apps like OBS
  BestEffortSetFriendlyNames();

  // Persist
  state.null_sink_module_id = loaded.null_sink_module_id;
  state.remap_source_module_id = loaded.remap_source_module_id;
  // Processed mode does not use module-loopback; keep the field only for
  // migration/debug visibility.
  state.loopback_module_id.reset();

  std::string err;
  if (!SaveVirtualMicState(state, &err)) {
    if (error)
      *error = err;
    return false;
  }

  return true;
}

bool StopLoopback(std::string *error) {
  std::string details;
  if (!pulse::PactlAvailable(&details)) {
    if (error)
      *error = "pactl not available: " + details;
    return false;
  }

  // Unload any loopback modules routing into our sink (safe cleanup).
  std::string listErr;
  const auto ids = DetectAllLoopbacksToStudioCastSink(&listErr);
  if (!listErr.empty()) {
    if (error)
      *error = "Failed to list StudioCast mic loopbacks: " + listErr;
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
    oss << "Failed to unload StudioCast mic loopback";
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

  // Clear loopback from state file (preserve other ids if present).
  auto state = LoadVirtualMicState();
  state.loopback_module_id.reset();

  std::string err;
  if (!SaveVirtualMicState(state, &err)) {
    if (error)
      *error = err;
    return false;
  }

  return true;
}

bool StartLoopback(const std::string &source_name, int latency_ms,
                   std::string *error) {
#ifdef NDEBUG
  if (error) {
    *error =
        "Legacy pass-through loopback is disabled in release builds. "
        "Run the processed pipeline (Maxine AFX -> studiocast_sink) instead.";
  }
  (void)source_name;
  (void)latency_ms;
  return false;
#else
  std::string details;
  if (!pulse::PactlAvailable(&details)) {
    if (error)
      *error = "pactl not available: " + details;
    return false;
  }

  // Ensure virtual mic exists first.
  {
    std::string err;
    if (!CreateVirtualMic(&err)) {
      if (error)
        *error = err;
      return false;
    }
  }

  // Stop existing loopbacks into our sink to avoid duplicates.
  {
    std::string err;
    if (!StopLoopback(&err)) {
      if (error)
        *error = "Failed to stop existing mic loopback: " + err;
      return false;
    }
  }

  std::string chosen = util::TrimCopy(source_name);
  if (chosen.empty()) {
    std::string err;
    auto def = pulse::GetDefaultSourceName(&err);
    if (!def) {
      if (error)
        *error = "Failed to find default source: " + err;
      return false;
    }
    chosen = *def;
  }

  std::string err;
  auto id = pulse::LoadModule("module-loopback",
                              {
                                  "source=" + chosen,
                                  std::string("sink=") + kSinkName,
                                  "latency_msec=" + std::to_string(latency_ms),
                              },
                              &err);
  if (!id) {
    if (error)
      *error = "Failed to load module-loopback: " + err;
    return false;
  }

  auto state = LoadVirtualMicState();
  state.loopback_module_id = *id;

  if (!SaveVirtualMicState(state, &err)) {
    if (error)
      *error = err;
    return false;
  }

  return true;
#endif
}

bool DestroyVirtualMic(std::string *error) {
  std::string details;
  if (!pulse::PactlAvailable(&details)) {
    if (error)
      *error = "pactl not available: " + details;
    return false;
  }

  // Stop loopback first.
  {
    std::string err;
    if (!StopLoopback(&err)) {
      if (error)
        *error = "Failed to stop mic loopback before destroy: " + err;
      return false;
    }
  }

  // Prefer unloading the modules we detect by name/args (safe and works even if
  // state file is stale).
  std::string listErr;
  const auto loaded = DetectLoaded(&listErr);
  if (!listErr.empty()) {
    if (error)
      *error = "Failed to list virtual mic modules before destroy: " + listErr;
    return false;
  }

  auto state = LoadVirtualMicState();
  state.null_sink_module_id = loaded.null_sink_module_id;
  state.remap_source_module_id = loaded.remap_source_module_id;

  // Unload remap source
  if (loaded.remap_source_module_id) {
    std::string err;
    if (!pulse::UnloadModule(*loaded.remap_source_module_id, &err)) {
      if (error) {
        *error = "Failed to unload virtual mic remap source module " +
                 std::to_string(*loaded.remap_source_module_id);
        if (!err.empty())
          *error += ": " + err;
      }
      return false;
    }

    state.remap_source_module_id.reset();
    if (!SaveVirtualMicState(state, &err)) {
      if (error)
        *error = err;
      return false;
    }
  }

  // Unload null sink
  if (loaded.null_sink_module_id) {
    std::string err;
    if (!pulse::UnloadModule(*loaded.null_sink_module_id, &err)) {
      if (error) {
        *error = "Failed to unload virtual mic null sink module " +
                 std::to_string(*loaded.null_sink_module_id);
        if (!err.empty())
          *error += ": " + err;
      }
      return false;
    }

    state.null_sink_module_id.reset();
    if (!SaveVirtualMicState(state, &err)) {
      if (error)
        *error = err;
      return false;
    }
  }

  // Clear state file.
  std::string err;
  if (!ClearVirtualMicState(&err)) {
    if (error)
      *error = err;
    return false;
  }

  return true;
}

std::string StatusText() {
  std::ostringstream oss;

  std::string details;
  const bool ok = pulse::PactlAvailable(&details);
  oss << "Audio stack\n";
  oss << "  pactl: " << (ok ? "OK" : "MISSING") << "\n";
  if (!details.empty())
    oss << "  pactl details: " << details << "\n";

  oss << "\nStudioCast Virtual Mic\n";
  oss << "  sink name: " << kSinkName << "\n";
  oss << "  source name: " << kSourceName << "\n";

  const auto state = LoadVirtualMicState();
  const auto loaded = DetectLoaded();

  oss << "  state file: " << VirtualMicStatePath().string() << "\n";
  oss << "  state ids: "
      << "sink="
      << (state.null_sink_module_id ? std::to_string(*state.null_sink_module_id)
                                    : "none")
      << ", "
      << "remap="
      << (state.remap_source_module_id
              ? std::to_string(*state.remap_source_module_id)
              : "none")
      << ", "
      << "loopback="
      << (state.loopback_module_id ? std::to_string(*state.loopback_module_id)
                                   : "none")
      << "\n";

  oss << "  loaded ids: "
      << "sink="
      << (loaded.null_sink_module_id
              ? std::to_string(*loaded.null_sink_module_id)
              : "none")
      << ", "
      << "remap="
      << (loaded.remap_source_module_id
              ? std::to_string(*loaded.remap_source_module_id)
              : "none")
      << ", "
      << "loopback="
      << (loaded.loopback_module_id ? std::to_string(*loaded.loopback_module_id)
                                    : "none")
      << "\n";

  // List sources for convenience
  std::string err;
  const auto sources = pulse::ListSources(&err);
  oss << "\nSources (pactl list short sources)\n";
  if (!err.empty())
    oss << "  (note) " << err << "\n";
  for (const auto &s : sources) {
    oss << "  [" << s.id << "] " << s.name;
    if (s.name == kSourceName)
      oss << "  <== StudioCast virtual mic";
    oss << "\n";
  }

  // Virtual speaker status
  oss << "\nStudioCast Virtual Speakers\n";
  oss << "  sink name: studiocast_speakers\n";
  oss << "  monitor source: " << VirtualSpeakerMonitorSourceName() << "\n";

  const auto spkState = LoadVirtualSpeakerState();
  const auto spkLoaded = DetectVirtualSpeakerLoaded();
  oss << "  state file: " << VirtualSpeakerStatePath().string() << "\n";
  oss << "  state ids: "
      << "sink="
      << (spkState.null_sink_module_id
              ? std::to_string(*spkState.null_sink_module_id)
              : "none")
      << ", "
      << "loopback="
      << (spkState.loopback_module_id
              ? std::to_string(*spkState.loopback_module_id)
              : "none")
      << ", "
      << "target_sink="
      << (spkState.loopback_target_sink_name
              ? *spkState.loopback_target_sink_name
              : "none")
      << "\n";
  oss << "  loaded ids: "
      << "sink="
      << (spkLoaded.null_sink_module_id
              ? std::to_string(*spkLoaded.null_sink_module_id)
              : "none")
      << ", "
      << "loopback="
      << (spkLoaded.loopback_module_id
              ? std::to_string(*spkLoaded.loopback_module_id)
              : "none")
      << "\n";

  {
    std::string err2;
    auto def = pulse::GetDefaultSinkName(&err2);
    oss << "\nDefault sink\n";
    if (def) {
      oss << "  " << *def << "\n";
    } else {
      oss << "  (none)";
      if (!err2.empty())
        oss << "  (note) " << err2;
      oss << "\n";
    }
  }

  // List sinks for convenience
  err.clear();
  const auto sinks = pulse::ListSinks(&err);
  oss << "\nSinks (pactl list short sinks)\n";
  if (!err.empty())
    oss << "  (note) " << err << "\n";
  for (const auto &s : sinks) {
    oss << "  [" << s.id << "] " << s.name;
    if (s.name == "studiocast_speakers")
      oss << "  <== StudioCast Speakers";
    if (s.name == kSinkName)
      oss << "  <== StudioCast Sink";
    oss << "\n";
  }

  return oss.str();
}

} // namespace studiocast::audio
