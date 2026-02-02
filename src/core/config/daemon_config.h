#pragma once

#include <filesystem>
#include <string>

#include "core/video/virtual_camera_service.h"

namespace studiocast::config {

// Persistent configuration for studiocastd.
// Stored as a simple key=value file under XDG_CONFIG_HOME.
struct DaemonConfig {
  // Video
  bool video_enabled = true;
  std::string video_input_device;   // empty = auto
  std::string video_output_device;  // empty = auto
  int video_width = 1280;
  int video_height = 720;
  int video_fps = 30;
  bool video_mirror = false;

  // Service behavior
  int consumer_poll_ms = 250;
  int stop_grace_ms = 1000;
  bool always_on = false;
};

// ~/.config/studiocast/daemon.conf (respecting XDG_CONFIG_HOME)
std::filesystem::path DaemonConfigPath();

DaemonConfig LoadDaemonConfig();
bool SaveDaemonConfig(const DaemonConfig& s, std::string* error);

// Convert persisted settings into the runtime VirtualCameraServiceConfig used by the daemon.
studiocast::video::VirtualCameraServiceConfig ToVideoServiceConfig(const DaemonConfig& s);

// Update a DaemonConfig from a runtime service config (useful for persistence on IPC changes).
void ApplyVideoServiceConfigToDaemonConfig(const studiocast::video::VirtualCameraServiceConfig& cfg,
                                          DaemonConfig* out);

}  // namespace studiocast::config
