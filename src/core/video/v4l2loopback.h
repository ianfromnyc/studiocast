#pragma once

#include <string>
#include <vector>

namespace studiocast::video {

struct VideoDevice {
  std::string sys_name; // e.g. "video0"
  std::string dev_node; // e.g. "/dev/video0"
  std::string name;     // from /sys/class/video4linux/.../name (best-effort)
  std::string driver;   // from sysfs driver symlink (best-effort)

  bool is_loopback = false;
  bool can_read = false;
  bool can_write = false;
};

struct LoopbackReport {
  bool sys_video_class_present = false;

  bool modinfo_available = false;
  bool module_installed = false;
  bool module_loaded = false;

  std::vector<VideoDevice> devices;

  int suggested_video_nr = -1;
  std::string suggested_modprobe_cmd;

  std::vector<std::string> notes;

  bool ReadyForVirtualCamera() const;
  std::string ToText() const;
};

LoopbackReport ProbeLoopback();

} // namespace studiocast::video
