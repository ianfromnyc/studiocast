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

  // NVIDIA Broadcast-style effects (video).
  //
  // These are persisted as strings to keep the config file human-readable and
  // stable as we add more effects/backends.
  //
  // video.background: none|blur|remove|replace|auto_frame
  // video.background_backend: auto|cpu|maxine
  // video.background_strength: integer intensity knob (blur radius for CPU placeholder)
  // video.background_remove_color: #RRGGBB (used when video.background=remove)
  // video.background_replace_image: filesystem path (used when video.background=replace)
  std::string video_background = "none";
  std::string video_background_backend = "auto";
  int video_background_strength = 8;

  std::string video_background_remove_color = "#000000";
  std::string video_background_replace_image;

  // Virtual Key Light (Video Relighting)
  //
  // video.virtual_key_light: true|false
  // video.virtual_key_light_intensity: 0..100 (percent)
  // video.virtual_key_light_temperature: neutral|warm|cool
  // video.virtual_key_light_pan: degrees (integer)
  // video.virtual_key_light_hdri: filesystem path (optional override)
  bool video_virtual_key_light = false;
  int video_virtual_key_light_intensity = 70;
  std::string video_virtual_key_light_temperature = "neutral";
  int video_virtual_key_light_pan = 0;
  std::string video_virtual_key_light_hdri;

  // Eye Contact (Maxine AR Gaze Redirection)
  //
  // video.eye_contact: true|false
  // video.eye_contact_strength: 0..100 (percent)
  // video.eye_contact_look_away: true|false
  bool video_eye_contact = false;
  int video_eye_contact_strength = 50;
  bool video_eye_contact_look_away = true;

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
