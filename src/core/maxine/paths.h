#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace studiocast::maxine {

namespace fs = std::filesystem;

struct ComponentPaths {
  std::string component;    // e.g. "VFX" or "AR"
  std::string root_env_var; // e.g. "STUDIOCAST_VFX_SDK_ROOT"

  fs::path root;                         // selected root (may not exist)
  std::string root_source;               // "env", "xdg", "system"
  std::vector<fs::path> candidate_roots; // in priority order

  std::vector<std::string> library_names;  // in priority order
  std::vector<fs::path> searched_lib_dirs; // derived from root
  fs::path library; // resolved shared library path (empty if missing)

  // Resolved models directory. Legacy SDKs (0.7/0.8) keep it at
  // `<root>/models`; the SDK Core 1.x keeps it at `<root>/lib/models`.
  fs::path models_dir;
  // Relative name of the directory that `models_dir` came from: "models" or
  // "lib/models". Empty when no candidate exists.
  std::string models_dir_source;
  // All models directories we look at, in priority order.
  std::vector<fs::path> candidate_models_dirs;

  fs::path features_dir;

  // Some components (e.g. AFX) do not ship a `models/` directory.
  bool require_models_dir = true;
  bool require_features_dir = true;

  bool root_exists = false;
  bool models_dir_exists = false;
  bool features_dir_exists = false;
  bool library_exists = false;

  bool ok = false;
  std::vector<std::string> problems;
};

struct MaxinePathsReport {
  ComponentPaths vfx;
  ComponentPaths ar;
  ComponentPaths afx;
};

// Resolves expected Maxine SDK paths for VFX, AR, and AFX.
//
// Priority order for selecting roots:
//  1) environment override (if set)
//  2) XDG default roots (see `core/util/xdg.*`)
//  3) `/usr/local/VideoFX`, `/usr/local/ARSDK`, and
//  `/usr/local/Audio_Effects_SDK`
//
// The returned report contains precise missing-piece diagnostics.
MaxinePathsReport ResolveMaxinePaths();

// Derives the models directory from the path of an SDK library. It looks for a
// `models` directory beside the library (SDK Core 1.x: `<root>/lib/models`) and
// then in the directories above it (legacy: `<root>/models`). Returns an empty
// path when there is none. Effects use it because they only know the library
// they loaded.
fs::path ModelsDirForLibrary(const fs::path &library);

} // namespace studiocast::maxine
