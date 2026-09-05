// Pins the STUDIOCAST_ENABLE_LIBYUV configure option. The RPM spec passes ON
// or OFF from its libyuv build conditional, so the option must decide the
// build: ON has to fail the configure step when libyuv is missing, and OFF has
// to leave libyuv alone even on a machine that has it.
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>

#include <unistd.h>

#include "core/util/exec.h"

#ifndef STUDIOCAST_SOURCE_DIR
#define STUDIOCAST_SOURCE_DIR ""
#endif

#ifndef STUDIOCAST_CMAKE_COMMAND
#define STUDIOCAST_CMAKE_COMMAND "cmake"
#endif

namespace {

namespace fs = std::filesystem;

bool Expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << "\n";
    return false;
  }
  return true;
}

bool ExpectContains(const std::string &name, const std::string &haystack,
                    const std::string &needle) {
  if (haystack.find(needle) == std::string::npos) {
    std::cerr << name << " missing expected text: " << needle << "\n";
    return false;
  }
  return true;
}

std::string ShellQuote(const std::string &value) {
  std::string out = "'";
  for (char ch : value) {
    if (ch == '\'') {
      out += "'\\''";
    } else {
      out += ch;
    }
  }
  out += "'";
  return out;
}

class ScopedTempDir {
public:
  explicit ScopedTempDir(const std::string &prefix) {
    std::error_code ec;
    const fs::path base = fs::temp_directory_path(ec);
    if (ec) {
      error_ = "temp_directory_path failed: " + ec.message();
      return;
    }
    path_ = base /
            (prefix + "-" + std::to_string(static_cast<long long>(::getpid())));
    fs::remove_all(path_, ec);
    fs::create_directories(path_, ec);
    if (ec)
      error_ = "create_directories failed: " + ec.message();
  }

  ~ScopedTempDir() {
    std::error_code ec;
    fs::remove_all(path_, ec);
  }

  bool ok() const { return error_.empty(); }
  const std::string &error() const { return error_; }
  const fs::path &path() const { return path_; }

private:
  fs::path path_;
  std::string error_;
};

// Configures the repository into a throwaway build directory. Nothing is
// built, so a case costs one configure step.
studiocast::util::ExecResult
ConfigureRepository(const fs::path &build_dir, const std::string &extra_args,
                    const std::string &env_prefix) {
  const fs::path repo = fs::path(STUDIOCAST_SOURCE_DIR);
  const std::string command =
      env_prefix + ShellQuote(STUDIOCAST_CMAKE_COMMAND) + " -S " +
      ShellQuote(repo.string()) + " -B " + ShellQuote(build_dir.string()) +
      " -DBUILD_TESTING=OFF -DSTUDIOCAST_ENABLE_DLIB=OFF " + extra_args +
      " 2>&1";

  studiocast::util::ExecCaptureOptions options;
  options.timeout_ms = 180000;
  options.max_output_bytes = 2 * 1024 * 1024;
  return studiocast::util::ExecCapture(command, options);
}

// An explicit STUDIOCAST_ENABLE_LIBYUV=ON must not fall back quietly. The
// package build passes it, so a machine without libyuv has to stop here
// instead of producing a package that lost the backend without saying so.
bool TestExplicitLibyuvRequestFailsWhenLibyuvIsMissing() {
  ScopedTempDir temp("studiocast-cmake-libyuv-required");
  if (!Expect(temp.ok(), temp.error().c_str()))
    return false;

  const fs::path noPkgConfig = temp.path() / "empty-pkgconfig";
  std::error_code ec;
  fs::create_directories(noPkgConfig, ec);
  if (!Expect(!ec, "failed to create empty pkg-config directory"))
    return false;

  // An empty pkg-config path hides libyuv.pc, and the two pre-set cache
  // entries stop the find_path/find_library fallback from searching. So the
  // configure step sees no libyuv, whatever the machine really has.
  const std::string env_prefix =
      "PKG_CONFIG_LIBDIR=" + ShellQuote(noPkgConfig.string()) + " ";
  const auto result = ConfigureRepository(
      temp.path() / "build",
      "-DSTUDIOCAST_ENABLE_LIBYUV=ON -DLIBYUV_INCLUDE_DIR= -DLIBYUV_LIBRARY=",
      env_prefix);

  if (!Expect(result.exit_code != 0,
              "configure with STUDIOCAST_ENABLE_LIBYUV=ON must fail when "
              "libyuv is missing")) {
    std::cerr << result.stdout_str << "\n";
    return false;
  }

  return ExpectContains("required libyuv configure output", result.stdout_str,
                        "STUDIOCAST_ENABLE_LIBYUV") &&
         ExpectContains("required libyuv configure output", result.stdout_str,
                        "libyuv was not found");
}

// STUDIOCAST_ENABLE_LIBYUV=OFF must skip the detection, so a machine that has
// libyuv still gets a build without it. The RPM --without libyuv build depends
// on that.
bool TestDisabledLibyuvSkipsDetection() {
  ScopedTempDir temp("studiocast-cmake-libyuv-off");
  if (!Expect(temp.ok(), temp.error().c_str()))
    return false;

  const auto result = ConfigureRepository(temp.path() / "build",
                                          "-DSTUDIOCAST_ENABLE_LIBYUV=OFF", "");

  if (!Expect(result.exit_code == 0,
              "configure with STUDIOCAST_ENABLE_LIBYUV=OFF must succeed")) {
    std::cerr << result.stdout_str << "\n";
    return false;
  }

  return ExpectContains("disabled libyuv configure output", result.stdout_str,
                        "libyuv disabled") &&
         Expect(
             result.stdout_str.find("optimized RGB/YUV conversion enabled") ==
                 std::string::npos,
             "STUDIOCAST_ENABLE_LIBYUV=OFF must not enable libyuv");
}

} // namespace

int main() {
  const bool ok = TestExplicitLibyuvRequestFailsWhenLibyuvIsMissing() &&
                  TestDisabledLibyuvSkipsDetection();
  return ok ? 0 : 1;
}
