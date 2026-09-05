// Pins the STUDIOCAST_ENABLE_LIBYUV configure option. The RPM spec passes ON
// or OFF from its libyuv build conditional, so the option must decide the
// build: ON has to fail the configure step when libyuv is missing, and OFF has
// to leave libyuv alone even on a machine that has it.
#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>

#include "core/util/exec.h"
#include "scoped_temp_dir.h"

#ifndef STUDIOCAST_SOURCE_DIR
#define STUDIOCAST_SOURCE_DIR ""
#endif

#ifndef STUDIOCAST_CMAKE_COMMAND
#define STUDIOCAST_CMAKE_COMMAND "cmake"
#endif

namespace {

namespace fs = std::filesystem;

using studiocast::tests::ScopedTempDir;

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

  // Empty pkg-config search paths hide libyuv.pc, and the two pre-set cache
  // entries stop the find_path/find_library fallback from searching. So the
  // configure step sees no libyuv, whatever the machine really has. Both
  // variables are needed: PKG_CONFIG_PATH is searched as well as
  // PKG_CONFIG_LIBDIR, and an RPM build sets it.
  const std::string empty = ShellQuote(noPkgConfig.string());
  const std::string env_prefix =
      "PKG_CONFIG_LIBDIR=" + empty + " PKG_CONFIG_PATH=" + empty + " ";
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

// Two helpers with one prefix must not name the same directory. A predictable
// name in the temp directory lets one run remove the directory of another, and
// lets anyone else on the machine create it first.
bool TestTempDirGivesEachInstanceItsOwnPath() {
  fs::path first_path;
  {
    ScopedTempDir first("studiocast-cmake-temp-check");
    ScopedTempDir second("studiocast-cmake-temp-check");
    if (!Expect(first.ok(), first.error().c_str()) ||
        !Expect(second.ok(), second.error().c_str()))
      return false;
    if (!Expect(first.path() != second.path(),
                "two temp directories with one prefix must differ"))
      return false;

    std::error_code ec;
    if (!Expect(fs::is_directory(first.path(), ec) &&
                    fs::is_directory(second.path(), ec),
                "both temp directories must exist"))
      return false;
    first_path = first.path();
  }

  std::error_code ec;
  return Expect(!fs::exists(first_path, ec),
                "the helper must remove its directory at the end of the scope");
}

} // namespace

int main() {
  const bool ok = TestTempDirGivesEachInstanceItsOwnPath() &&
                  TestExplicitLibyuvRequestFailsWhenLibyuvIsMissing() &&
                  TestDisabledLibyuvSkipsDetection();
  return ok ? 0 : 1;
}
