#include "probe.h"

#include <sys/utsname.h>

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "core/util/exec.h"
#include "core/util/fs.h"
#include "core/util/os_release.h"
#include "core/util/strings.h"
#include "studiocast/version.h"

namespace fs = std::filesystem;

namespace studiocast::probe {
namespace {

constexpr int kRequiredDriverMajor = 570;
constexpr int kRequiredDriverMinor = 26;

std::string KernelString() {
  utsname u{};
  if (uname(&u) != 0) return "unknown";
  std::ostringstream oss;
  oss << u.sysname << " " << u.release << " (" << u.machine << ")";
  return oss.str();
}

std::optional<Version> ParseVersionLike(const std::string& s) {
  // Find first token that looks like digits[.digits][.digits]
  // e.g. "570.26" or "570.26.02"
  std::string token;
  for (size_t i = 0; i < s.size(); ++i) {
    if (!std::isdigit(static_cast<unsigned char>(s[i]))) continue;
    size_t j = i;
    while (j < s.size()) {
      char c = s[j];
      if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
        ++j;
      } else {
        break;
      }
    }
    token = s.substr(i, j - i);
    break;
  }
  if (token.empty()) return std::nullopt;

  std::vector<std::string> parts = util::Split(token, '.');
  if (parts.size() < 2) return std::nullopt;

  Version v;
  v.original = util::TrimCopy(token);
  v.major = std::atoi(parts[0].c_str());
  v.minor = std::atoi(parts[1].c_str());
  if (parts.size() >= 3) {
    v.patch = std::atoi(parts[2].c_str());
    v.has_patch = true;
  }
  return v;
}

bool MeetsRequiredDriver(const Version& v) {
  if (v.major != kRequiredDriverMajor) return v.major > kRequiredDriverMajor;
  return v.minor >= kRequiredDriverMinor;
}

std::optional<Version> DetectDriverVersion() {
  // 1) /proc/driver/nvidia/version is the most direct when the kernel module is loaded.
  if (auto content = util::ReadTextFile("/proc/driver/nvidia/version")) {
    if (auto v = ParseVersionLike(*content)) return v;
  }

  // 2) fallback to nvidia-smi (if installed and in PATH)
  auto out = util::ExecCapture("nvidia-smi --query-gpu=driver_version --format=csv,noheader 2>/dev/null");
  if (out.exit_code == 0) {
    const std::string line = util::FirstNonEmptyLine(out.stdout_str);
    if (!line.empty()) {
      if (auto v = ParseVersionLike(line)) return v;
    }
  }

  return std::nullopt;
}

std::vector<std::string> DetectGpus() {
  auto out = util::ExecCapture("nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null");
  if (out.exit_code != 0) return {};

  std::vector<std::string> lines = util::SplitLines(out.stdout_str);
  std::vector<std::string> gpus;
  for (auto& l : lines) {
    auto t = util::TrimCopy(l);
    if (!t.empty()) gpus.push_back(t);
  }
  return gpus;
}

fs::path GetEnvPathAny(std::initializer_list<const char*> names) {
  for (const char* n : names) {
    const char* v = std::getenv(n);
    if (v && *v) return fs::path(v);
  }
  return {};
}

CheckResult CheckDriver(const std::optional<Version>& v) {
  CheckResult r;
  r.name = "NVIDIA driver >= 570.26 (Maxine requirement)";
  if (!v) {
    r.ok = false;
    r.details = "Driver version not detected (missing driver, kernel module not loaded, or nvidia-smi not available).";
    return r;
  }

  r.ok = MeetsRequiredDriver(*v);
  std::ostringstream oss;
  oss << "Detected: " << v->original << " | Required: " << kRequiredDriverMajor << "." << kRequiredDriverMinor;
  r.details = oss.str();
  return r;
}

CheckResult CheckVfxCore(const fs::path& root) {
  CheckResult r;
  r.name = "Maxine VFX SDK core present (/usr/local/VideoFX)";
  if (root.empty()) {
    r.ok = false;
    r.details = "Root path not set.";
    return r;
  }
  if (!fs::exists(root)) {
    r.ok = false;
    r.details = "Not found: " + root.string();
    return r;
  }
  r.ok = true;
  r.details = "Found: " + root.string();
  return r;
}

CheckResult CheckVfxInstallScript(const fs::path& root) {
  CheckResult r;
  r.name = "VFX features install script present (features/install_feature.sh)";
  const fs::path script = root / "features" / "install_feature.sh";
  if (fs::exists(script)) {
    r.ok = true;
    r.details = "Found: " + script.string();
  } else {
    r.ok = false;
    r.details = "Missing: " + script.string();
  }
  return r;
}

CheckResult CheckArCore(const fs::path& root) {
  CheckResult r;
  r.name = "Maxine AR SDK core present (/usr/local/ARSDK)";
  if (root.empty()) {
    r.ok = false;
    r.details = "Root path not set.";
    return r;
  }
  if (!fs::exists(root)) {
    r.ok = false;
    r.details = "Not found: " + root.string();
    return r;
  }
  r.ok = true;
  r.details = "Found: " + root.string();
  return r;
}

CheckResult CheckArInstallScript(const fs::path& root) {
  CheckResult r;
  r.name = "AR features install script present (features/install_feature.sh)";
  const fs::path script = root / "features" / "install_feature.sh";
  if (fs::exists(script)) {
    r.ok = true;
    r.details = "Found: " + script.string();
  } else {
    r.ok = false;
    r.details = "Missing: " + script.string();
  }
  return r;
}

CheckResult CheckAfxRoot(const fs::path& root) {
  CheckResult r;
  r.name = "Maxine AFX SDK root set (AFX_SDK_ROOT)";
  if (root.empty()) {
    r.ok = false;
    r.details = "AFX_SDK_ROOT not set. Extract AFX core package somewhere and set AFX_SDK_ROOT to that folder.";
    return r;
  }
  if (!fs::exists(root)) {
    r.ok = false;
    r.details = "AFX_SDK_ROOT points to missing path: " + root.string();
    return r;
  }
  r.ok = true;
  r.details = "AFX_SDK_ROOT=" + root.string();
  return r;
}

CheckResult CheckAfxFeaturesDir(const fs::path& root) {
  CheckResult r;
  r.name = "AFX features directory present (features/)";
  const fs::path features = root / "features";
  if (!root.empty() && fs::exists(features) && fs::is_directory(features)) {
    r.ok = true;
    r.details = "Found: " + features.string();
  } else {
    r.ok = false;
    r.details = "Missing: " + features.string();
  }
  return r;
}

std::string JsonEscape(const std::string& s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          out += "?";
        } else {
          out += c;
        }
    }
  }
  return out;
}

}  // namespace

bool Report::AllChecksPassed() const {
  for (const auto& c : checks) {
    if (!c.ok) return false;
  }
  return true;
}

std::string Report::ToText() const {
  std::ostringstream oss;
  oss << "StudioCast Probe\n";
  oss << "Version: " << app_version << " (" << app_git_sha << ")\n\n";

  oss << "System\n";
  oss << "  OS: " << (os_pretty_name.empty() ? "unknown" : os_pretty_name) << "\n";
  oss << "  Kernel: " << (kernel.empty() ? "unknown" : kernel) << "\n";
  if (nvidia_driver) {
    oss << "  NVIDIA Driver: " << nvidia_driver->original << "\n";
  } else {
    oss << "  NVIDIA Driver: not detected\n";
  }

  if (!gpus.empty()) {
    oss << "  GPU(s):\n";
    for (const auto& g : gpus) oss << "    - " << g << "\n";
  } else {
    oss << "  GPU(s): none detected via nvidia-smi\n";
  }

  oss << "\nChecks\n";
  for (const auto& c : checks) {
    oss << "  [" << (c.ok ? "OK" : "FAIL") << "] " << c.name << "\n";
    if (!c.details.empty()) oss << "       " << c.details << "\n";
  }

  if (!notes.empty()) {
    oss << "\nNotes\n";
    for (const auto& n : notes) oss << "  - " << n << "\n";
  }

  return oss.str();
}

std::string Report::ToJson() const {
  std::ostringstream oss;
  oss << "{";
  oss << "\"app_version\":\"" << JsonEscape(app_version) << "\",";
  oss << "\"app_git_sha\":\"" << JsonEscape(app_git_sha) << "\",";
  oss << "\"os_pretty_name\":\"" << JsonEscape(os_pretty_name) << "\",";
  oss << "\"kernel\":\"" << JsonEscape(kernel) << "\",";

  if (nvidia_driver) {
    oss << "\"nvidia_driver\":\"" << JsonEscape(nvidia_driver->original) << "\",";
  } else {
    oss << "\"nvidia_driver\":null,";
  }

  oss << "\"gpus\":[";
  for (size_t i = 0; i < gpus.size(); ++i) {
    if (i) oss << ",";
    oss << "\"" << JsonEscape(gpus[i]) << "\"";
  }
  oss << "],";

  oss << "\"checks\":[";
  for (size_t i = 0; i < checks.size(); ++i) {
    if (i) oss << ",";
    oss << "{"
        << "\"name\":\"" << JsonEscape(checks[i].name) << "\","
        << "\"ok\":" << (checks[i].ok ? "true" : "false") << ","
        << "\"details\":\"" << JsonEscape(checks[i].details) << "\""
        << "}";
  }
  oss << "],";

  oss << "\"notes\":[";
  for (size_t i = 0; i < notes.size(); ++i) {
    if (i) oss << ",";
    oss << "\"" << JsonEscape(notes[i]) << "\"";
  }
  oss << "]";

  oss << "}";
  return oss.str();
}

Report Run(bool /*verbose*/) {
  Report rep;
  rep.app_version = STUDIOCAST_VERSION;
  rep.app_git_sha = STUDIOCAST_GIT_SHA;

  rep.os_pretty_name = util::ReadOsPrettyName();
  rep.kernel = KernelString();

  rep.nvidia_driver = DetectDriverVersion();
  rep.gpus = DetectGpus();

  // Standard Linux install locations for Maxine VFX/AR core packages.
  const fs::path vfxRoot = "/usr/local/VideoFX";
  const fs::path arRoot  = "/usr/local/ARSDK";

  // AFX root is user-chosen; we follow NVIDIA's common convention env var.
  const fs::path afxRoot = GetEnvPathAny({"AFX_SDK_ROOT", "STUDIOCAST_AFX_SDK_ROOT"});

  rep.checks.push_back(CheckDriver(rep.nvidia_driver));
  rep.checks.push_back(CheckVfxCore(vfxRoot));
  rep.checks.push_back(CheckVfxInstallScript(vfxRoot));
  rep.checks.push_back(CheckArCore(arRoot));
  rep.checks.push_back(CheckArInstallScript(arRoot));
  rep.checks.push_back(CheckAfxRoot(afxRoot));
  rep.checks.push_back(CheckAfxFeaturesDir(afxRoot));

  rep.notes.push_back("VFX/AR/AFX SDKs require feature/model packages (not included in core). Use install scripts with an NGC API key.");
  rep.notes.push_back("These SDKs are documented as optimized for server-side deployment; desktop use is not officially supported.");

  return rep;
}

}  // namespace studiocast::probe
