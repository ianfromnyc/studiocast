#include "virtual_mic.h"

#include <sstream>
#include <string>
#include <vector>

#include "core/audio/pulse/pactl.h"
#include "core/audio/virtual_mic_state.h"
#include "core/util/strings.h"

namespace studiocast::audio {
namespace {

constexpr const char* kSinkName = "studiocast_sink";
constexpr const char* kSourceName = "studiocast_mic";

bool Contains(const std::string& hay, const std::string& needle) {
  return hay.find(needle) != std::string::npos;
}

void BestEffortSetFriendlyNames() {
  // Best-effort; ignore failures (older servers may reject unknown props).
  std::string err;
  (void)pulse::UpdateSinkProplist(
      kSinkName,
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

VirtualMicState DetectLoaded() {
  std::string err;
  const auto mods = pulse::ListModules(&err);

  VirtualMicState s;
  for (const auto& m : mods) {
    if (m.name == "module-null-sink" && Contains(m.args, std::string("sink_name=") + kSinkName)) {
      s.null_sink_module_id = m.id;
    }
    if (m.name == "module-remap-source" && Contains(m.args, std::string("source_name=") + kSourceName)) {
      s.remap_source_module_id = m.id;
    }
    // loopback can be multiple; we track one in state, but can also clean by scanning.
    if (m.name == "module-loopback" && Contains(m.args, std::string("sink=") + kSinkName)) {
      if (!s.loopback_module_id) s.loopback_module_id = m.id;
    }
  }
  return s;
}

std::vector<int> DetectAllLoopbacksToStudioCastSink() {
  std::string err;
  const auto mods = pulse::ListModules(&err);

  std::vector<int> ids;
  for (const auto& m : mods) {
    if (m.name == "module-loopback" && Contains(m.args, std::string("sink=") + kSinkName)) {
      ids.push_back(m.id);
    }
  }
  return ids;
}

}  // namespace

bool CreateVirtualMic(std::string* error) {
  std::string details;
  if (!pulse::PactlAvailable(&details)) {
    if (error) *error = "pactl not available: " + details;
    return false;
  }

  // Prefer existing loaded modules (idempotent behavior).
  auto loaded = DetectLoaded();
  auto state = LoadVirtualMicState();

  // Ensure null sink
  if (!loaded.null_sink_module_id) {
    // NOTE: Keep quotes *inside* the value so Pulse/PipeWire module parsing can handle spaces.
    // The outer single quotes are for the shell; the inner double quotes survive into pactl.
    std::string err;
    const std::string argsWithDesc =
        std::string("sink_name=") + kSinkName +
        " sink_properties='device.description=\"StudioCast Sink\"'";

    auto id = pulse::LoadModule("module-null-sink", argsWithDesc, &err);
    if (!id) {
      // Fallback: try without sink_properties (some servers are picky)
      std::string err2;
      const std::string argsMinimal = std::string("sink_name=") + kSinkName;
      id = pulse::LoadModule("module-null-sink", argsMinimal, &err2);

      if (!id) {
        if (error) {
          *error = "Failed to load module-null-sink.\n"
                   "Attempt 1 (with description): " + err + "\n"
                   "Attempt 2 (minimal): " + err2;
        }
        return false;
      }
    }
    loaded.null_sink_module_id = *id;
  }

  // Ensure remap source (virtual mic)
  if (!loaded.remap_source_module_id) {
    std::string err;
    const std::string argsWithDesc =
        std::string("master=") + kSinkName + ".monitor " +
        "source_name=" + kSourceName + " " +
        "source_properties='device.description=\"StudioCast Microphone\"'";

    auto id = pulse::LoadModule("module-remap-source", argsWithDesc, &err);
    if (!id) {
      // Fallback: try without source_properties
      std::string err2;
      const std::string argsMinimal =
          std::string("master=") + kSinkName + ".monitor " +
          "source_name=" + kSourceName;

      id = pulse::LoadModule("module-remap-source", argsMinimal, &err2);

      if (!id) {
        if (error) {
          *error = "Failed to load module-remap-source.\n"
                   "Attempt 1 (with description): " + err + "\n"
                   "Attempt 2 (minimal): " + err2;
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
  // Processed mode does not use module-loopback; keep the field only for migration/debug visibility.
  state.loopback_module_id.reset();

  std::string err;
  if (!SaveVirtualMicState(state, &err)) {
    if (error) *error = err;
    return false;
  }

  return true;
}

bool StopLoopback(std::string* error) {
  std::string details;
  if (!pulse::PactlAvailable(&details)) {
    if (error) *error = "pactl not available: " + details;
    return false;
  }

  // Unload any loopback modules routing into our sink (safe cleanup).
  const auto ids = DetectAllLoopbacksToStudioCastSink();
  for (int id : ids) {
    std::string err;
    (void)pulse::UnloadModule(id, &err);
  }

  // Clear loopback from state file (preserve other ids if present).
  auto state = LoadVirtualMicState();
  state.loopback_module_id.reset();

  std::string err;
  if (!SaveVirtualMicState(state, &err)) {
    if (error) *error = err;
    return false;
  }

  return true;
}

bool StartLoopback(const std::string& source_name, int latency_ms, std::string* error) {
#ifdef NDEBUG
  if (error) {
    *error = "Legacy pass-through loopback is disabled in release builds. "
             "Run the processed pipeline (Maxine AFX -> studiocast_sink) instead.";
  }
  (void)source_name;
  (void)latency_ms;
  return false;
#else
  std::string details;
  if (!pulse::PactlAvailable(&details)) {
    if (error) *error = "pactl not available: " + details;
    return false;
  }

  // Ensure virtual mic exists first.
  {
    std::string err;
    if (!CreateVirtualMic(&err)) {
      if (error) *error = err;
      return false;
    }
  }

  // Stop existing loopbacks into our sink to avoid duplicates.
  {
    std::string err;
    (void)StopLoopback(&err);
  }

  std::string chosen = util::TrimCopy(source_name);
  if (chosen.empty()) {
    std::string err;
    auto def = pulse::GetDefaultSourceName(&err);
    if (!def) {
      if (error) *error = "Failed to find default source: " + err;
      return false;
    }
    chosen = *def;
  }

  std::ostringstream args;
  args << "source=" << chosen << " "
       << "sink=" << kSinkName << " "
       << "latency_msec=" << latency_ms;

  std::string err;
  auto id = pulse::LoadModule("module-loopback", args.str(), &err);
  if (!id) {
    if (error) *error = "Failed to load module-loopback: " + err;
    return false;
  }

  auto state = LoadVirtualMicState();
  state.loopback_module_id = *id;

  if (!SaveVirtualMicState(state, &err)) {
    if (error) *error = err;
    return false;
  }

  return true;
#endif
}

bool DestroyVirtualMic(std::string* error) {
  std::string details;
  if (!pulse::PactlAvailable(&details)) {
    if (error) *error = "pactl not available: " + details;
    return false;
  }

  // Stop loopback first.
  {
    std::string err;
    (void)StopLoopback(&err);
  }

  // Prefer unloading the modules we detect by name/args (safe and works even if state file is stale).
  const auto loaded = DetectLoaded();

  // Unload remap source
  if (loaded.remap_source_module_id) {
    std::string err;
    (void)pulse::UnloadModule(*loaded.remap_source_module_id, &err);
  }

  // Unload null sink
  if (loaded.null_sink_module_id) {
    std::string err;
    (void)pulse::UnloadModule(*loaded.null_sink_module_id, &err);
  }

  // Clear state file.
  std::string err;
  if (!ClearVirtualMicState(&err)) {
    if (error) *error = err;
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
  if (!details.empty()) oss << "  pactl details: " << details << "\n";

  oss << "\nStudioCast Virtual Mic\n";
  oss << "  sink name: " << kSinkName << "\n";
  oss << "  source name: " << kSourceName << "\n";

  const auto state = LoadVirtualMicState();
  const auto loaded = DetectLoaded();

  oss << "  state file: " << VirtualMicStatePath().string() << "\n";
  oss << "  state ids: "
      << "sink=" << (state.null_sink_module_id ? std::to_string(*state.null_sink_module_id) : "none") << ", "
      << "remap=" << (state.remap_source_module_id ? std::to_string(*state.remap_source_module_id) : "none") << ", "
      << "loopback=" << (state.loopback_module_id ? std::to_string(*state.loopback_module_id) : "none") << "\n";

  oss << "  loaded ids: "
      << "sink=" << (loaded.null_sink_module_id ? std::to_string(*loaded.null_sink_module_id) : "none") << ", "
      << "remap=" << (loaded.remap_source_module_id ? std::to_string(*loaded.remap_source_module_id) : "none") << ", "
      << "loopback=" << (loaded.loopback_module_id ? std::to_string(*loaded.loopback_module_id) : "none") << "\n";

  // List sources for convenience
  std::string err;
  const auto sources = pulse::ListSources(&err);
  oss << "\nSources (pactl list short sources)\n";
  if (!err.empty()) oss << "  (note) " << err << "\n";
  for (const auto& s : sources) {
    oss << "  [" << s.id << "] " << s.name;
    if (s.name == kSourceName) oss << "  <== StudioCast virtual mic";
    oss << "\n";
  }

  return oss.str();
}

}  // namespace studiocast::audio
