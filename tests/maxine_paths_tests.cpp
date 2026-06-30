#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <unistd.h>

#include "core/maxine/paths.h"

namespace {
namespace fs = std::filesystem;

class EnvGuard {
public:
  EnvGuard(const char *name, const std::string &value) : name_(name) {
    if (const char *old = std::getenv(name)) {
      old_value_ = std::string(old);
    }
    ::setenv(name, value.c_str(), 1);
  }

  ~EnvGuard() {
    if (old_value_) {
      ::setenv(name_, old_value_->c_str(), 1);
    } else {
      ::unsetenv(name_);
    }
  }

  EnvGuard(const EnvGuard &) = delete;
  EnvGuard &operator=(const EnvGuard &) = delete;

private:
  const char *name_;
  std::optional<std::string> old_value_;
};

bool Touch(const fs::path &path) {
  std::ofstream out(path, std::ios::out | std::ios::trunc);
  return out.good();
}

bool Require(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << message << "\n";
    return false;
  }
  return true;
}

bool TestCurrentLinuxMaxineLibraryNamesResolve() {
  const fs::path root =
      fs::temp_directory_path() /
      ("studiocast-maxine-paths-" + std::to_string(::getpid()));
  std::error_code ec;
  fs::remove_all(root, ec);

  const fs::path vfx = root / "VideoFX";
  const fs::path ar = root / "ARSDK";
  const fs::path afx = root / "Audio_Effects_SDK";

  fs::create_directories(vfx / "lib", ec);
  fs::create_directories(vfx / "models", ec);
  fs::create_directories(vfx / "features", ec);
  fs::create_directories(ar / "lib", ec);
  fs::create_directories(ar / "models", ec);
  fs::create_directories(ar / "features", ec);
  fs::create_directories(afx / "nvafx" / "lib", ec);
  fs::create_directories(afx / "features", ec);
  if (ec) {
    std::cerr << "failed to create test SDK layout: " << ec.message() << "\n";
    return false;
  }

  if (!Touch(vfx / "lib" / "libVideoFX.so") ||
      !Touch(ar / "lib" / "libnvARPose.so") ||
      !Touch(afx / "nvafx" / "lib" / "libnv_audiofx.so")) {
    std::cerr << "failed to create fake Maxine libraries\n";
    fs::remove_all(root, ec);
    return false;
  }

  EnvGuard vfx_env("STUDIOCAST_VFX_SDK_ROOT", vfx.string());
  EnvGuard ar_env("STUDIOCAST_AR_SDK_ROOT", ar.string());
  EnvGuard afx_env("STUDIOCAST_AFX_SDK_ROOT", afx.string());

  const auto rep = studiocast::maxine::ResolveMaxinePaths();

  bool ok = true;
  ok &= Require(rep.vfx.ok, "expected VFX component to resolve");
  ok &= Require(rep.vfx.library == vfx / "lib" / "libVideoFX.so",
                "expected VFX to resolve libVideoFX.so, got " +
                    rep.vfx.library.string());
  ok &= Require(rep.ar.ok, "expected AR component to resolve");
  ok &= Require(rep.ar.library == ar / "lib" / "libnvARPose.so",
                "expected AR to resolve libnvARPose.so, got " +
                    rep.ar.library.string());
  ok &= Require(rep.afx.ok, "expected AFX component to resolve");
  ok &= Require(rep.afx.library == afx / "nvafx" / "lib" / "libnv_audiofx.so",
                "expected AFX to resolve libnv_audiofx.so, got " +
                    rep.afx.library.string());

  fs::remove_all(root, ec);
  return ok;
}

} // namespace

int main() {
  if (!TestCurrentLinuxMaxineLibraryNamesResolve()) {
    std::cout << "[FAIL] current Linux Maxine library names resolve\n";
    return 1;
  }

  std::cout << "[PASS] current Linux Maxine library names resolve\n";
  return 0;
}
