#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace studiocast::maxine {

struct FeatureInstallStatus {
  std::string id; // e.g. "greenscreen"
  bool installed = false;
  std::string details; // human-friendly hint
};

struct GpuSummary {
  int index = -1; // nvidia-smi index
  std::string uuid;
  std::string name;
  std::string compute_cap; // empty if unknown
  bool likely_supported = false;
  std::string maxine_gpu_arg; // empty if unknown
};

struct DriverDiagnostics {
  std::string version; // empty if unknown
  bool ok = false;     // known + meets minimum requirement
  std::string min_version;
  bool meets_min_version = false;
  std::string details;
};

struct ComponentDiagnostics {
  std::string component;    // "VFX" | "AR"
  std::string root_env_var; // e.g. "STUDIOCAST_VFX_SDK_ROOT"

  std::filesystem::path root;
  std::string root_source; // "env" | "xdg" | "system"
  std::vector<std::filesystem::path> candidate_roots;

  std::vector<std::string> library_names;
  std::vector<std::filesystem::path> searched_lib_dirs;
  std::filesystem::path library;

  std::filesystem::path models_dir;
  // "models" (legacy) or "lib/models" (SDK Core 1.x). Empty when missing.
  std::string models_dir_source;
  std::vector<std::filesystem::path> candidate_models_dirs;
  std::filesystem::path features_dir;

  // Some components (e.g. AFX) do not ship a models directory.
  bool require_models_dir = true;

  bool root_exists = false;
  bool models_dir_exists = false;
  bool features_dir_exists = false;
  bool library_exists = false;

  bool library_loadable = false;
  std::string library_dlopen_error;

  bool ok = false;
  std::vector<std::string> problems;

  std::vector<FeatureInstallStatus> features;
};

struct GpuDiagnostics {
  std::string selection_mode; // "auto" | "index" | "uuid"

  std::optional<int> selected_index;
  std::string selected_uuid;
  std::string selected_name;

  std::optional<std::string> compute_cap;    // e.g. "8.9"
  std::optional<std::string> maxine_gpu_arg; // e.g. "l4"

  bool ok = false;
  std::string error;
};

struct MaxineDiagnostics {
  // True when at least one Maxine-backed effect is runnable.
  bool ok = false;
  bool supported = false;

  std::string blocked_reason;               // stable enum string
  std::vector<std::string> blocked_details; // human actionable strings

  std::string summary;

  DriverDiagnostics driver;
  std::string nvidia_driver; // legacy alias: empty if unknown
  std::vector<GpuSummary> gpus;

  GpuDiagnostics gpu;
  ComponentDiagnostics vfx;
  ComponentDiagnostics ar;
  ComponentDiagnostics afx;

  // Stable effect IDs (see `core/video/effects/effect_descriptors.*`).
  std::vector<std::string> available_effects;

  // Stable audio effect IDs (AFX-backed). Keep stable for GUI/CLI.
  std::vector<std::string> available_audio_effects;

  // effect_id -> reason(s)
  std::map<std::string, std::vector<std::string>> missing_effects;

  std::vector<std::string> problems;
  std::vector<std::string> hints;

  std::string last_error;

  std::string ToJson() const;
};

class MaxineManager {
public:
  MaxineDiagnostics Diagnose(bool verbose_probe = false) const;
};

} // namespace studiocast::maxine
