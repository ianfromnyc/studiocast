#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

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

std::string ReadFile(const fs::path &path) {
  std::ifstream in(path);
  std::ostringstream out;
  out << in.rdbuf();
  return out.str();
}

bool TestMissingOnnxRuntimeDoesNotForceOpenBackendsOff() {
  ScopedTempDir temp("studiocast-cmake-open-backend-cache");
  if (!Expect(temp.ok(), temp.error().c_str()))
    return false;

  const fs::path repo = fs::path(STUDIOCAST_SOURCE_DIR);
  const fs::path buildDir = temp.path() / "build";
  const fs::path noPkgConfig = temp.path() / "empty-pkgconfig";
  std::error_code ec;
  fs::create_directories(noPkgConfig, ec);
  if (!Expect(!ec, "failed to create empty pkg-config directory"))
    return false;

  std::string command = "env -u ONNXRUNTIME_ROOT PKG_CONFIG_LIBDIR=" +
                        ShellQuote(noPkgConfig.string()) + " " +
                        ShellQuote(STUDIOCAST_CMAKE_COMMAND) + " -S " +
                        ShellQuote(repo.string()) + " -B " +
                        ShellQuote(buildDir.string()) +
                        " -DBUILD_TESTING=OFF"
                        " -DSTUDIOCAST_ENABLE_DLIB=OFF"
                        " -DSTUDIOCAST_ENABLE_OPEN_CUDA=ON"
                        " -DSTUDIOCAST_ENABLE_OPEN_AUDIO=ON"
                        " -DCMAKE_DISABLE_FIND_PACKAGE_onnxruntime=ON"
                        " -DCMAKE_DISABLE_FIND_PACKAGE_Python3=ON"
                        " 2>&1";

  studiocast::util::ExecCaptureOptions options;
  options.timeout_ms = 60000;
  options.max_output_bytes = 2 * 1024 * 1024;
  const auto result = studiocast::util::ExecCapture(command, options);
  if (!Expect(result.exit_code == 0,
              "nested CMake configure without ONNX Runtime should succeed")) {
    std::cerr << result.stdout_str << "\n";
    return false;
  }

  const std::string cache = ReadFile(buildDir / "CMakeCache.txt");
  return ExpectContains("nested CMake cache", cache,
                        "STUDIOCAST_ENABLE_OPEN_CUDA:BOOL=ON") &&
         ExpectContains("nested CMake cache", cache,
                        "STUDIOCAST_ENABLE_OPEN_AUDIO:BOOL=ON") &&
         Expect(cache.find("STUDIOCAST_ENABLE_OPEN_CUDA:BOOL=OFF") ==
                    std::string::npos,
                "Open CUDA must not be force-cached OFF when ONNX Runtime is "
                "missing");
}

// An empty -DONNXRUNTIME_ROOT= must mean "no root", the same as an unset one.
// The root search (step 4 of cmake/OnnxRuntime.cmake) must stay off, because
// with an empty hint it searches the system directories and shadows the
// distro and pkg-config order above it. find_path() and find_library() always
// write their cache entry, so the absence of ONNXRUNTIME_INCLUDE_DIR in the
// cache shows that the step did not run.
bool TestEmptyOnnxRuntimeRootDoesNotRunTheRootSearch() {
  ScopedTempDir temp("studiocast-cmake-empty-ort-root");
  if (!Expect(temp.ok(), temp.error().c_str()))
    return false;

  const fs::path repo = fs::path(STUDIOCAST_SOURCE_DIR);
  const fs::path buildDir = temp.path() / "build";
  const fs::path noPkgConfig = temp.path() / "empty-pkgconfig";
  std::error_code ec;
  fs::create_directories(noPkgConfig, ec);
  if (!Expect(!ec, "failed to create empty pkg-config directory"))
    return false;

  std::string command = "env -u ONNXRUNTIME_ROOT PKG_CONFIG_LIBDIR=" +
                        ShellQuote(noPkgConfig.string()) + " " +
                        ShellQuote(STUDIOCAST_CMAKE_COMMAND) + " -S " +
                        ShellQuote(repo.string()) + " -B " +
                        ShellQuote(buildDir.string()) +
                        " -DONNXRUNTIME_ROOT="
                        " -DBUILD_TESTING=OFF"
                        " -DSTUDIOCAST_ENABLE_DLIB=OFF"
                        " -DSTUDIOCAST_ENABLE_OPEN_CUDA=ON"
                        " -DSTUDIOCAST_ENABLE_OPEN_AUDIO=ON"
                        " -DCMAKE_DISABLE_FIND_PACKAGE_onnxruntime=ON"
                        " -DCMAKE_DISABLE_FIND_PACKAGE_Python3=ON"
                        " 2>&1";

  studiocast::util::ExecCaptureOptions options;
  options.timeout_ms = 60000;
  options.max_output_bytes = 2 * 1024 * 1024;
  const auto result = studiocast::util::ExecCapture(command, options);
  if (!Expect(result.exit_code == 0,
              "nested CMake configure with an empty ONNXRUNTIME_ROOT should "
              "succeed")) {
    std::cerr << result.stdout_str << "\n";
    return false;
  }

  if (!Expect(result.stdout_str.find("using the explicit ONNXRUNTIME_ROOT") ==
                  std::string::npos,
              "an empty ONNXRUNTIME_ROOT must not count as an explicit root")) {
    std::cerr << result.stdout_str << "\n";
    return false;
  }

  const std::string cache = ReadFile(buildDir / "CMakeCache.txt");
  return Expect(cache.find("ONNXRUNTIME_INCLUDE_DIR") == std::string::npos,
                "the root search must not run for an empty ONNXRUNTIME_ROOT") &&
         Expect(cache.find("ONNXRUNTIME_LIBRARY") == std::string::npos,
                "the root search must not run for an empty ONNXRUNTIME_ROOT");
}

} // namespace

int main() {
  return (TestMissingOnnxRuntimeDoesNotForceOpenBackendsOff() &&
          TestEmptyOnnxRuntimeRootDoesNotRunTheRootSearch())
             ? 0
             : 1;
}
