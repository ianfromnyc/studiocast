#include "xdg.h"

#include <cstdlib>
#include <pwd.h>
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

}  // namespace studiocast::util
