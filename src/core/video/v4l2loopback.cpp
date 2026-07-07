#include "v4l2loopback.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <set>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>

#include <unistd.h>

// NEW: v4l2 querycap fallback (more reliable than sysfs for v4l2loopback)
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>

#include "core/util/exec.h"
#include "core/util/fs.h"
#include "core/util/json.h"
#include "core/util/proc.h"
#include "core/util/strings.h"

namespace fs = std::filesystem;

namespace studiocast::video {
namespace {

bool StartsWith(const std::string &s, const std::string &prefix) {
  return s.rfind(prefix, 0) == 0;
}

std::string ToLowerAscii(std::string s) {
  for (char &c : s) {
    if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

std::string ReadTrimmedFile(const fs::path &p) {
  if (auto v = util::ReadTextFile(p.string()))
    return util::TrimCopy(*v);
  return {};
}

std::string ReadDriverNameSysfs(const fs::path &sysVideo) {
  // /sys/class/video4linux/videoX/device/driver -> symlink to driver directory
  // (best-effort)
  std::error_code ec;
  const fs::path link = sysVideo / "device" / "driver";
  if (!fs::exists(link, ec))
    return {};

  const fs::path target = fs::read_symlink(link, ec);
  if (ec)
    return {};
  return target.filename().string();
}

std::string ShellQuote(const std::string &s) {
  std::string out;
  out.reserve(s.size() + 2);
  out.push_back('\'');
  for (char c : s) {
    if (c == '\'') {
      out.append("'\\''");
    } else {
      out.push_back(c);
    }
  }
  out.push_back('\'');
  return out;
}

std::string BuildShellCommand(const std::vector<std::string> &argv) {
  std::ostringstream oss;
  bool first = true;
  for (const auto &a : argv) {
    if (!first)
      oss << ' ';
    first = false;
    oss << ShellQuote(a);
  }
  return oss.str();
}

struct CapabilityInfo {
  std::string driver;
  std::string card;
  std::string bus_info;
  std::uint32_t version = 0;
  std::uint32_t capabilities = 0;
  std::uint32_t device_caps = 0;
};

std::string Hex32(std::uint32_t v) {
  std::ostringstream oss;
  oss << "0x" << std::hex << std::setw(8) << std::setfill('0') << v;
  return oss.str();
}

std::string CapabilitySummary(const CapabilityInfo &info) {
  std::ostringstream oss;
  oss << "driver=" << (info.driver.empty() ? "(unknown)" : info.driver)
      << ", card=" << (info.card.empty() ? "(unknown)" : info.card)
      << ", bus=" << (info.bus_info.empty() ? "(unknown)" : info.bus_info)
      << ", version=" << ((info.version >> 16) & 0xff) << "."
      << ((info.version >> 8) & 0xff) << "." << (info.version & 0xff)
      << ", capabilities=" << Hex32(info.capabilities)
      << ", device_caps=" << Hex32(info.device_caps);
  return oss.str();
}

bool QueryV4l2Cap(const std::string &devNode, CapabilityInfo *out,
                  std::string *error) {
  int fd = ::open(devNode.c_str(), O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    // Try read-only if R/W open fails
    fd = ::open(devNode.c_str(), O_RDONLY | O_CLOEXEC);
  }
  if (fd < 0) {
    if (error)
      *error = "open(" + devNode + ") failed: " + std::strerror(errno);
    return false;
  }

  v4l2_capability cap{};
  int r = 0;
  do {
    r = ::ioctl(fd, VIDIOC_QUERYCAP, &cap);
  } while (r < 0 && errno == EINTR);

  ::close(fd);

  if (r < 0) {
    if (error)
      *error = "VIDIOC_QUERYCAP failed for " + devNode + ": " +
               std::strerror(errno);
    return false;
  }

  if (out) {
    out->driver = util::TrimCopy(
        std::string(reinterpret_cast<const char *>(cap.driver)));
    out->card =
        util::TrimCopy(std::string(reinterpret_cast<const char *>(cap.card)));
    out->bus_info = util::TrimCopy(
        std::string(reinterpret_cast<const char *>(cap.bus_info)));
    out->version = cap.version;
    out->capabilities = cap.capabilities;
    out->device_caps = cap.device_caps;
  }
  if (error)
    error->clear();

  return true;
}

bool CommandExists(const std::string &name) {
  const std::string cmd =
      std::string("command -v ") + ShellQuote(name) + " >/dev/null 2>&1";
  auto r = util::ExecCapture(cmd);
  return r.exit_code == 0;
}

bool IsModuleLoadedSysfs(const char *moduleName) {
  std::error_code ec;
  return fs::exists(fs::path("/sys/module") / moduleName, ec);
}

bool IsModuleInstalledModinfo(const char *moduleName) {
  auto r = util::ExecCapture(std::string("modinfo ") + moduleName +
                             " >/dev/null 2>&1");
  return r.exit_code == 0;
}

int ParseVideoNumber(const std::string &sys_name) {
  if (!StartsWith(sys_name, "video"))
    return -1;
  const std::string rest = sys_name.substr(5);
  if (rest.empty())
    return -1;

  char *end = nullptr;
  const long v = std::strtol(rest.c_str(), &end, 10);
  if (!end || end == rest.c_str())
    return -1;
  if (v < 0 || v > 4096)
    return -1;
  return static_cast<int>(v);
}

int SuggestVideoNr(const std::vector<VideoDevice> &devices) {
  std::set<int> used;
  for (const auto &d : devices) {
    const int n = ParseVideoNumber(d.sys_name);
    if (n >= 0)
      used.insert(n);
  }

  for (int n = 10; n < 256; ++n) {
    if (used.find(n) == used.end())
      return n;
  }
  return -1;
}

std::string BuildSuggestedModprobeCmd(int videoNr) {
  std::ostringstream oss;
  oss << "sudo modprobe v4l2loopback devices=1";
  if (videoNr >= 0)
    oss << " video_nr=" << videoNr;
  oss << " card_label=\"StudioCast Camera\" exclusive_caps=1";
  return oss.str();
}

std::string CommandTimeoutSeconds(std::chrono::milliseconds timeout) {
  if (timeout.count() <= 0)
    return {};
  if (timeout.count() % 1000 == 0)
    return std::to_string(timeout.count() / 1000) + "s";

  std::ostringstream oss;
  oss << std::fixed << std::setprecision(3)
      << (static_cast<double>(timeout.count()) / 1000.0) << "s";
  return oss.str();
}

LoopbackCommandOutput
RunV4l2CtlCommand(const LoopbackProbeOptions &options, bool available,
                  const std::vector<std::string> &args) {
  LoopbackCommandOutput out;
  out.available = available;
  if (!available) {
    out.error = options.v4l2_ctl_command + " not found on PATH";
    return out;
  }

  out.attempted = true;

  std::vector<std::string> argv;
  const std::string timeout = CommandTimeoutSeconds(options.command_timeout);
  if (!timeout.empty()) {
    argv.push_back("timeout");
    argv.push_back(timeout);
  }
  argv.push_back(options.v4l2_ctl_command);
  argv.insert(argv.end(), args.begin(), args.end());

  const auto r = util::ExecCapture(BuildShellCommand(argv) + " 2>&1");
  out.exit_code = r.exit_code;
  out.output = r.stdout_str;
  out.timed_out = (r.exit_code == 124 || r.exit_code == 137);
  if (out.timed_out) {
    out.error = options.v4l2_ctl_command + " timed out after " +
                std::to_string(options.command_timeout.count()) + "ms";
  } else if (r.exit_code != 0) {
    out.error = options.v4l2_ctl_command + " exited with code " +
                std::to_string(r.exit_code);
  }
  return out;
}

void CollectModuleParameters(const LoopbackProbeOptions &options,
                             LoopbackReport *rep) {
  if (!rep)
    return;

  rep->module_parameters_checked = true;
  std::error_code ec;
  const bool exists = fs::exists(options.module_parameters_path, ec);
  if (ec) {
    rep->module_parameters_error =
        "Failed to inspect " + options.module_parameters_path.string() + ": " +
        ec.message();
    return;
  }
  if (!exists)
    return;

  const bool isDir = fs::is_directory(options.module_parameters_path, ec);
  if (ec) {
    rep->module_parameters_error =
        "Failed to inspect " + options.module_parameters_path.string() + ": " +
        ec.message();
    return;
  }
  if (!isDir) {
    rep->module_parameters_error =
        options.module_parameters_path.string() + " is not a directory";
    return;
  }
  rep->module_parameters_available = true;

  std::vector<LoopbackModuleParameter> params;
  fs::directory_iterator it(options.module_parameters_path, ec);
  if (ec) {
    rep->module_parameters_error =
        "Failed to read " + options.module_parameters_path.string() + ": " +
        ec.message();
    return;
  }
  for (; it != fs::directory_iterator(); it.increment(ec)) {
    if (ec) {
      rep->module_parameters_error =
          "Failed to read " + options.module_parameters_path.string() + ": " +
          ec.message();
      break;
    }
    const auto name = it->path().filename().string();
    if (name.empty())
      continue;
    params.push_back({name, ReadTrimmedFile(it->path())});
  }

  std::sort(params.begin(), params.end(),
            [](const LoopbackModuleParameter &a,
               const LoopbackModuleParameter &b) { return a.name < b.name; });
  rep->module_parameters = std::move(params);
}

std::vector<LoopbackProcessInfo> CollectHolders(const std::string &dev,
                                                int excludePid,
                                                std::string *error) {
  if (error)
    error->clear();

  util::OpenFileScanOptions opt;
  opt.exclude_pid = excludePid;
  opt.stop_at_first = false;

  std::string scanErr;
  const auto pids = util::PidsWithOpenFile(dev, opt, &scanErr);
  if (error)
    *error = scanErr;

  std::vector<LoopbackProcessInfo> out;
  out.reserve(pids.size());
  for (const int pid : pids) {
    out.push_back({pid, util::ProcessNameFromPid(pid)});
  }
  return out;
}

std::string FormatHolders(const std::vector<LoopbackProcessInfo> &holders) {
  if (holders.empty())
    return "none";

  std::ostringstream oss;
  for (std::size_t i = 0; i < holders.size(); ++i) {
    if (i)
      oss << ", ";
    oss << holders[i].pid;
    if (!holders[i].name.empty())
      oss << "(" << holders[i].name << ")";
  }
  return oss.str();
}

void AppendIndentedBlock(std::ostringstream &oss, const std::string &prefix,
                         const std::string &text) {
  if (text.empty())
    return;
  std::istringstream in(text);
  std::string line;
  while (std::getline(in, line)) {
    oss << prefix << line << "\n";
  }
}

std::string JsonEscape(const std::string &s) {
  return util::json::EscapeString(s);
}

std::string CommandOutputToJson(const LoopbackCommandOutput &cmd) {
  std::ostringstream oss;
  oss << "{";
  oss << "\"attempted\":" << (cmd.attempted ? "true" : "false") << ",";
  oss << "\"available\":" << (cmd.available ? "true" : "false") << ",";
  oss << "\"timed_out\":" << (cmd.timed_out ? "true" : "false") << ",";
  oss << "\"exit_code\":" << cmd.exit_code << ",";
  oss << "\"error\":\"" << JsonEscape(cmd.error) << "\",";
  oss << "\"output\":\"" << JsonEscape(cmd.output) << "\"";
  oss << "}";
  return oss.str();
}

} // namespace

bool LoopbackReport::ReadyForVirtualCamera() const {
  for (const auto &d : devices) {
    if (d.is_loopback && d.can_write)
      return true;
  }
  return false;
}

std::string LoopbackReport::ToText() const {
  std::ostringstream oss;

  oss << "StudioCast Video Status\n\n";

  oss << "v4l2loopback\n";
  oss << "  modinfo available: " << (modinfo_available ? "yes" : "no") << "\n";
  oss << "  module installed:  " << (module_installed ? "yes" : "no/unknown")
      << "\n";
  oss << "  module loaded:     " << (module_loaded ? "yes" : "no") << "\n";
  if (v4l2_ctl_checked) {
    oss << "  v4l2-ctl available: " << (v4l2_ctl_available ? "yes" : "no")
        << "\n";
  }

  if (module_parameters_checked) {
    oss << "  module parameters:";
    if (!module_parameters_available) {
      oss << " unavailable";
      if (!module_parameters_error.empty())
        oss << " (" << module_parameters_error << ")";
      oss << "\n";
    } else if (module_parameters.empty()) {
      oss << " none\n";
    } else {
      oss << "\n";
      for (const auto &p : module_parameters) {
        oss << "    " << p.name << "=" << p.value << "\n";
      }
    }
  }

  if (!suggested_modprobe_cmd.empty()) {
    oss << "  suggested command: " << suggested_modprobe_cmd << "\n";
    oss << "  remove command:    sudo modprobe -r v4l2loopback\n";
  }

  oss << "\nVideo devices\n";
  if (!sys_video_class_present) {
    oss << "  /sys/class/video4linux not present.\n";
  } else if (devices.empty()) {
    oss << "  No devices found.\n";
  } else {
    for (const auto &d : devices) {
      oss << "  - " << d.dev_node;
      if (!d.name.empty())
        oss << " : " << d.name;
      if (!d.driver.empty())
        oss << " (driver " << d.driver << ")";
      if (d.is_loopback)
        oss << " [loopback]";
      oss << " [" << (d.can_read ? "r" : "-") << (d.can_write ? "w" : "-")
          << "]";
      oss << "\n";
      if (!d.current_caps_summary.empty())
        oss << "      current caps: " << d.current_caps_summary << "\n";
      if (!d.current_caps_error.empty())
        oss << "      current caps error: " << d.current_caps_error << "\n";
      if (d.holder_scan_attempted) {
        oss << "      holders: " << FormatHolders(d.holders) << "\n";
        if (!d.holder_scan_error.empty())
          oss << "      holder scan warning: " << d.holder_scan_error << "\n";
      }
      if (d.v4l2_ctl_driver.available || d.v4l2_ctl_driver.attempted ||
          !d.v4l2_ctl_driver.error.empty()) {
        oss << "      v4l2-ctl -D:";
        if (!d.v4l2_ctl_driver.error.empty())
          oss << " " << d.v4l2_ctl_driver.error;
        if (d.v4l2_ctl_driver.exit_code >= 0)
          oss << " (exit " << d.v4l2_ctl_driver.exit_code << ")";
        oss << "\n";
        AppendIndentedBlock(oss, "        ", d.v4l2_ctl_driver.output);
      }
      if (d.v4l2_ctl_formats_ext.available ||
          d.v4l2_ctl_formats_ext.attempted ||
          !d.v4l2_ctl_formats_ext.error.empty()) {
        oss << "      v4l2-ctl --list-formats-ext:";
        if (!d.v4l2_ctl_formats_ext.error.empty())
          oss << " " << d.v4l2_ctl_formats_ext.error;
        if (d.v4l2_ctl_formats_ext.exit_code >= 0)
          oss << " (exit " << d.v4l2_ctl_formats_ext.exit_code << ")";
        oss << "\n";
        AppendIndentedBlock(oss, "        ", d.v4l2_ctl_formats_ext.output);
      }
    }
  }

  oss << "\nReady for virtual camera: "
      << (ReadyForVirtualCamera() ? "YES" : "NO") << "\n";

  if (!notes.empty()) {
    oss << "\nNotes\n";
    for (const auto &n : notes)
      oss << "  - " << n << "\n";
  }

  return oss.str();
}

std::string LoopbackReport::ToJson() const {
  std::ostringstream oss;
  oss << "{";
  oss << "\"sys_video_class_present\":"
      << (sys_video_class_present ? "true" : "false") << ",";
  oss << "\"modinfo_available\":" << (modinfo_available ? "true" : "false")
      << ",";
  oss << "\"module_installed\":" << (module_installed ? "true" : "false")
      << ",";
  oss << "\"module_loaded\":" << (module_loaded ? "true" : "false") << ",";
  oss << "\"module_parameters_checked\":"
      << (module_parameters_checked ? "true" : "false") << ",";
  oss << "\"module_parameters_available\":"
      << (module_parameters_available ? "true" : "false") << ",";
  oss << "\"module_parameters_error\":\""
      << JsonEscape(module_parameters_error) << "\",";
  oss << "\"module_parameters\":{";
  for (std::size_t i = 0; i < module_parameters.size(); ++i) {
    if (i)
      oss << ",";
    oss << "\"" << JsonEscape(module_parameters[i].name) << "\":\""
        << JsonEscape(module_parameters[i].value) << "\"";
  }
  oss << "},";
  oss << "\"v4l2_ctl_checked\":" << (v4l2_ctl_checked ? "true" : "false")
      << ",";
  oss << "\"v4l2_ctl_available\":"
      << (v4l2_ctl_available ? "true" : "false") << ",";
  oss << "\"ready_for_virtual_camera\":"
      << (ReadyForVirtualCamera() ? "true" : "false") << ",";
  oss << "\"suggested_video_nr\":" << suggested_video_nr << ",";
  oss << "\"suggested_modprobe_cmd\":\""
      << JsonEscape(suggested_modprobe_cmd) << "\",";

  oss << "\"devices\":[";
  for (std::size_t i = 0; i < devices.size(); ++i) {
    if (i)
      oss << ",";
    const auto &d = devices[i];
    oss << "{";
    oss << "\"sys_name\":\"" << JsonEscape(d.sys_name) << "\",";
    oss << "\"dev_node\":\"" << JsonEscape(d.dev_node) << "\",";
    oss << "\"name\":\"" << JsonEscape(d.name) << "\",";
    oss << "\"driver\":\"" << JsonEscape(d.driver) << "\",";
    oss << "\"is_loopback\":" << (d.is_loopback ? "true" : "false") << ",";
    oss << "\"can_read\":" << (d.can_read ? "true" : "false") << ",";
    oss << "\"can_write\":" << (d.can_write ? "true" : "false") << ",";
    oss << "\"current_caps_summary\":\""
        << JsonEscape(d.current_caps_summary) << "\",";
    oss << "\"current_caps_error\":\"" << JsonEscape(d.current_caps_error)
        << "\",";
    oss << "\"holder_scan_attempted\":"
        << (d.holder_scan_attempted ? "true" : "false") << ",";
    oss << "\"holder_scan_error\":\"" << JsonEscape(d.holder_scan_error)
        << "\",";
    oss << "\"holders\":[";
    for (std::size_t j = 0; j < d.holders.size(); ++j) {
      if (j)
        oss << ",";
      oss << "{";
      oss << "\"pid\":" << d.holders[j].pid << ",";
      oss << "\"name\":\"" << JsonEscape(d.holders[j].name) << "\"";
      oss << "}";
    }
    oss << "],";
    oss << "\"v4l2_ctl\":{";
    oss << "\"driver\":" << CommandOutputToJson(d.v4l2_ctl_driver) << ",";
    oss << "\"formats_ext\":" << CommandOutputToJson(d.v4l2_ctl_formats_ext);
    oss << "}";
    oss << "}";
  }
  oss << "],";

  oss << "\"notes\":[";
  for (std::size_t i = 0; i < notes.size(); ++i) {
    if (i)
      oss << ",";
    oss << "\"" << JsonEscape(notes[i]) << "\"";
  }
  oss << "]";
  oss << "}";
  return oss.str();
}

LoopbackReport ProbeLoopback(const LoopbackProbeOptions &options) {
  LoopbackReport rep;

  const fs::path sysDir = options.sys_video_class_path;
  {
    std::error_code ec;
    rep.sys_video_class_present = fs::exists(sysDir, ec);
  }

  rep.modinfo_available = CommandExists("modinfo");
  rep.module_installed =
      rep.modinfo_available ? IsModuleInstalledModinfo("v4l2loopback") : false;
  rep.module_loaded = IsModuleLoadedSysfs("v4l2loopback");
  if (options.collect_module_parameters)
    CollectModuleParameters(options, &rep);

  if (options.collect_v4l2_ctl) {
    rep.v4l2_ctl_checked = true;
    rep.v4l2_ctl_available = CommandExists(options.v4l2_ctl_command);
  }

  if (rep.sys_video_class_present) {
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator(sysDir, ec)) {
      if (ec)
        break;

      const auto sys_name = entry.path().filename().string();
      if (!StartsWith(sys_name, "video"))
        continue;

      VideoDevice d;
      d.sys_name = sys_name;
      d.dev_node = (options.dev_root_path / sys_name).string();

      // Best-effort sysfs fields
      d.name = ReadTrimmedFile(entry.path() / "name");
      d.driver = ReadDriverNameSysfs(entry.path());

      // Query V4L2 caps for a more reliable driver name (v4l2loopback often
      // needs this)
      CapabilityInfo cap;
      std::string capErr;
      if (QueryV4l2Cap(d.dev_node, &cap, &capErr)) {
        d.current_caps_summary = CapabilitySummary(cap);
        if (d.name.empty())
          d.name = cap.card;
        if (d.driver.empty())
          d.driver = cap.driver;

        const auto dl = ToLowerAscii(cap.driver);
        const auto bl = ToLowerAscii(cap.bus_info);
        if (dl.find("loopback") != std::string::npos ||
            bl.find("loopback") != std::string::npos) {
          d.is_loopback = true;
        }
      } else if (options.collect_v4l2_ctl || options.collect_holders ||
                 options.collect_module_parameters) {
        d.current_caps_error = capErr;
      }

      // Permissions (simple)
      d.can_read = (::access(d.dev_node.c_str(), R_OK) == 0);
      d.can_write = (::access(d.dev_node.c_str(), W_OK) == 0);

      // Old heuristic fallback (keep it)
      if (!d.is_loopback) {
        if (d.driver == "v4l2loopback")
          d.is_loopback = true;
        if (!d.name.empty() &&
            ToLowerAscii(d.name).find("v4l2loopback") != std::string::npos)
          d.is_loopback = true;
      }

      if (options.collect_holders) {
        d.holder_scan_attempted = true;
        const int excludePid =
            options.holder_exclude_pid > 0
                ? options.holder_exclude_pid
                : static_cast<int>(::getpid());
        d.holders = CollectHolders(d.dev_node, excludePid, &d.holder_scan_error);
      }

      if (options.collect_v4l2_ctl) {
        d.v4l2_ctl_driver = RunV4l2CtlCommand(
            options, rep.v4l2_ctl_available, {"-D", "-d", d.dev_node});
        d.v4l2_ctl_formats_ext = RunV4l2CtlCommand(
            options, rep.v4l2_ctl_available,
            {"--list-formats-ext", "-d", d.dev_node});
      }

      rep.devices.push_back(d);
    }
  }

  rep.suggested_video_nr = SuggestVideoNr(rep.devices);
  rep.suggested_modprobe_cmd =
      BuildSuggestedModprobeCmd(rep.suggested_video_nr);

  rep.notes.push_back("OBS may cache device lists; if you add/remove "
                      "v4l2loopback devices, restart OBS.");
  rep.notes.push_back("If /dev/video* is not writable, ensure your user is in "
                      "the 'video' group and re-login.");
  rep.notes.push_back("StudioCast will not run modprobe for you; it only "
                      "prints the suggested command.");

  return rep;
}

LoopbackReport ProbeLoopback() {
  LoopbackProbeOptions options;
  return ProbeLoopback(options);
}

LoopbackReport ProbeLoopbackDiagnostics() {
  LoopbackProbeOptions options;
  options.collect_module_parameters = true;
  options.collect_holders = true;
  options.collect_v4l2_ctl = true;
  return ProbeLoopback(options);
}

} // namespace studiocast::video
