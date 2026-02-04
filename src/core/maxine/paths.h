#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace studiocast::maxine {

    namespace fs = std::filesystem;

    struct ComponentPaths {
        std::string component;          // e.g. "VFX" or "AR"
        std::string root_env_var;       // e.g. "STUDIOCAST_VFX_SDK_ROOT"

        fs::path root;                  // selected root (may not exist)
        std::string root_source;        // "env", "xdg", "system"
        std::vector<fs::path> candidate_roots;   // in priority order

        std::vector<std::string> library_names;  // in priority order
        std::vector<fs::path> searched_lib_dirs; // derived from root
        fs::path library;               // resolved shared library path (empty if missing)

        fs::path models_dir;
        fs::path features_dir;

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
    };

    // Resolves expected Maxine SDK paths for VFX and AR.
    //
    // Priority order for selecting roots:
    //  1) environment override (if set)
    //  2) XDG default roots (see `core/util/xdg.*`)
    //  3) `/usr/local/VideoFX` and `/usr/local/ARSDK`
    //
    // The returned report contains precise missing-piece diagnostics.
    MaxinePathsReport ResolveMaxinePaths();

}  // namespace studiocast::maxine
