#include "v4l2loopback.h"

#include <cstdlib>
#include <filesystem>
#include <set>
#include <sstream>
#include <string>
#include <system_error>

#include <unistd.h>

// NEW: v4l2 querycap fallback (more reliable than sysfs for v4l2loopback)
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>

#include "core/util/exec.h"
#include "core/util/fs.h"
#include "core/util/strings.h"

namespace fs = std::filesystem;

namespace studiocast::video {
namespace {

bool StartsWith(const std::string& s, const std::string& prefix) {
  return s.rfind(prefix, 0) == 0;
}

std::string ToLowerAscii(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

std::string ReadTrimmedFile(const fs::path& p) {
  if (auto v = util::ReadTextFile(p.string())) return util::TrimCopy(*v);
  return {};
}

std::string ReadDriverNameSysfs(const fs::path& sysVideo) {
  // /sys/class/video4linux/videoX/device/driver -> symlink to driver directory (best-effort)
  std::error_code ec;
  const fs::path link = sysVideo / "device" / "driver";
  if (!fs::exists(link, ec)) return {};

  const fs::path target = fs::read_symlink(link, ec);
  if (ec) return {};
  return target.filename().string();
}

bool QueryV4l2Cap(const std::string& devNode,
                  std::string* outDriver,
                  std::string* outCard,
                  std::string* outBusInfo) {
  int fd = ::open(devNode.c_str(), O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    // Try read-only if R/W open fails
    fd = ::open(devNode.c_str(), O_RDONLY | O_CLOEXEC);
  }
  if (fd < 0) return false;

  v4l2_capability cap{};
  int r = 0;
  do {
    r = ::ioctl(fd, VIDIOC_QUERYCAP, &cap);
  } while (r < 0 && errno == EINTR);

  ::close(fd);

  if (r < 0) return false;

  if (outDriver) *outDriver = std::string(reinterpret_cast<const char*>(cap.driver));
  if (outCard) *outCard = std::string(reinterpret_cast<const char*>(cap.card));
  if (outBusInfo) *outBusInfo = std::string(reinterpret_cast<const char*>(cap.bus_info));

  return true;
}

bool CommandExists(const char* name) {
  const std::string cmd = std::string("command -v ") + name + " >/dev/null 2>&1";
  auto r = util::ExecCapture(cmd);
  return r.exit_code == 0;
}

bool IsModuleLoadedSysfs(const char* moduleName) {
  std::error_code ec;
  return fs::exists(fs::path("/sys/module") / moduleName, ec);
}

bool IsModuleInstalledModinfo(const char* moduleName) {
  auto r = util::ExecCapture(std::string("modinfo ") + moduleName + " >/dev/null 2>&1");
  return r.exit_code == 0;
}

int ParseVideoNumber(const std::string& sys_name) {
  if (!StartsWith(sys_name, "video")) return -1;
  const std::string rest = sys_name.substr(5);
  if (rest.empty()) return -1;

  char* end = nullptr;
  const long v = std::strtol(rest.c_str(), &end, 10);
  if (!end || end == rest.c_str()) return -1;
  if (v < 0 || v > 4096) return -1;
  return static_cast<int>(v);
}

int SuggestVideoNr(const std::vector<VideoDevice>& devices) {
  std::set<int> used;
  for (const auto& d : devices) {
    const int n = ParseVideoNumber(d.sys_name);
    if (n >= 0) used.insert(n);
  }

  for (int n = 10; n < 256; ++n) {
    if (used.find(n) == used.end()) return n;
  }
  return -1;
}

std::string BuildSuggestedModprobeCmd(int videoNr) {
  std::ostringstream oss;
  oss << "sudo modprobe v4l2loopback devices=1";
  if (videoNr >= 0) oss << " video_nr=" << videoNr;
  oss << " card_label=\"StudioCast Camera\" exclusive_caps=1";
  return oss.str();
}

}  // namespace

bool LoopbackReport::ReadyForVirtualCamera() const {
  for (const auto& d : devices) {
    if (d.is_loopback && d.can_write) return true;
  }
  return false;
}

std::string LoopbackReport::ToText() const {
  std::ostringstream oss;

  oss << "StudioCast Video Status\n\n";

  oss << "v4l2loopback\n";
  oss << "  modinfo available: " << (modinfo_available ? "yes" : "no") << "\n";
  oss << "  module installed:  " << (module_installed ? "yes" : "no/unknown") << "\n";
  oss << "  module loaded:     " << (module_loaded ? "yes" : "no") << "\n";

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
    for (const auto& d : devices) {
      oss << "  - " << d.dev_node;
      if (!d.name.empty()) oss << " : " << d.name;
      if (!d.driver.empty()) oss << " (driver " << d.driver << ")";
      if (d.is_loopback) oss << " [loopback]";
      oss << " [" << (d.can_read ? "r" : "-") << (d.can_write ? "w" : "-") << "]";
      oss << "\n";
    }
  }

  oss << "\nReady for virtual camera: " << (ReadyForVirtualCamera() ? "YES" : "NO") << "\n";

  if (!notes.empty()) {
    oss << "\nNotes\n";
    for (const auto& n : notes) oss << "  - " << n << "\n";
  }

  return oss.str();
}

LoopbackReport ProbeLoopback() {
  LoopbackReport rep;

  const fs::path sysDir = "/sys/class/video4linux";
  {
    std::error_code ec;
    rep.sys_video_class_present = fs::exists(sysDir, ec);
  }

  rep.modinfo_available = CommandExists("modinfo");
  rep.module_installed = rep.modinfo_available ? IsModuleInstalledModinfo("v4l2loopback") : false;
  rep.module_loaded = IsModuleLoadedSysfs("v4l2loopback");

  if (rep.sys_video_class_present) {
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(sysDir, ec)) {
      if (ec) break;

      const auto sys_name = entry.path().filename().string();
      if (!StartsWith(sys_name, "video")) continue;

      VideoDevice d;
      d.sys_name = sys_name;
      d.dev_node = std::string("/dev/") + sys_name;

      // Best-effort sysfs fields
      d.name = ReadTrimmedFile(entry.path() / "name");
      d.driver = ReadDriverNameSysfs(entry.path());

      // Query V4L2 caps for a more reliable driver name (v4l2loopback often needs this)
      std::string capDriver, capCard, capBus;
      if (QueryV4l2Cap(d.dev_node, &capDriver, &capCard, &capBus)) {
        if (d.name.empty()) d.name = util::TrimCopy(capCard);
        if (d.driver.empty()) d.driver = util::TrimCopy(capDriver);

        const auto dl = ToLowerAscii(capDriver);
        const auto bl = ToLowerAscii(capBus);
        if (dl.find("loopback") != std::string::npos || bl.find("loopback") != std::string::npos) {
          d.is_loopback = true;
        }
      }

      // Permissions (simple)
      d.can_read = (::access(d.dev_node.c_str(), R_OK) == 0);
      d.can_write = (::access(d.dev_node.c_str(), W_OK) == 0);

      // Old heuristic fallback (keep it)
      if (!d.is_loopback) {
        if (d.driver == "v4l2loopback") d.is_loopback = true;
        if (!d.name.empty() && ToLowerAscii(d.name).find("v4l2loopback") != std::string::npos) d.is_loopback = true;
      }

      rep.devices.push_back(d);
    }
  }

  rep.suggested_video_nr = SuggestVideoNr(rep.devices);
  rep.suggested_modprobe_cmd = BuildSuggestedModprobeCmd(rep.suggested_video_nr);

  rep.notes.push_back("OBS may cache device lists; if you add/remove v4l2loopback devices, restart OBS.");
  rep.notes.push_back("If /dev/video* is not writable, ensure your user is in the 'video' group and re-login.");
  rep.notes.push_back("StudioCast will not run modprobe for you; it only prints the suggested command.");

  return rep;
}

}  // namespace studiocast::video
