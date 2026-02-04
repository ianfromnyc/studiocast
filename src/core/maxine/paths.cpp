#include "core/maxine/paths.h"

#include "core/util/strings.h"
#include "core/util/xdg.h"

#include <cstdlib>
#include <optional>
#include <sstream>

namespace studiocast::maxine {

    namespace {
        namespace fs = std::filesystem;

        std::string JoinStrings(const std::vector<std::string>& v, const char* sep) {
            std::ostringstream oss;
            bool first = true;
            for (const auto& s : v) {
                if (!first) oss << sep;
                oss << s;
                first = false;
            }
            return oss.str();
        }

        std::optional<fs::path> GetEnvPath(const char* name) {
            const char* v = std::getenv(name);
            if (!v || *v == '\0') return std::nullopt;
            return fs::path(v);
        }

        bool DirExists(const fs::path& p) {
            std::error_code ec;
            return !p.empty() && fs::exists(p, ec) && fs::is_directory(p, ec);
        }

        std::vector<fs::path> CandidateLibDirs(const fs::path& root) {
            std::vector<fs::path> dirs;
            if (root.empty()) return dirs;
            dirs.push_back(root / "lib");
            dirs.push_back(root / "lib64");
            dirs.push_back(root / "bin");
            dirs.push_back(root / "lib" / "x86_64-linux-gnu");
            dirs.push_back(root / "lib64" / "x86_64-linux-gnu");
            // Some extractions place shared libraries at the root.
            dirs.push_back(root);
            return dirs;
        }

        fs::path FindFirstExistingLib(const std::vector<fs::path>& dirs,
                                     const std::vector<std::string>& names) {
            std::error_code ec;
            for (const auto& d : dirs) {
                if (!DirExists(d)) continue;
                for (const auto& n : names) {
                    const auto p = d / n;
                    if (fs::exists(p, ec) && fs::is_regular_file(p, ec)) {
                        return p;
                    }
                }
            }
            return {};
        }

        fs::path ChooseRoot(const std::optional<fs::path>& env_override,
                            const fs::path& xdg_default,
                            const fs::path& system_default,
                            std::string* source_out) {
            if (env_override) {
                if (source_out) *source_out = "env";
                return *env_override;
            }

            if (DirExists(xdg_default)) {
                if (source_out) *source_out = "xdg";
                return xdg_default;
            }

            if (DirExists(system_default)) {
                if (source_out) *source_out = "system";
                return system_default;
            }

            // Default to the XDG location even if missing, so callers can show a stable hint.
            if (source_out) *source_out = "xdg";
            return xdg_default;
        }

        std::string SearchedDirsString(const std::vector<fs::path>& dirs) {
            std::vector<std::string> s;
            s.reserve(dirs.size());
            for (const auto& d : dirs) s.push_back(d.string());
            return JoinStrings(s, ", ");
        }

        ComponentPaths ResolveComponent(const std::string& component,
                                        const char* env_var,
                                        const fs::path& xdg_default,
                                        const fs::path& system_default,
                                        std::vector<std::string> library_names) {
            ComponentPaths out;
            out.component = component;
            out.root_env_var = env_var;
            out.library_names = std::move(library_names);

            const auto env_override = GetEnvPath(env_var);
            out.candidate_roots = {};
            if (env_override) out.candidate_roots.push_back(*env_override);
            out.candidate_roots.push_back(xdg_default);
            out.candidate_roots.push_back(system_default);

            out.root = ChooseRoot(env_override, xdg_default, system_default, &out.root_source);
            out.root_exists = DirExists(out.root);

            out.models_dir = out.root / "models";
            out.features_dir = out.root / "features";

            out.models_dir_exists = DirExists(out.models_dir);
            out.features_dir_exists = DirExists(out.features_dir);

            out.searched_lib_dirs = CandidateLibDirs(out.root);
            out.library = FindFirstExistingLib(out.searched_lib_dirs, out.library_names);
            out.library_exists = !out.library.empty();

            if (!out.root_exists) {
                std::ostringstream oss;
                oss << "SDK root not found: " << out.root.string();
                if (env_override) {
                    oss << " (from " << env_var << ")";
                } else {
                    oss << " (override with " << env_var << ")";
                }
                out.problems.push_back(oss.str());
            } else {
                if (!out.library_exists) {
                    std::ostringstream oss;
                    oss << "Missing shared library (expected one of: "
                        << JoinStrings(out.library_names, ", ")
                        << "). Searched: " << SearchedDirsString(out.searched_lib_dirs);
                    out.problems.push_back(oss.str());
                }
                if (!out.models_dir_exists) {
                    out.problems.push_back("Missing models dir: " + out.models_dir.string());
                }
                if (!out.features_dir_exists) {
                    out.problems.push_back("Missing features dir: " + out.features_dir.string());
                }
            }

            out.ok = out.root_exists && out.library_exists && out.models_dir_exists && out.features_dir_exists;
            return out;
        }
    }  // namespace

    MaxinePathsReport ResolveMaxinePaths() {
        MaxinePathsReport rep;

        rep.vfx = ResolveComponent(
            "VFX",
            "STUDIOCAST_VFX_SDK_ROOT",
            util::DefaultVfxRoot(),
            fs::path("/usr/local/VideoFX"),
            {"libnvvfx.so", "libNvVFX.so"});

        rep.ar = ResolveComponent(
            "AR",
            "STUDIOCAST_AR_SDK_ROOT",
            util::DefaultArRoot(),
            fs::path("/usr/local/ARSDK"),
            {"libnvar.so", "libNvAR.so"});

        return rep;
    }

}  // namespace studiocast::maxine
