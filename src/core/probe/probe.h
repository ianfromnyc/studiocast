#pragma once

#include <optional>
#include <string>
#include <vector>

namespace studiocast::probe {

struct Version {
  int major = 0;
  int minor = 0;
  int patch = 0;
  bool has_patch = false;
  std::string original;
};

struct GpuInfo {
  int index = -1;   // nvidia-smi index
  std::string uuid; // stable identifier
  std::string name;
  std::optional<std::string> compute_cap;    // e.g. "7.5"
  bool likely_supported = false;             // heuristic (>= 7.5)
  std::optional<std::string> maxine_gpu_arg; // e.g. "t4", "a10"
};

struct CheckResult {
  std::string name;
  bool ok = false;
  bool skipped = false;
  std::string details;
};

struct Report {
  std::string app_version;
  std::string app_git_sha;

  std::string os_pretty_name;
  std::string kernel;

  std::optional<Version> nvidia_driver;
  std::vector<GpuInfo> gpus;

  // What StudioCast would target (based on settings + detected GPUs)
  std::string gpu_selection_mode;        // "auto" | "index" | "uuid"
  std::optional<int> selected_gpu_index; // nvidia-smi index
  std::string selected_gpu_uuid;         // if known

  std::vector<CheckResult> checks;
  std::vector<std::string> notes;

  bool AllChecksPassed() const;

  std::string ToText() const;
  std::string ToJson() const;
};

Report Run(bool verbose);

} // namespace studiocast::probe
