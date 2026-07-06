#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

#include <unistd.h>

#include "core/util/exec.h"

#ifndef STUDIOCAST_SOURCE_DIR
#define STUDIOCAST_SOURCE_DIR ""
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
    std::cerr << "Output:\n" << haystack << "\n";
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

std::string BackendCommand(const ScopedTempDir &root,
                           const std::string &subcommand,
                           const std::string &extra_options = "") {
  const fs::path repo = fs::path(STUDIOCAST_SOURCE_DIR);
  const fs::path backend =
      repo / "installer" / "backend" / "studiocast-installer-backend";
  const fs::path buildDir = root.path() / "build";
  std::string command =
      "HOME=" + ShellQuote((root.path() / "home").string()) + " " +
      ShellQuote(backend.string()) + " " + subcommand + " --source-dir " +
      ShellQuote(repo.string()) + " --build-dir " +
      ShellQuote(buildDir.string()) +
      " --skip-deps --no-v4l2loopback --no-service --no-models "
      "--allow-unsupported";
  if (!extra_options.empty()) {
    command += " " + extra_options;
  }
  return command;
}

bool TestRepairPlanIncludesDefaultOpenBackendConfigureFlags() {
  ScopedTempDir temp("studiocast-installer-backend-plan");
  if (!Expect(temp.ok(), temp.error().c_str()))
    return false;

  studiocast::util::ExecCaptureOptions options;
  options.timeout_ms = 10000;
  const auto result = studiocast::util::ExecCapture(
      BackendCommand(temp, "plan repair --json"), options);

  return Expect(result.exit_code == 0,
                "installer backend plan repair should exit successfully") &&
         ExpectContains("installer backend plan", result.stdout_str,
                        "-DSTUDIOCAST_ENABLE_OPEN_CUDA=ON") &&
         ExpectContains("installer backend plan", result.stdout_str,
                        "-DSTUDIOCAST_ENABLE_OPEN_AUDIO=ON") &&
         ExpectContains("installer backend plan", result.stdout_str,
                        "Force Linux CMake configuration to keep Open Video/"
                        "Open CUDA and Open Audio enabled");
}

bool TestRepairPlanCanDisableOpenBackendConfigureFlags() {
  ScopedTempDir temp("studiocast-installer-backend-no-open");
  if (!Expect(temp.ok(), temp.error().c_str()))
    return false;

  studiocast::util::ExecCaptureOptions options;
  options.timeout_ms = 10000;
  const auto result = studiocast::util::ExecCapture(
      BackendCommand(temp, "plan repair --json", "--no-open-backends"),
      options);

  return Expect(result.exit_code == 0,
                "installer backend plan repair without open backends should "
                "exit successfully") &&
         Expect(result.stdout_str.find("-DSTUDIOCAST_ENABLE_OPEN_CUDA=ON") ==
                    std::string::npos,
                "disabled Open Source backend setup should omit Open CUDA "
                "configure flag") &&
         Expect(result.stdout_str.find("-DSTUDIOCAST_ENABLE_OPEN_AUDIO=ON") ==
                    std::string::npos,
                "disabled Open Source backend setup should omit Open Audio "
                "configure flag");
}

bool TestRepairDryRunIncludesOpenBackendConfigureFlags() {
  ScopedTempDir temp("studiocast-installer-backend-dry-run");
  if (!Expect(temp.ok(), temp.error().c_str()))
    return false;

  studiocast::util::ExecCaptureOptions options;
  options.timeout_ms = 10000;
  const auto result = studiocast::util::ExecCapture(
      BackendCommand(temp, "repair --dry-run"), options);

  return Expect(result.exit_code == 0,
                "installer backend repair dry-run should exit successfully") &&
         ExpectContains("installer backend repair dry-run", result.stdout_str,
                        "-DSTUDIOCAST_ENABLE_OPEN_CUDA=ON") &&
         ExpectContains("installer backend repair dry-run", result.stdout_str,
                        "-DSTUDIOCAST_ENABLE_OPEN_AUDIO=ON");
}

bool TestStatusReportsOptionalComponents() {
  ScopedTempDir temp("studiocast-installer-backend-status");
  if (!Expect(temp.ok(), temp.error().c_str()))
    return false;

  studiocast::util::ExecCaptureOptions options;
  options.timeout_ms = 10000;
  const auto result = studiocast::util::ExecCapture(
      BackendCommand(temp, "status --json"), options);

  return Expect(result.exit_code == 0,
                "installer backend status should exit successfully") &&
         ExpectContains("installer backend status", result.stdout_str,
                        "\"optional_components\"") &&
         ExpectContains("installer backend status", result.stdout_str,
                        "\"onnxruntime_cuda\"") &&
         ExpectContains("installer backend status", result.stdout_str,
                        "docs/open_source_video_models_install.md") &&
         ExpectContains("installer backend status", result.stdout_str,
                        "docs/maxine_install.md");
}

} // namespace

int main() {
  bool ok = true;
  ok = TestRepairPlanIncludesDefaultOpenBackendConfigureFlags() && ok;
  ok = TestRepairPlanCanDisableOpenBackendConfigureFlags() && ok;
  ok = TestRepairDryRunIncludesOpenBackendConfigureFlags() && ok;
  ok = TestStatusReportsOptionalComponents() && ok;
  return ok ? 0 : 1;
}
