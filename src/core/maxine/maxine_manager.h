#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace studiocast::maxine {

struct FeatureInstallStatus {
  std::string id;      // e.g. "greenscreen"
  bool installed = false;
  std::string details; // human-friendly hint
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
  std::filesystem::path features_dir;

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
  bool ok = false;
  std::string summary;

  std::string nvidia_driver; // empty if unknown
  GpuDiagnostics gpu;
  ComponentDiagnostics vfx;
  ComponentDiagnostics ar;

  // Stable effect IDs (see `core/video/effects/effect_descriptors.*`).
  std::vector<std::string> available_effects;

  std::vector<std::string> problems;
  std::vector<std::string> hints;

  std::string ToJson() const;
};

class MaxineManager {
public:
  MaxineDiagnostics Diagnose(bool verbose_probe = false) const;
};

}  // namespace studiocast::maxine
