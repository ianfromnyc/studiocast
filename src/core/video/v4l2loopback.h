#pragma once

#include <chrono>
#include <filesystem>
#include <string>
#include <vector>

namespace studiocast::video {

struct LoopbackProcessInfo {
  int pid = -1;
  std::string name;
};

struct LoopbackCommandOutput {
  bool attempted = false;
  bool available = false;
  bool timed_out = false;
  int exit_code = -1;
  std::string output;
  std::string error;
};

struct LoopbackModuleParameter {
  std::string name;
  std::string value;
};

struct VideoDevice {
  std::string sys_name; // e.g. "video0"
  std::string dev_node; // e.g. "/dev/video0"
  std::string name;     // from /sys/class/video4linux/.../name (best-effort)
  std::string driver;   // from sysfs driver symlink (best-effort)

  bool is_loopback = false;
  bool can_read = false;
  bool can_write = false;

  // Populated only by ProbeLoopbackDiagnostics().
  std::string current_caps_summary;
  std::string current_caps_error;
  bool holder_scan_attempted = false;
  std::vector<LoopbackProcessInfo> holders;
  std::string holder_scan_error;
  LoopbackCommandOutput v4l2_ctl_driver;
  LoopbackCommandOutput v4l2_ctl_formats_ext;
};

struct LoopbackReport {
  bool sys_video_class_present = false;

  bool modinfo_available = false;
  bool module_installed = false;
  bool module_loaded = false;

  bool module_parameters_checked = false;
  bool module_parameters_available = false;
  std::string module_parameters_error;
  std::vector<LoopbackModuleParameter> module_parameters;

  bool v4l2_ctl_checked = false;
  bool v4l2_ctl_available = false;

  std::vector<VideoDevice> devices;

  int suggested_video_nr = -1;
  std::string suggested_modprobe_cmd;

  std::vector<std::string> notes;

  bool ReadyForVirtualCamera() const;
  std::string ToText() const;
  std::string ToJson() const;
};

struct LoopbackProbeOptions {
  std::filesystem::path sys_video_class_path = "/sys/class/video4linux";
  std::filesystem::path dev_root_path = "/dev";
  std::filesystem::path module_parameters_path =
      "/sys/module/v4l2loopback/parameters";

  bool collect_module_parameters = false;
  bool collect_holders = false;
  bool collect_v4l2_ctl = false;

  int holder_exclude_pid = -1;
  std::string v4l2_ctl_command = "v4l2-ctl";
  std::chrono::milliseconds command_timeout{750};
};

LoopbackReport ProbeLoopback();
LoopbackReport ProbeLoopback(const LoopbackProbeOptions &options);
LoopbackReport ProbeLoopbackDiagnostics();

} // namespace studiocast::video
