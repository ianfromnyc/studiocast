#include "probe.h"

#include <sys/utsname.h>

#include <cctype>
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

        // Heuristic: Maxine Linux docs require Tensor Core GPUs (Turing+).
        // Compute capability heuristic: treat >= 7.5 as "likely supported".
        // (Note: some Turing GTX parts lack Tensor Cores; this is best-effort.)
        constexpr int kMinCcMajor = 7;
        constexpr int kMinCcMinor = 5;

        std::string KernelString() {
            utsname u{};
            if (uname(&u) != 0) return "unknown";
            std::ostringstream oss;
            oss << u.sysname << " " << u.release << " (" << u.machine << ")";
            return oss.str();
        }

        std::optional<Version> ParseVersionLike(const std::string &s) {
            std::string token;
            for (size_t i = 0; i < s.size(); ++i) {
                if (!std::isdigit(static_cast<unsigned char>(s[i]))) continue;
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
            if (token.empty()) return std::nullopt;

            const auto parts = util::Split(token, '.');
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

        bool MeetsRequiredDriver(const Version &v) {
            if (v.major != kRequiredDriverMajor) return v.major > kRequiredDriverMajor;
            return v.minor >= kRequiredDriverMinor;
        }

        std::optional<Version> DetectDriverVersion() {
            // 1) Direct when kernel module is loaded
            if (auto content = util::ReadTextFile("/proc/driver/nvidia/version")) {
                if (auto v = ParseVersionLike(*content)) return v;
            }

            // 2) Fallback to nvidia-smi
            auto out = util::ExecCapture(
                "nvidia-smi --query-gpu=driver_version --format=csv,noheader 2>/dev/null");
            if (out.exit_code == 0) {
                const std::string line = util::FirstNonEmptyLine(out.stdout_str);
                if (!line.empty()) {
                    if (auto v = ParseVersionLike(line)) return v;
                }
            }

            return std::nullopt;
        }

        std::optional<std::pair<int, int> > ParseComputeCap(const std::string &s) {
            const auto t = util::TrimCopy(s);
            const auto parts = util::Split(t, '.');
            if (parts.size() < 2) return std::nullopt;

            const int major = std::atoi(parts[0].c_str());
            const int minor = std::atoi(parts[1].c_str());
            return std::make_pair(major, minor);
        }

        bool LikelyMeetsTensorCoreRequirement(const std::string &compute_cap_str) {
            auto cc = ParseComputeCap(compute_cap_str);
            if (!cc) return false;

            const auto [maj, min] = *cc;
            if (maj > kMinCcMajor) return true;
            if (maj < kMinCcMajor) return false;
            return min >= kMinCcMinor;
        }

        std::vector<GpuInfo> DetectGpus() {
            // Prefer querying compute capability too.
            // nvidia-smi supports: --query-gpu=name,compute_cap (newer drivers) :contentReference[oaicite:1]{index=1}
            auto out = util::ExecCapture(
                "nvidia-smi --query-gpu=name,compute_cap --format=csv,noheader 2>/dev/null");

            std::vector<GpuInfo> gpus;

            if (out.exit_code == 0) {
                for (const auto &lineRaw: util::SplitLines(out.stdout_str)) {
                    const auto line = util::TrimCopy(lineRaw);
                    if (line.empty()) continue;

                    // Split on last comma: "<name>, <compute_cap>"
                    const auto pos = line.rfind(',');
                    if (pos == std::string::npos) {
                        gpus.push_back(GpuInfo{line, std::nullopt});
                        continue;
                    }

                    auto name = util::TrimCopy(line.substr(0, pos));
                    auto cap = util::TrimCopy(line.substr(pos + 1));
                    if (name.empty()) name = line;

                    if (!cap.empty()) {
                        gpus.push_back(GpuInfo{name, cap});
                    } else {
                        gpus.push_back(GpuInfo{name, std::nullopt});
                    }
                }

                if (!gpus.empty()) return gpus;
            }

            // Fallback: names only
            out = util::ExecCapture("nvidia-smi --query-gpu=name --format=csv,noheader 2>/dev/null");
            if (out.exit_code == 0) {
                for (const auto &lineRaw: util::SplitLines(out.stdout_str)) {
                    const auto name = util::TrimCopy(lineRaw);
                    if (!name.empty()) gpus.push_back(GpuInfo{name, std::nullopt});
                }
            }

            return gpus;
        }

        fs::path GetEnvPathAny(std::initializer_list<const char *> names) {
            for (const char *n: names) {
                const char *v = std::getenv(n);
                if (v && *v) return fs::path(v);
            }
            return {};
        }

        CheckResult CheckDriver(const std::optional<Version> &v) {
            CheckResult r;
            r.name = "NVIDIA driver >= 570.26 (Maxine requirement)";

            if (!v) {
                r.ok = false;
                r.details =
                        "Driver version not detected (missing driver, kernel module not loaded, or nvidia-smi not available).";
                return r;
            }

            r.ok = MeetsRequiredDriver(*v);
            std::ostringstream oss;
            oss << "Detected: " << v->original << " | Required: " << kRequiredDriverMajor << "." <<
                    kRequiredDriverMinor;
            r.details = oss.str();
            return r;
        }

        CheckResult CheckMaxineGpuSupport(const std::vector<GpuInfo> &gpus) {
            CheckResult r;
            r.name = "GPU supports Maxine (Tensor Cores required)";

            if (gpus.empty()) {
                r.ok = false;
                r.details = "No NVIDIA GPU detected via nvidia-smi.";
                return r;
            }

            bool anyHasCap = false;
            bool anyLikelyOk = false;

            std::ostringstream detected;
            for (size_t i = 0; i < gpus.size(); ++i) {
                if (i) detected << "; ";
                detected << gpus[i].name;
                if (gpus[i].compute_cap) {
                    anyHasCap = true;
                    detected << " (compute_cap " << *gpus[i].compute_cap << ")";
                    if (LikelyMeetsTensorCoreRequirement(*gpus[i].compute_cap)) anyLikelyOk = true;
                }
            }

            if (!anyHasCap) {
                r.skipped = true;
                r.details =
                        "Could not query compute capability. Try: nvidia-smi --query-gpu=name,compute_cap --format=csv";
                return r;
            }

            r.ok = anyLikelyOk;
            if (r.ok) {
                r.details = "Detected: " + detected.str();
            } else {
                r.details =
                        "Detected: " + detected.str() +
                        " | Maxine Linux SDKs require Tensor Core GPUs (Turing/Ampere/Ada/Hopper/Blackwell).";
            }
            return r;
        }

        CheckResult CheckSdkCorePresent(const std::string &label, const fs::path &root, const char *hintEnv) {
            CheckResult r;
            r.name = label;

            if (root.empty()) {
                r.skipped = true;
                r.details = std::string("Root not set. Set ") + hintEnv + " to your extracted SDK core folder.";
                return r;
            }

            if (!fs::exists(root)) {
                r.ok = false;
                r.details = "Not found: " + root.string() + " (override with " + hintEnv + ")";
                return r;
            }

            r.ok = true;
            r.details = "Found: " + root.string();
            return r;
        }

        CheckResult CheckInstallScript(const std::string &label,
                                       const fs::path &root,
                                       const fs::path &relScript,
                                       const char *hintEnv) {
            CheckResult r;
            r.name = label;

            if (root.empty()) {
                r.skipped = true;
                r.details = std::string("Skipped (root not set). Set ") + hintEnv + " first.";
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
            for (char c: s) {
                switch (c) {
                    case '\\': out += "\\\\";
                        break;
                    case '"': out += "\\\"";
                        break;
                    case '\n': out += "\\n";
                        break;
                    case '\r': out += "\\r";
                        break;
                    case '\t': out += "\\t";
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
            if (c.skipped) return "SKIP";
            return c.ok ? "OK" : "FAIL";
        }

        std::string StatusJson(const CheckResult &c) {
            if (c.skipped) return "skip";
            return c.ok ? "ok" : "fail";
        }
    } // namespace

    bool Report::AllChecksPassed() const {
        for (const auto &c: checks) {
            if (!c.ok && !c.skipped) return false;
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
            for (const auto &g: gpus) {
                if (g.compute_cap) {
                    oss << "    - " << g.name << " (compute_cap " << *g.compute_cap << ")\n";
                } else {
                    oss << "    - " << g.name << "\n";
                }
            }
        } else {
            oss << "  GPU(s): none detected via nvidia-smi\n";
        }

        oss << "\nChecks\n";
        for (const auto &c: checks) {
            oss << "  [" << StatusLabel(c) << "] " << c.name << "\n";
            if (!c.details.empty()) oss << "       " << c.details << "\n";
        }

        if (!notes.empty()) {
            oss << "\nNotes\n";
            for (const auto &n: notes) oss << "  - " << n << "\n";
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
            oss << "{"
                    << "\"name\":\"" << JsonEscape(gpus[i].name) << "\",";
            if (gpus[i].compute_cap) {
                oss << "\"compute_cap\":\"" << JsonEscape(*gpus[i].compute_cap) << "\"";
            } else {
                oss << "\"compute_cap\":null";
            }
            oss << "}";
        }
        oss << "],";

        oss << "\"checks\":[";
        for (size_t i = 0; i < checks.size(); ++i) {
            if (i) oss << ",";
            oss << "{"
                    << "\"name\":\"" << JsonEscape(checks[i].name) << "\","
                    << "\"status\":\"" << StatusJson(checks[i]) << "\","
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

        // Roots:
        // - VFX/AR docs commonly use /usr/local/... but for dev we allow overrides.
        fs::path vfxRoot = GetEnvPathAny({"STUDIOCAST_VFX_SDK_ROOT"});
        if (vfxRoot.empty()) vfxRoot = "/usr/local/VideoFX";

        fs::path arRoot = GetEnvPathAny({"STUDIOCAST_AR_SDK_ROOT"});
        if (arRoot.empty()) arRoot = "/usr/local/ARSDK";

        fs::path afxRoot = GetEnvPathAny({"AFX_SDK_ROOT", "STUDIOCAST_AFX_SDK_ROOT"});

        rep.checks.push_back(CheckDriver(rep.nvidia_driver));
        rep.checks.push_back(CheckMaxineGpuSupport(rep.gpus));

        rep.checks.push_back(CheckSdkCorePresent(
            "Maxine VFX SDK core present", vfxRoot, "STUDIOCAST_VFX_SDK_ROOT"));
        rep.checks.push_back(CheckInstallScript(
            "VFX features install script present (features/install_feature.sh)",
            vfxRoot, fs::path("features") / "install_feature.sh", "STUDIOCAST_VFX_SDK_ROOT"));

        rep.checks.push_back(CheckSdkCorePresent(
            "Maxine AR SDK core present", arRoot, "STUDIOCAST_AR_SDK_ROOT"));
        rep.checks.push_back(CheckInstallScript(
            "AR features install script present (features/install_feature.sh)",
            arRoot, fs::path("features") / "install_feature.sh", "STUDIOCAST_AR_SDK_ROOT"));

        rep.checks.push_back(CheckSdkCorePresent(
            "Maxine AFX SDK root set", afxRoot, "AFX_SDK_ROOT"));
        rep.checks.push_back(CheckInstallScript(
            "AFX features directory present (features/)",
            afxRoot, fs::path("features"), "AFX_SDK_ROOT"));

        rep.notes.push_back(
            "Maxine SDKs require feature/model packages (not included in core). Install via the included scripts (NGC API key required).");
        rep.notes.push_back(
            "Maxine Linux SDK docs describe server-side optimization; desktop use is not officially supported.");

        return rep;
    }
} // namespace studiocast::probe
