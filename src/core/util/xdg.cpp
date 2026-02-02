#include "xdg.h"

#include <cstdlib>
#include <pwd.h>
#include <string>
#include <unistd.h>

namespace fs = std::filesystem;

namespace studiocast::util {
    namespace {

        fs::path GetEnvPath(const char* name) {
            const char* v = std::getenv(name);
            if (!v || !*v) return {};
            return fs::path(v);
        }

    }  // namespace

    fs::path HomeDir() {
        if (auto p = GetEnvPath("HOME"); !p.empty()) return p;

        passwd* pw = getpwuid(getuid());
        if (pw && pw->pw_dir && *pw->pw_dir) return fs::path(pw->pw_dir);

        return {};
    }

    fs::path XdgDataHome() {
        if (auto p = GetEnvPath("XDG_DATA_HOME"); !p.empty()) return p;

        const auto home = HomeDir();
        if (home.empty()) return {};
        return home / ".local" / "share";
    }

    fs::path XdgConfigHome() {
        if (auto p = GetEnvPath("XDG_CONFIG_HOME"); !p.empty()) return p;

        const auto home = HomeDir();
        if (home.empty()) return {};
        return home / ".config";
    }

    fs::path XdgRuntimeDir() {
        // XDG_RUNTIME_DIR is expected to be set in most desktop sessions.
        // It points to a user-private, tmpfs-backed directory like /run/user/$UID.
        if (auto p = GetEnvPath("XDG_RUNTIME_DIR"); !p.empty()) return p;

        // Fallback for non-systemd / atypical environments.
        // Use a per-user subdir under /tmp.
        std::error_code ec;
        const auto tmp = fs::temp_directory_path(ec);
        if (ec) return {};

        return tmp / (std::string("studiocast-runtime-") + std::to_string(::getuid()));
    }

    fs::path StudioCastDataDir() {
        const auto dataHome = XdgDataHome();
        if (dataHome.empty()) return {};
        return dataHome / "studiocast";
    }

    fs::path StudioCastConfigDir() {
        const auto configHome = XdgConfigHome();
        if (configHome.empty()) return {};
        return configHome / "studiocast";
    }

    fs::path StudioCastMaxineDir() {
        const auto dataDir = StudioCastDataDir();
        if (dataDir.empty()) return {};
        return dataDir / "maxine";
    }

    fs::path StudioCastRuntimeDir() {
        const auto rt = XdgRuntimeDir();
        if (rt.empty()) return {};
        return rt / "studiocast";
    }

    fs::path DefaultVfxRoot() {
        const auto base = StudioCastMaxineDir();
        if (base.empty()) return {};
        return base / "VideoFX";
    }

    fs::path DefaultArRoot() {
        const auto base = StudioCastMaxineDir();
        if (base.empty()) return {};
        return base / "ARSDK";
    }

    fs::path DefaultAfxRoot() {
        const auto base = StudioCastMaxineDir();
        if (base.empty()) return {};
        return base / "Audio_Effects_SDK";
    }

    fs::path XdgStateHome() {
        if (auto p = GetEnvPath("XDG_STATE_HOME"); !p.empty()) return p;

        const auto home = HomeDir();
        if (home.empty()) return {};
        return home / ".local" / "state";
    }

    fs::path StudioCastStateDir() {
        const auto stateHome = XdgStateHome();
        if (stateHome.empty()) return {};
        return stateHome / "studiocast";
    }
}  // namespace studiocast::util
