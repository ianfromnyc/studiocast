#include "probe.h"

#include <sys/utsname.h>

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "core/config/settings.h"
#include "core/maxine/gpu.h"
#include "core/util/exec.h"
#include "core/util/fs.h"
#include "core/util/os_release.h"
#include "core/util/strings.h"
#include "core/util/xdg.h"
#include "studiocast/version.h"

namespace fs = std::filesystem;

namespace studiocast::probe {
namespace {
constexpr int kRequiredDriverMajor = 570;
constexpr int kRequiredDriverMinor = 26;

// Heuristic: Tensor Core era (Turing+) roughly maps to compute capability
// >= 7.5. Best-effort only.
constexpr int kMinCcMajor = 7;
constexpr int kMinCcMinor = 5;

int ComputeCapRank(const std::string &cc) {
  const auto parts = util::Split(util::TrimCopy(cc), '.');
  if (parts.size() < 2)
    return -1;
  const int maj = std::atoi(parts[0].c_str());
  const int min = std::atoi(parts[1].c_str());
  return maj * 10 + min; // "7.5" -> 75, "8.6" -> 86
}

std::string KernelString() {
  utsname u{};
  if (uname(&u) != 0)
    return "unknown";
  std::ostringstream oss;
  oss << u.sysname << " " << u.release << " (" << u.machine << ")";
  return oss.str();
}

std::optional<Version> ParseVersionLike(const std::string &s) {
  std::string token;
  for (size_t i = 0; i < s.size(); ++i) {
    if (!std::isdigit(static_cast<unsigned char>(s[i])))
      continue;
    size_t j = i;
    while (j < s.size()) {
      const char c = s[j];
      if (std::isdigit(static_cast<unsigned char>(c)) || c == '.') {
        ++j;
      } else {
        break;
      }
    }
    token = s.substr(i, j - i);
    break;
  }
  if (token.empty())
    return std::nullopt;

  const auto parts = util::Split(token, '.');
  if (parts.size() < 2)
    return std::nullopt;

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

bool MeetsRequiredDriver(const Version &v) {
  if (v.major != kRequiredDriverMajor)
    return v.major > kRequiredDriverMajor;
  return v.minor >= kRequiredDriverMinor;
}

std::optional<Version> DetectDriverVersion() {
  if (auto content = util::ReadTextFile("/proc/driver/nvidia/version")) {
    if (auto v = ParseVersionLike(*content))
      return v;
  }

  auto out = util::ExecCapture("nvidia-smi --query-gpu=driver_version "
                               "--format=csv,noheader 2>/dev/null");
  if (out.exit_code == 0) {
    const std::string line = util::FirstNonEmptyLine(out.stdout_str);
    if (!line.empty()) {
      if (auto v = ParseVersionLike(line))
        return v;
    }
  }
  return std::nullopt;
}

std::optional<std::pair<int, int>> ParseComputeCap(const std::string &s) {
  const auto t = util::TrimCopy(s);
  const auto parts = util::Split(t, '.');
  if (parts.size() < 2)
    return std::nullopt;

  const int major = std::atoi(parts[0].c_str());
  const int minor = std::atoi(parts[1].c_str());
  return std::make_pair(major, minor);
}

bool LikelyMeetsTensorCoreRequirement(const std::string &compute_cap_str) {
  auto cc = ParseComputeCap(compute_cap_str);
  if (!cc)
    return false;

  const auto [maj, min] = *cc;
  if (maj > kMinCcMajor)
    return true;
  if (maj < kMinCcMajor)
    return false;
  return min >= kMinCcMinor;
}

std::string JoinComma(const std::vector<std::string> &parts, size_t begin,
                      size_t end_exclusive) {
  std::string out;
  for (size_t i = begin; i < end_exclusive; ++i) {
    if (!out.empty())
      out += ",";
    out += parts[i];
  }
  return util::TrimCopy(out);
}

std::vector<std::string> SplitTrimComma(const std::string &line) {
  auto parts = util::Split(line, ',');
  for (auto &p : parts)
    p = util::TrimCopy(p);
  return parts;
}

std::vector<GpuInfo> DetectGpus() {
  std::vector<GpuInfo> gpus;

  // Best case: index + uuid + name + compute_cap
  auto out =
      util::ExecCapture("nvidia-smi --query-gpu=index,uuid,name,compute_cap "
                        "--format=csv,noheader 2>/dev/null");

  if (out.exit_code == 0) {
    for (const auto &raw : util::SplitLines(out.stdout_str)) {
      const auto line = util::TrimCopy(raw);
      if (line.empty())
        continue;

      auto fields = SplitTrimComma(line);
      if (fields.size() < 3)
        continue;

      // We expect 4 fields. If name contains commas, we may get >4.
      // Layout: idx, uuid, name...(maybe commas), compute_cap(last)
      GpuInfo g{};
      g.index = std::atoi(fields[0].c_str());
      g.uuid = (fields.size() >= 2) ? fields[1] : "";

      if (fields.size() >= 4) {
        g.compute_cap = util::TrimCopy(fields.back());
        g.name = JoinComma(fields, 2, fields.size() - 1);
      } else {
        g.name = JoinComma(fields, 2, fields.size());
        g.compute_cap = std::nullopt;
      }

      if (g.compute_cap && !g.compute_cap->empty()) {
        g.likely_supported = LikelyMeetsTensorCoreRequirement(*g.compute_cap);
        g.maxine_gpu_arg = maxine::MaxineGpuArgFromComputeCap(*g.compute_cap);
      }
      gpus.push_back(g);
    }

    if (!gpus.empty())
      return gpus;
  }

  // Fallback: index + uuid + name
  out = util::ExecCapture("nvidia-smi --query-gpu=index,uuid,name "
                          "--format=csv,noheader 2>/dev/null");
  if (out.exit_code == 0) {
    for (const auto &raw : util::SplitLines(out.stdout_str)) {
      const auto line = util::TrimCopy(raw);
      if (line.empty())
        continue;

      auto fields = SplitTrimComma(line);
      if (fields.size() < 3)
        continue;

      GpuInfo g{};
      g.index = std::atoi(fields[0].c_str());
      g.uuid = fields[1];
      g.name = JoinComma(fields, 2, fields.size());
      g.compute_cap = std::nullopt;
      g.likely_supported = false;
      g.maxine_gpu_arg = std::nullopt;

      gpus.push_back(g);
    }
  }

  return gpus;
}

fs::path GetEnvPathAny(std::initializer_list<const char *> names) {
  for (const char *n : names) {
    const char *v = std::getenv(n);
    if (v && *v)
      return fs::path(v);
  }
  return {};
}

struct RootSelection {
  fs::path root;
  std::string chosen_from; // "env", "xdg", "system", "default"
  fs::path xdg_candidate;
  fs::path system_candidate;
  std::string env_hint;
};

RootSelection ResolveRoot(const fs::path &env, const std::string &envHint,
                          const fs::path &xdgCandidate,
                          const fs::path &systemCandidate) {
  RootSelection s;
  s.env_hint = envHint;
  s.xdg_candidate = xdgCandidate;
  s.system_candidate = systemCandidate;

  if (!env.empty()) {
    s.root = env;
    s.chosen_from = "env";
    return s;
  }
  if (!xdgCandidate.empty() && fs::exists(xdgCandidate)) {
    s.root = xdgCandidate;
    s.chosen_from = "xdg";
    return s;
  }
  if (!systemCandidate.empty() && fs::exists(systemCandidate)) {
    s.root = systemCandidate;
    s.chosen_from = "system";
    return s;
  }

  s.root = xdgCandidate;
  s.chosen_from = "default";
  return s;
}

std::string RootExplain(const RootSelection &s) {
  std::ostringstream oss;
  if (s.chosen_from == "default") {
    oss << " (default user-local path)";
  } else if (s.chosen_from == "xdg") {
    oss << " (found user-local install)";
  } else if (s.chosen_from == "system") {
    oss << " (found system install)";
  } else if (s.chosen_from == "env") {
    oss << " (from env override)";
  }
  if (!s.system_candidate.empty() && s.system_candidate != s.root) {
    oss << " (also checked " << s.system_candidate.string() << ")";
  }
  return oss.str();
}

const GpuInfo *FindGpuByIndex(const std::vector<GpuInfo> &gpus, int index) {
  for (const auto &g : gpus) {
    if (g.index == index)
      return &g;
  }
  return nullptr;
}

const GpuInfo *FindGpuByUuid(const std::vector<GpuInfo> &gpus,
                             const std::string &uuid) {
  for (const auto &g : gpus) {
    if (!g.uuid.empty() && g.uuid == uuid)
      return &g;
  }
  return nullptr;
}

const GpuInfo *PickBestSupportedGpu(const std::vector<GpuInfo> &gpus) {
  const GpuInfo *best = nullptr;
  int bestRank = -1;

  for (const auto &g : gpus) {
    if (!g.likely_supported)
      continue;
    if (!g.compute_cap)
      continue;

    const int r = ComputeCapRank(*g.compute_cap);
    if (r > bestRank) {
      bestRank = r;
      best = &g;
    }
  }
  return best;
}

CheckResult CheckDriver(const std::optional<Version> &v) {
  CheckResult r;
  r.name = "NVIDIA driver >= 570.26 (Maxine requirement)";

  if (!v) {
    r.ok = false;
    r.details = "Driver version not detected (missing driver, kernel module "
                "not loaded, or nvidia-smi not available).";
    return r;
  }

  r.ok = MeetsRequiredDriver(*v);
  std::ostringstream oss;
  oss << "Detected: " << v->original << " | Required: " << kRequiredDriverMajor
      << "." << kRequiredDriverMinor;
  r.details = oss.str();
  return r;
}

CheckResult CheckMaxineGpuSupport(const std::vector<GpuInfo> &gpus) {
  CheckResult r;
  r.name = "At least one GPU supports Maxine (Tensor Cores required)";

  if (gpus.empty()) {
    r.ok = false;
    r.details = "No NVIDIA GPUs detected via nvidia-smi.";
    return r;
  }

  bool anyCap = false;
  bool anySupported = false;

  std::ostringstream oss;
  oss << "Detected:";
  for (const auto &g : gpus) {
    oss << " [" << g.index << "] " << g.name;
    if (!g.uuid.empty())
      oss << " (" << g.uuid << ")";
    if (g.compute_cap) {
      anyCap = true;
      oss << " cc " << *g.compute_cap;
      oss << (g.likely_supported ? " supported" : " unsupported");
    } else {
      oss << " cc ?";
    }
    oss << ";";
    if (g.likely_supported)
      anySupported = true;
  }

  if (!anyCap) {
    r.skipped = true;
    r.details = "Could not query compute capability. Try: nvidia-smi "
                "--query-gpu=index,uuid,name,compute_cap --format=csv";
    return r;
  }

  r.ok = anySupported;
  r.details = oss.str();
  return r;
}

CheckResult CheckGpuSelectionPolicy(const config::Settings &settings,
                                    Report *rep) {
  CheckResult r;
  r.name = "GPU selection policy resolves to a supported GPU";

  rep->gpu_selection_mode = config::ToString(settings.gpu.mode);
  rep->selected_gpu_index.reset();
  rep->selected_gpu_uuid.clear();

  if (rep->gpus.empty()) {
    r.ok = false;
    r.details = "No GPUs available to select.";
    return r;
  }

  const GpuInfo *selected = nullptr;
  std::string reason;

  if (settings.gpu.mode == config::GpuSelectMode::Uuid) {
    if (settings.gpu.uuid.empty()) {
      r.ok = false;
      r.details = "gpu.mode=uuid but gpu.uuid is empty.";
      return r;
    }
    selected = FindGpuByUuid(rep->gpus, settings.gpu.uuid);
    reason = "mode=uuid";
    if (!selected) {
      r.ok = false;
      r.details = "Selected UUID not found: " + settings.gpu.uuid;
      return r;
    }
  } else if (settings.gpu.mode == config::GpuSelectMode::Index) {
    if (!settings.gpu.index) {
      r.ok = false;
      r.details = "gpu.mode=index but gpu.index is missing.";
      return r;
    }
    selected = FindGpuByIndex(rep->gpus, *settings.gpu.index);
    reason = "mode=index";
    if (!selected) {
      r.ok = false;
      r.details =
          "Selected index not found: " + std::to_string(*settings.gpu.index);
      return r;
    }
  } else {
    // auto
    selected = PickBestSupportedGpu(rep->gpus);
    reason = "mode=auto";
    if (!selected) {
      // Fallback to first GPU when none supported, but fail the check.
      selected = &rep->gpus.front();
      rep->selected_gpu_index = selected->index;
      rep->selected_gpu_uuid = selected->uuid;
      r.ok = false;

      std::ostringstream oss;
      oss << "Auto mode found no supported GPUs. Selected fallback: ["
          << selected->index << "] " << selected->name;
      if (selected->compute_cap)
        oss << " cc " << *selected->compute_cap;
      r.details = oss.str();
      return r;
    }
  }

  rep->selected_gpu_index = selected->index;
  rep->selected_gpu_uuid = selected->uuid;

  r.ok = selected->likely_supported;
  std::ostringstream oss;
  oss << reason << " -> [" << selected->index << "] " << selected->name;
  if (!selected->uuid.empty())
    oss << " (" << selected->uuid << ")";
  if (selected->compute_cap)
    oss << " cc " << *selected->compute_cap;
  if (selected->maxine_gpu_arg)
    oss << " maxine_gpu_arg=" << *selected->maxine_gpu_arg;

  if (!r.ok) {
    oss << " (selected GPU appears unsupported for Maxine)";
  }
  r.details = oss.str();
  return r;
}

CheckResult
CheckSuggestedGpuArgsForFeatureInstalls(const std::vector<GpuInfo> &gpus) {
  CheckResult r;
  r.name = "Suggested --gpu values for VFX/AR feature install scripts";

  std::set<std::string> args;
  for (const auto &g : gpus) {
    if (!g.likely_supported)
      continue;
    if (g.maxine_gpu_arg)
      args.insert(*g.maxine_gpu_arg);
  }

  if (args.empty()) {
    r.skipped = true;
    r.details = "No supported GPUs with known Maxine installer mapping.";
    return r;
  }

  r.ok = true;
  std::ostringstream oss;
  oss << "Install for:";
  bool first = true;
  for (const auto &a : args) {
    oss << (first ? " " : ", ") << a;
    first = false;
  }
  r.details = oss.str();
  return r;
}

CheckResult CheckSdkCorePresent(const std::string &label, const fs::path &root,
                                const char *hintEnv) {
  CheckResult r;
  r.name = label;

  if (root.empty()) {
    r.skipped = true;
    r.details = std::string("Root not set. Set ") + hintEnv +
                " to your extracted SDK core folder.";
    return r;
  }

  if (!fs::exists(root)) {
    r.ok = false;
    r.details =
        "Not found: " + root.string() + " (override with " + hintEnv + ")";
    return r;
  }

  r.ok = true;
  r.details = "Found: " + root.string();
  return r;
}

CheckResult CheckInstallScript(const std::string &label, const fs::path &root,
                               const fs::path &relScript, const char *hintEnv) {
  CheckResult r;
  r.name = label;

  if (root.empty()) {
    r.skipped = true;
    r.details =
        std::string("Skipped (root not set). Set ") + hintEnv + " first.";
    return r;
  }

  if (!fs::exists(root)) {
    r.skipped = true;
    r.details = "Skipped (SDK core not present at " + root.string() + ").";
    return r;
  }

  const fs::path script = root / relScript;
  if (fs::exists(script)) {
    r.ok = true;
    r.details = "Found: " + script.string();
  } else {
    r.ok = false;
    r.details = "Missing: " + script.string();
  }

  return r;
}

std::string JsonEscape(const std::string &s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
    case '\\':
      out += "\\\\";
      break;
    case '"':
      out += "\\\"";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
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

const char *StatusLabel(const CheckResult &c) {
  if (c.skipped)
    return "SKIP";
  return c.ok ? "OK" : "FAIL";
}

std::string StatusJson(const CheckResult &c) {
  if (c.skipped)
    return "skip";
  return c.ok ? "ok" : "fail";
}
} // namespace

bool Report::AllChecksPassed() const {
  for (const auto &c : checks) {
    if (!c.ok && !c.skipped)
      return false;
  }
  return true;
}

std::string Report::ToText() const {
  std::ostringstream oss;
  oss << "StudioCast Probe\n";
  oss << "Version: " << app_version << " (" << app_git_sha << ")\n\n";

  oss << "System\n";
  oss << "  OS: " << (os_pretty_name.empty() ? "unknown" : os_pretty_name)
      << "\n";
  oss << "  Kernel: " << (kernel.empty() ? "unknown" : kernel) << "\n";

  if (nvidia_driver) {
    oss << "  NVIDIA Driver: " << nvidia_driver->original << "\n";
  } else {
    oss << "  NVIDIA Driver: not detected\n";
  }

  if (!gpus.empty()) {
    oss << "  GPU(s):\n";
    for (const auto &g : gpus) {
      oss << "    - [" << g.index << "] " << g.name;
      if (!g.uuid.empty())
        oss << " (" << g.uuid << ")";
      if (g.compute_cap) {
        oss << " (compute_cap " << *g.compute_cap << ")";
        oss << (g.likely_supported ? " [supported]" : " [unsupported]");
      } else {
        oss << " (compute_cap unknown)";
      }
      if (g.maxine_gpu_arg) {
        oss << " (maxine --gpu " << *g.maxine_gpu_arg << ")";
      }
      oss << "\n";
    }
  } else {
    oss << "  GPU(s): none detected via nvidia-smi\n";
  }

  if (selected_gpu_index) {
    oss << "  Selected GPU (policy="
        << (gpu_selection_mode.empty() ? "unknown" : gpu_selection_mode)
        << "): [" << *selected_gpu_index << "]";
    if (!selected_gpu_uuid.empty())
      oss << " (" << selected_gpu_uuid << ")";
    oss << "\n";
  }

  oss << "\nChecks\n";
  for (const auto &c : checks) {
    oss << "  [" << StatusLabel(c) << "] " << c.name << "\n";
    if (!c.details.empty())
      oss << "       " << c.details << "\n";
  }

  if (!notes.empty()) {
    oss << "\nNotes\n";
    for (const auto &n : notes)
      oss << "  - " << n << "\n";
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
    oss << "\"nvidia_driver\":\"" << JsonEscape(nvidia_driver->original)
        << "\",";
  } else {
    oss << "\"nvidia_driver\":null,";
  }

  oss << "\"gpu_selection_mode\":\"" << JsonEscape(gpu_selection_mode) << "\",";
  if (selected_gpu_index) {
    oss << "\"selected_gpu_index\":" << *selected_gpu_index << ",";
  } else {
    oss << "\"selected_gpu_index\":null,";
  }
  if (!selected_gpu_uuid.empty()) {
    oss << "\"selected_gpu_uuid\":\"" << JsonEscape(selected_gpu_uuid) << "\",";
  } else {
    oss << "\"selected_gpu_uuid\":null,";
  }

  oss << "\"gpus\":[";
  for (size_t i = 0; i < gpus.size(); ++i) {
    if (i)
      oss << ",";
    oss << "{";
    oss << "\"index\":" << gpus[i].index << ",";
    if (!gpus[i].uuid.empty()) {
      oss << "\"uuid\":\"" << JsonEscape(gpus[i].uuid) << "\",";
    } else {
      oss << "\"uuid\":null,";
    }
    oss << "\"name\":\"" << JsonEscape(gpus[i].name) << "\",";
    if (gpus[i].compute_cap) {
      oss << "\"compute_cap\":\"" << JsonEscape(*gpus[i].compute_cap) << "\",";
    } else {
      oss << "\"compute_cap\":null,";
    }
    oss << "\"likely_supported\":"
        << (gpus[i].likely_supported ? "true" : "false") << ",";
    if (gpus[i].maxine_gpu_arg) {
      oss << "\"maxine_gpu_arg\":\"" << JsonEscape(*gpus[i].maxine_gpu_arg)
          << "\"";
    } else {
      oss << "\"maxine_gpu_arg\":null";
    }
    oss << "}";
  }
  oss << "],";

  oss << "\"checks\":[";
  for (size_t i = 0; i < checks.size(); ++i) {
    if (i)
      oss << ",";
    oss << "{"
        << "\"name\":\"" << JsonEscape(checks[i].name) << "\","
        << "\"status\":\"" << StatusJson(checks[i]) << "\","
        << "\"details\":\"" << JsonEscape(checks[i].details) << "\""
        << "}";
  }
  oss << "],";

  oss << "\"notes\":[";
  for (size_t i = 0; i < notes.size(); ++i) {
    if (i)
      oss << ",";
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

  const auto settings = config::LoadSettings();

  // Canonical user-local install root (XDG default)
  const fs::path vfxXdg = util::DefaultVfxRoot();
  const fs::path arXdg = util::DefaultArRoot();
  const fs::path afxXdg = util::DefaultAfxRoot();

  // Optional system-wide installs
  const fs::path vfxSys = "/usr/local/VideoFX";
  const fs::path arSys = "/usr/local/ARSDK";
  const fs::path afxSys = "/usr/local/Audio_Effects_SDK";

  // Env overrides
  const fs::path vfxEnv = GetEnvPathAny({"STUDIOCAST_VFX_SDK_ROOT"});
  const fs::path arEnv = GetEnvPathAny({"STUDIOCAST_AR_SDK_ROOT"});
  const fs::path afxEnv =
      GetEnvPathAny({"AFX_SDK_ROOT", "STUDIOCAST_AFX_SDK_ROOT"});

  const auto vfxRoot =
      ResolveRoot(vfxEnv, "STUDIOCAST_VFX_SDK_ROOT", vfxXdg, vfxSys);
  const auto arRoot =
      ResolveRoot(arEnv, "STUDIOCAST_AR_SDK_ROOT", arXdg, arSys);
  const auto afxRoot = ResolveRoot(afxEnv, "AFX_SDK_ROOT", afxXdg, afxSys);

  // Checks
  rep.checks.push_back(CheckDriver(rep.nvidia_driver));
  rep.checks.push_back(CheckMaxineGpuSupport(rep.gpus));

  // Selection check populates rep.gpu_selection_mode + selected_gpu_*
  rep.checks.push_back(CheckGpuSelectionPolicy(settings, &rep));

  rep.checks.push_back(CheckSuggestedGpuArgsForFeatureInstalls(rep.gpus));

  // VFX
  {
    auto core = CheckSdkCorePresent("Maxine VFX SDK core present", vfxRoot.root,
                                    vfxRoot.env_hint.c_str());
    if (!core.ok && !core.skipped)
      core.details += RootExplain(vfxRoot);
    rep.checks.push_back(core);

    rep.checks.push_back(CheckInstallScript(
        "VFX features install script present (features/install_feature.sh)",
        vfxRoot.root, fs::path("features") / "install_feature.sh",
        vfxRoot.env_hint.c_str()));
  }

  // AR
  {
    auto core = CheckSdkCorePresent("Maxine AR SDK core present", arRoot.root,
                                    arRoot.env_hint.c_str());
    if (!core.ok && !core.skipped)
      core.details += RootExplain(arRoot);
    rep.checks.push_back(core);

    rep.checks.push_back(CheckInstallScript(
        "AR features install script present (features/install_feature.sh)",
        arRoot.root, fs::path("features") / "install_feature.sh",
        arRoot.env_hint.c_str()));
  }

  // AFX
  {
    auto core = CheckSdkCorePresent("Maxine AFX SDK core present", afxRoot.root,
                                    afxRoot.env_hint.c_str());
    if (!core.ok && !core.skipped)
      core.details += RootExplain(afxRoot);
    rep.checks.push_back(core);

    rep.checks.push_back(CheckInstallScript(
        "AFX feature download script present (features/download_features.sh)",
        afxRoot.root, fs::path("features") / "download_features.sh",
        afxRoot.env_hint.c_str()));
  }

  // Notes
  {
    const auto base = util::StudioCastMaxineDir();
    if (!base.empty()) {
      rep.notes.push_back("Default user-local Maxine base dir: " +
                          base.string());
    }
  }
  rep.notes.push_back("GPU policy file: " + config::SettingsPath().string());
  rep.notes.push_back("Override GPU policy via env: STUDIOCAST_GPU_MODE, "
                      "STUDIOCAST_GPU_UUID, STUDIOCAST_GPU_INDEX.");
  rep.notes.push_back("Overrides for SDK roots: STUDIOCAST_VFX_SDK_ROOT, "
                      "STUDIOCAST_AR_SDK_ROOT, AFX_SDK_ROOT.");
  rep.notes.push_back("VFX/AR features are installed via "
                      "features/install_feature.sh (NGC_CLI_API_KEY).");
  rep.notes.push_back("AFX features are downloaded via "
                      "features/download_features.sh (NGC_API_KEY).");

  return rep;
}
} // namespace studiocast::probe
