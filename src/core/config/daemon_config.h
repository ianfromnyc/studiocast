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

  // Broadcast-style camera effects (video), persisted under a stable `video.effects.*` namespace.
  //
  // Product rule: Maxine is the only supported effect engine in production.
  // Legacy/CPU backend selections are ignored.
  //
  // Engine preference (Maxine-only):
  // - video.effects.engine: auto|maxine
  std::string video_effects_engine = "auto";

  // Virtual background:
  // - video.effects.virtual_background.mode: none|blur|remove|replace
  // - video.effects.virtual_background.blur_strength: integer strength (1..64-ish)
  // - video.effects.virtual_background.remove_color: #RRGGBB (used when mode=remove)
  // - video.effects.virtual_background.replace_path: filesystem path (used when mode=replace)
  std::string video_effects_virtual_background_mode = "none";
  int video_effects_virtual_background_blur_strength = 8;
  std::string video_effects_virtual_background_remove_color = "#000000";
  std::string video_effects_virtual_background_replace_path;

  // Auto Frame:
  // - video.effects.auto_frame.enabled: true|false
  // - video.effects.auto_frame.zoom: 0..100 (percent-like)
  // - video.effects.auto_frame.smoothing: 0..100 (percent-like)
  // - video.effects.auto_frame.headroom: 0..1 (fraction)
  bool video_effects_auto_frame_enabled = false;
  int video_effects_auto_frame_zoom = 50;
  int video_effects_auto_frame_smoothing = 70;
  float video_effects_auto_frame_headroom = 0.15f;

  // Eye Contact:
  // - video.effects.eye_contact.enabled: true|false
  // - video.effects.eye_contact.strength: 0..100 (percent)
  // - video.effects.eye_contact.look_away: true|false
  bool video_effects_eye_contact_enabled = false;
  int video_effects_eye_contact_strength = 50;
  bool video_effects_eye_contact_look_away = true;

  // Video Noise Removal:
  // - video.effects.video_noise_removal.enabled: true|false
  // - video.effects.video_noise_removal.strength: 0..100 (percent)
  bool video_effects_video_noise_removal_enabled = false;
  int video_effects_video_noise_removal_strength = 50;

  // Virtual Key Light:
  // - video.effects.virtual_key_light.enabled: true|false
  // - video.effects.virtual_key_light.intensity: 0..100 (percent)
  // - video.effects.virtual_key_light.temperature_preset: neutral|warm|cool
  // - video.effects.virtual_key_light.pan: degrees (integer)
  // - video.effects.virtual_key_light.hdri_path: filesystem path (optional override)
  bool video_effects_virtual_key_light_enabled = false;
  int video_effects_virtual_key_light_intensity = 70;
  std::string video_effects_virtual_key_light_temperature_preset = "neutral";
  int video_effects_virtual_key_light_pan = 0;
  std::string video_effects_virtual_key_light_hdri_path;

  // Vignette:
  // - video.effects.vignette.enabled: true|false
  // - video.effects.vignette.intensity: 0..100 (percent)
  // - video.effects.vignette.center_on_face: true|false
  bool video_effects_vignette_enabled = false;
  int video_effects_vignette_intensity = 35;
  bool video_effects_vignette_center_on_face = true;

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
