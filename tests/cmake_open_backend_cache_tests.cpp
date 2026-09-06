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

} // namespace

int main() {
  return TestMissingOnnxRuntimeDoesNotForceOpenBackendsOff() ? 0 : 1;
}
