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

// Write a tiny project that runs the ONNX Runtime module and nothing else.
// The two root checks below only look at that module, so they do not need to
// configure the whole build.
bool WriteOnnxRuntimeProbeProject(const fs::path &dir) {
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (!Expect(!ec, "failed to create the probe project directory"))
    return false;

  std::ofstream out(dir / "CMakeLists.txt");
  out << "cmake_minimum_required(VERSION 3.16)\n"
      << "project(studiocast_ort_probe LANGUAGES CXX)\n"
      << "list(APPEND CMAKE_MODULE_PATH \""
      << fs::path(STUDIOCAST_SOURCE_DIR).string() << "/cmake\")\n"
      << "include(OnnxRuntime)\n"
      << "studiocast_configure_onnxruntime(ORT_PROBE_FOUND ORT_PROBE_TARGET)\n"
      << "message(STATUS \"ORT_PROBE_FOUND=${ORT_PROBE_FOUND}\")\n";
  out.close();
  return Expect(static_cast<bool>(out), "failed to write the probe project");
}

bool WriteEmptyFile(const fs::path &path) {
  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  if (!Expect(!ec, "failed to create a fake root directory"))
    return false;
  std::ofstream out(path);
  out.close();
  return Expect(static_cast<bool>(out), "failed to write a fake root file");
}

// The command that configures the probe project against one root. The decoy
// directory goes into CMAKE_LIBRARY_PATH, which find_library searches before
// any HINTS, so a search that leaves the root picks the decoy library.
std::string OnnxRuntimeProbeCommand(const fs::path &project,
                                    const fs::path &buildDir,
                                    const fs::path &root, const fs::path &decoy,
                                    const fs::path &noPkgConfig) {
  return "env -u ONNXRUNTIME_ROOT PKG_CONFIG_LIBDIR=" +
         ShellQuote(noPkgConfig.string()) + " " +
         ShellQuote(STUDIOCAST_CMAKE_COMMAND) + " -S " +
         ShellQuote(project.string()) + " -B " + ShellQuote(buildDir.string()) +
         " -DONNXRUNTIME_ROOT=" + ShellQuote(root.string()) +
         " -DCMAKE_LIBRARY_PATH=" + ShellQuote(decoy.string()) + " 2>&1";
}

// With an explicit root, both the header and the library must come out of that
// root. A library from anywhere else would pair the headers of one build with
// the binary of another.
bool TestExplicitOnnxRuntimeRootTakesBothPartsFromTheRoot() {
  ScopedTempDir temp("studiocast-cmake-ort-root-only");
  if (!Expect(temp.ok(), temp.error().c_str()))
    return false;

  const fs::path project = temp.path() / "project";
  const fs::path buildDir = temp.path() / "build";
  const fs::path root = temp.path() / "root";
  const fs::path decoy = temp.path() / "decoy";
  const fs::path noPkgConfig = temp.path() / "empty-pkgconfig";

  std::error_code ec;
  fs::create_directories(noPkgConfig, ec);
  if (!Expect(!ec, "failed to create empty pkg-config directory"))
    return false;
  if (!WriteOnnxRuntimeProbeProject(project))
    return false;
  if (!WriteEmptyFile(root / "include" / "onnxruntime_cxx_api.h"))
    return false;
  if (!WriteEmptyFile(root / "lib" / "libonnxruntime.so"))
    return false;
  if (!WriteEmptyFile(decoy / "libonnxruntime.so"))
    return false;

  studiocast::util::ExecCaptureOptions options;
  options.timeout_ms = 120000;
  options.max_output_bytes = 2 * 1024 * 1024;
  const auto result = studiocast::util::ExecCapture(
      OnnxRuntimeProbeCommand(project, buildDir, root, decoy, noPkgConfig),
      options);
  if (!Expect(result.exit_code == 0,
              "the probe configure with a complete root should succeed")) {
    std::cerr << result.stdout_str << "\n";
    return false;
  }

  const std::string cache = ReadFile(buildDir / "CMakeCache.txt");
  return ExpectContains("probe CMake cache", cache,
                        "ONNXRUNTIME_INCLUDE_DIR:PATH=" +
                            (root / "include").string()) &&
         ExpectContains("probe CMake cache", cache,
                        "ONNXRUNTIME_LIBRARY:FILEPATH=" +
                            (root / "lib" / "libonnxruntime.so").string());
}

// A root without a library must stop the configure with a message that names
// the root. Falling back to a library outside the root, or to a distribution
// package, would hide the wrong root.
bool TestExplicitOnnxRuntimeRootWithoutALibraryFails() {
  ScopedTempDir temp("studiocast-cmake-ort-root-bad");
  if (!Expect(temp.ok(), temp.error().c_str()))
    return false;

  const fs::path project = temp.path() / "project";
  const fs::path buildDir = temp.path() / "build";
  const fs::path root = temp.path() / "root";
  const fs::path decoy = temp.path() / "decoy";
  const fs::path noPkgConfig = temp.path() / "empty-pkgconfig";

  std::error_code ec;
  fs::create_directories(noPkgConfig, ec);
  if (!Expect(!ec, "failed to create empty pkg-config directory"))
    return false;
  if (!WriteOnnxRuntimeProbeProject(project))
    return false;
  if (!WriteEmptyFile(root / "include" / "onnxruntime_cxx_api.h"))
    return false;
  if (!WriteEmptyFile(decoy / "libonnxruntime.so"))
    return false;

  studiocast::util::ExecCaptureOptions options;
  options.timeout_ms = 120000;
  options.max_output_bytes = 2 * 1024 * 1024;
  const auto result = studiocast::util::ExecCapture(
      OnnxRuntimeProbeCommand(project, buildDir, root, decoy, noPkgConfig),
      options);
  if (!Expect(result.exit_code != 0,
              "a root without a library should stop the configure")) {
    std::cerr << result.stdout_str << "\n";
    return false;
  }

  return ExpectContains("probe configure output", result.stdout_str,
                        "ONNXRUNTIME_ROOT=" + root.string()) &&
         Expect(result.stdout_str.find(decoy.string()) == std::string::npos,
                "the root search must not report a library outside the root");
}

} // namespace

int main() {
  return (TestMissingOnnxRuntimeDoesNotForceOpenBackendsOff() &&
          TestEmptyOnnxRuntimeRootDoesNotRunTheRootSearch() &&
          TestExplicitOnnxRuntimeRootTakesBothPartsFromTheRoot() &&
          TestExplicitOnnxRuntimeRootWithoutALibraryFails())
             ? 0
             : 1;
}
