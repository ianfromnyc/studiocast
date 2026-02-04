#include "daemon_config.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <sstream>
#include <string>

#include "core/util/fs.h"
#include "core/util/strings.h"
#include "core/util/xdg.h"
#include "core/video/effects/effect_types.h"

namespace fs = std::filesystem;

namespace studiocast::config {
namespace {

std::map<std::string, std::string> ParseKeyValueFile(const std::string& content) {
  std::map<std::string, std::string> kv;
  for (const auto& lineRaw : studiocast::util::SplitLines(content)) {
    auto line = studiocast::util::TrimCopy(lineRaw);
    if (line.empty()) continue;
    if (line[0] == '#') continue;

    const auto pos = line.find('=');
    if (pos == std::string::npos) continue;

    auto key = studiocast::util::TrimCopy(line.substr(0, pos));
    auto val = studiocast::util::TrimCopy(line.substr(pos + 1));
    if (key.empty()) continue;
    kv[key] = val;
  }
  return kv;
}

bool ParseBool(const std::string& raw, bool fallback) {
  const auto v = studiocast::util::TrimCopy(raw);
  if (v.empty()) return fallback;
  if (v == "1" || v == "true" || v == "yes" || v == "on") return true;
  if (v == "0" || v == "false" || v == "no" || v == "off") return false;
  return fallback;
}

int ParseInt(const std::string& raw, int fallback) {
  const auto v = studiocast::util::TrimCopy(raw);
  if (v.empty()) return fallback;
  return std::atoi(v.c_str());
}

float ParseFloat(const std::string& raw, float fallback) {
  const auto v = studiocast::util::TrimCopy(raw);
  if (v.empty()) return fallback;
  return static_cast<float>(std::atof(v.c_str()));
}

bool ParseRgbHex(const std::string& raw, std::uint32_t* out) {
  if (!out) return false;
  std::string s = studiocast::util::TrimCopy(raw);
  if (s.empty()) return false;
  if (!s.empty() && s[0] == '#') s.erase(0, 1);
  if (s.size() != 6) return false;

  std::uint32_t v = 0;
  for (const char c : s) {
    v <<= 4u;
    if (c >= '0' && c <= '9') {
      v |= static_cast<std::uint32_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      v |= static_cast<std::uint32_t>(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      v |= static_cast<std::uint32_t>(c - 'A' + 10);
    } else {
      return false;
    }
  }
  *out = v;
  return true;
}

std::string FormatRgbHex(std::uint32_t rgb) {
  std::ostringstream oss;
  oss << "#" << std::hex << std::nouppercase << std::setfill('0') << std::setw(6) << (rgb & 0xFFFFFFu);
  return oss.str();
}

int ParseKeyLightTemperaturePreset(const std::string& raw, int fallback) {
  auto v = studiocast::util::TrimCopy(raw);
  std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  if (v.empty()) return fallback;
  if (v == "0" || v == "neutral") return 0;
  if (v == "1" || v == "warm") return 1;
  if (v == "2" || v == "cool") return 2;
  return fallback;
}

std::string FormatKeyLightTemperaturePreset(int preset) {
  switch (preset) {
    case 1: return "warm";
    case 2: return "cool";
    default: return "neutral";
  }
}

}  // namespace

std::filesystem::path DaemonConfigPath() {
  const auto dir = studiocast::util::StudioCastConfigDir();
  if (dir.empty()) return {};
  return dir / "daemon.conf";
}

DaemonConfig LoadDaemonConfig() {
  DaemonConfig s;

  const auto path = DaemonConfigPath();
  if (!path.empty()) {
    if (auto content = studiocast::util::ReadTextFile(path.string())) {
      const auto kv = ParseKeyValueFile(*content);

      if (auto it = kv.find("video.enabled"); it != kv.end()) {
        s.video_enabled = ParseBool(it->second, s.video_enabled);
      }
      if (auto it = kv.find("video.input"); it != kv.end()) {
        s.video_input_device = it->second;
      }
      if (auto it = kv.find("video.output"); it != kv.end()) {
        s.video_output_device = it->second;
      }
      if (auto it = kv.find("video.width"); it != kv.end()) {
        s.video_width = ParseInt(it->second, s.video_width);
      }
      if (auto it = kv.find("video.height"); it != kv.end()) {
        s.video_height = ParseInt(it->second, s.video_height);
      }
      if (auto it = kv.find("video.fps"); it != kv.end()) {
        s.video_fps = ParseInt(it->second, s.video_fps);
      }
      if (auto it = kv.find("video.mirror"); it != kv.end()) {
        s.video_mirror = ParseBool(it->second, s.video_mirror);
      }

      // New schema (video.effects.*)
      if (auto it = kv.find("video.effects.engine"); it != kv.end()) {
        s.video_effects_engine = it->second;
      }

      if (auto it = kv.find("video.effects.virtual_background.mode"); it != kv.end()) {
        s.video_effects_virtual_background_mode = it->second;
      }
      if (auto it = kv.find("video.effects.virtual_background.blur_strength"); it != kv.end()) {
        s.video_effects_virtual_background_blur_strength =
            ParseInt(it->second, s.video_effects_virtual_background_blur_strength);
      }
      if (auto it = kv.find("video.effects.virtual_background.remove_color"); it != kv.end()) {
        s.video_effects_virtual_background_remove_color = it->second;
      }
      if (auto it = kv.find("video.effects.virtual_background.replace_path"); it != kv.end()) {
        s.video_effects_virtual_background_replace_path = it->second;
      }

      if (auto it = kv.find("video.effects.auto_frame.enabled"); it != kv.end()) {
        s.video_effects_auto_frame_enabled = ParseBool(it->second, s.video_effects_auto_frame_enabled);
      }
      if (auto it = kv.find("video.effects.auto_frame.zoom"); it != kv.end()) {
        s.video_effects_auto_frame_zoom = ParseInt(it->second, s.video_effects_auto_frame_zoom);
      }
      if (auto it = kv.find("video.effects.auto_frame.smoothing"); it != kv.end()) {
        s.video_effects_auto_frame_smoothing =
            ParseInt(it->second, s.video_effects_auto_frame_smoothing);
      }
      if (auto it = kv.find("video.effects.auto_frame.headroom"); it != kv.end()) {
        s.video_effects_auto_frame_headroom = ParseFloat(it->second, s.video_effects_auto_frame_headroom);
      }

      if (auto it = kv.find("video.effects.eye_contact.enabled"); it != kv.end()) {
        s.video_effects_eye_contact_enabled = ParseBool(it->second, s.video_effects_eye_contact_enabled);
      }
      if (auto it = kv.find("video.effects.eye_contact.strength"); it != kv.end()) {
        s.video_effects_eye_contact_strength = ParseInt(it->second, s.video_effects_eye_contact_strength);
      }
      if (auto it = kv.find("video.effects.eye_contact.look_away"); it != kv.end()) {
        s.video_effects_eye_contact_look_away = ParseBool(it->second, s.video_effects_eye_contact_look_away);
      }

      if (auto it = kv.find("video.effects.video_noise_removal.enabled"); it != kv.end()) {
        s.video_effects_video_noise_removal_enabled =
            ParseBool(it->second, s.video_effects_video_noise_removal_enabled);
      }
      if (auto it = kv.find("video.effects.video_noise_removal.strength"); it != kv.end()) {
        s.video_effects_video_noise_removal_strength =
            ParseInt(it->second, s.video_effects_video_noise_removal_strength);
      }

      if (auto it = kv.find("video.effects.virtual_key_light.enabled"); it != kv.end()) {
        s.video_effects_virtual_key_light_enabled =
            ParseBool(it->second, s.video_effects_virtual_key_light_enabled);
      }
      if (auto it = kv.find("video.effects.virtual_key_light.intensity"); it != kv.end()) {
        s.video_effects_virtual_key_light_intensity =
            ParseInt(it->second, s.video_effects_virtual_key_light_intensity);
      }
      if (auto it = kv.find("video.effects.virtual_key_light.temperature_preset"); it != kv.end()) {
        s.video_effects_virtual_key_light_temperature_preset = it->second;
      }
      if (auto it = kv.find("video.effects.virtual_key_light.pan"); it != kv.end()) {
        s.video_effects_virtual_key_light_pan = ParseInt(it->second, s.video_effects_virtual_key_light_pan);
      }
      if (auto it = kv.find("video.effects.virtual_key_light.hdri_path"); it != kv.end()) {
        s.video_effects_virtual_key_light_hdri_path = it->second;
      }

      if (auto it = kv.find("video.effects.vignette.enabled"); it != kv.end()) {
        s.video_effects_vignette_enabled = ParseBool(it->second, s.video_effects_vignette_enabled);
      }
      if (auto it = kv.find("video.effects.vignette.intensity"); it != kv.end()) {
        s.video_effects_vignette_intensity = ParseInt(it->second, s.video_effects_vignette_intensity);
      }
      if (auto it = kv.find("video.effects.vignette.center_on_face"); it != kv.end()) {
        s.video_effects_vignette_center_on_face =
            ParseBool(it->second, s.video_effects_vignette_center_on_face);
      }

      // Legacy keys migration (read old keys as fallback).
      const bool vb_has_any_new = (kv.find("video.effects.virtual_background.mode") != kv.end()) ||
                                 (kv.find("video.effects.virtual_background.blur_strength") != kv.end()) ||
                                 (kv.find("video.effects.virtual_background.remove_color") != kv.end()) ||
                                 (kv.find("video.effects.virtual_background.replace_path") != kv.end());
      const bool af_has_any_new = (kv.find("video.effects.auto_frame.enabled") != kv.end()) ||
                                 (kv.find("video.effects.auto_frame.zoom") != kv.end()) ||
                                 (kv.find("video.effects.auto_frame.smoothing") != kv.end()) ||
                                 (kv.find("video.effects.auto_frame.headroom") != kv.end());

      const bool legacy_bg_has_any = (kv.find("video.background") != kv.end()) ||
                                     (kv.find("video.background_strength") != kv.end()) ||
                                     (kv.find("video.background_remove_color") != kv.end()) ||
                                     (kv.find("video.background_replace_image") != kv.end()) ||
                                     (kv.find("video.auto_frame_strength") != kv.end()) ||
                                     (kv.find("video.auto_frame_smoothing") != kv.end()) ||
                                     (kv.find("video.auto_frame_headroom") != kv.end());

      // Legacy background keys only migrate forward if the new virtual_background/auto_frame keys are absent.
      if (legacy_bg_has_any && !vb_has_any_new && !af_has_any_new) {
        std::string legacy_bg_raw;
        if (auto it = kv.find("video.background"); it != kv.end()) legacy_bg_raw = it->second;
        if (legacy_bg_raw.empty()) legacy_bg_raw = "none";

        studiocast::video::effects::BackgroundEffect legacy_bg =
            studiocast::video::effects::BackgroundEffect::none;
        (void)studiocast::video::effects::ParseBackgroundEffect(legacy_bg_raw, &legacy_bg);

        if (legacy_bg == studiocast::video::effects::BackgroundEffect::auto_frame) {
          s.video_effects_auto_frame_enabled = true;
          if (auto it = kv.find("video.auto_frame_strength"); it != kv.end()) {
            s.video_effects_auto_frame_zoom = ParseInt(it->second, s.video_effects_auto_frame_zoom);
          }
          if (auto it = kv.find("video.auto_frame_smoothing"); it != kv.end()) {
            s.video_effects_auto_frame_smoothing =
                ParseInt(it->second, s.video_effects_auto_frame_smoothing);
          }
          if (auto it = kv.find("video.auto_frame_headroom"); it != kv.end()) {
            s.video_effects_auto_frame_headroom =
                ParseFloat(it->second, s.video_effects_auto_frame_headroom);
          }

          s.video_effects_virtual_background_mode = "none";
        } else {
          s.video_effects_auto_frame_enabled = false;
          s.video_effects_virtual_background_mode = studiocast::video::effects::ToString(legacy_bg);
          if (auto it = kv.find("video.background_strength"); it != kv.end()) {
            s.video_effects_virtual_background_blur_strength =
                ParseInt(it->second, s.video_effects_virtual_background_blur_strength);
          }
          if (auto it = kv.find("video.background_remove_color"); it != kv.end()) {
            s.video_effects_virtual_background_remove_color = it->second;
          }
          if (auto it = kv.find("video.background_replace_image"); it != kv.end()) {
            s.video_effects_virtual_background_replace_path = it->second;
          }
        }
      }

      // Legacy backend selection (deprecated): map to new Maxine-only engine preference.
      if (kv.find("video.effects.engine") == kv.end()) {
        if (auto it = kv.find("video.background_backend"); it != kv.end()) {
          studiocast::video::effects::EffectBackend legacy_be =
              studiocast::video::effects::EffectBackend::auto_select;
          if (studiocast::video::effects::ParseEffectBackend(it->second, &legacy_be)) {
            // Ignore legacy CPU selection.
            if (legacy_be == studiocast::video::effects::EffectBackend::maxine) {
              s.video_effects_engine = "maxine";
            } else {
              s.video_effects_engine = "auto";
            }
          }
        }
      }

      // Legacy key light keys.
      if (kv.find("video.effects.virtual_key_light.enabled") == kv.end()) {
        if (auto it = kv.find("video.virtual_key_light"); it != kv.end()) {
          s.video_effects_virtual_key_light_enabled =
              ParseBool(it->second, s.video_effects_virtual_key_light_enabled);
        }
      }
      if (kv.find("video.effects.virtual_key_light.intensity") == kv.end()) {
        if (auto it = kv.find("video.virtual_key_light_intensity"); it != kv.end()) {
          s.video_effects_virtual_key_light_intensity =
              ParseInt(it->second, s.video_effects_virtual_key_light_intensity);
        }
      }
      if (kv.find("video.effects.virtual_key_light.temperature_preset") == kv.end()) {
        if (auto it = kv.find("video.virtual_key_light_temperature"); it != kv.end()) {
          s.video_effects_virtual_key_light_temperature_preset = it->second;
        }
      }
      if (kv.find("video.effects.virtual_key_light.pan") == kv.end()) {
        if (auto it = kv.find("video.virtual_key_light_pan"); it != kv.end()) {
          s.video_effects_virtual_key_light_pan = ParseInt(it->second, s.video_effects_virtual_key_light_pan);
        }
      }
      if (kv.find("video.effects.virtual_key_light.hdri_path") == kv.end()) {
        if (auto it = kv.find("video.virtual_key_light_hdri"); it != kv.end()) {
          s.video_effects_virtual_key_light_hdri_path = it->second;
        }
      }

      // Legacy eye contact keys.
      if (kv.find("video.effects.eye_contact.enabled") == kv.end()) {
        if (auto it = kv.find("video.eye_contact"); it != kv.end()) {
          s.video_effects_eye_contact_enabled = ParseBool(it->second, s.video_effects_eye_contact_enabled);
        }
      }
      if (kv.find("video.effects.eye_contact.strength") == kv.end()) {
        if (auto it = kv.find("video.eye_contact_strength"); it != kv.end()) {
          s.video_effects_eye_contact_strength = ParseInt(it->second, s.video_effects_eye_contact_strength);
        }
      }
      if (kv.find("video.effects.eye_contact.look_away") == kv.end()) {
        if (auto it = kv.find("video.eye_contact_look_away"); it != kv.end()) {
          s.video_effects_eye_contact_look_away = ParseBool(it->second, s.video_effects_eye_contact_look_away);
        }
      }

      // Legacy vignette keys.
      if (kv.find("video.effects.vignette.enabled") == kv.end()) {
        if (auto it = kv.find("video.vignette"); it != kv.end()) {
          s.video_effects_vignette_enabled = ParseBool(it->second, s.video_effects_vignette_enabled);
        }
      }
      if (kv.find("video.effects.vignette.intensity") == kv.end()) {
        if (auto it = kv.find("video.vignette_intensity"); it != kv.end()) {
          s.video_effects_vignette_intensity = ParseInt(it->second, s.video_effects_vignette_intensity);
        }
      }
      if (kv.find("video.effects.vignette.center_on_face") == kv.end()) {
        if (auto it = kv.find("video.vignette_center_on_face"); it != kv.end()) {
          s.video_effects_vignette_center_on_face =
              ParseBool(it->second, s.video_effects_vignette_center_on_face);
        }
      }

      if (auto it = kv.find("service.consumer_poll_ms"); it != kv.end()) {
        s.consumer_poll_ms = ParseInt(it->second, s.consumer_poll_ms);
      }
      if (auto it = kv.find("service.stop_grace_ms"); it != kv.end()) {
        s.stop_grace_ms = ParseInt(it->second, s.stop_grace_ms);
      }
      if (auto it = kv.find("service.always_on"); it != kv.end()) {
        s.always_on = ParseBool(it->second, s.always_on);
      }
    }
  }

  return s;
}

bool SaveDaemonConfig(const DaemonConfig& s, std::string* error) {
  const auto path = DaemonConfigPath();
  if (path.empty()) {
    if (error) *error = "DaemonConfigPath() is empty (HOME/XDG_CONFIG_HOME not available).";
    return false;
  }

  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  if (ec) {
    if (error) *error = "Failed to create config dir: " + ec.message();
    return false;
  }

  std::ofstream out(path, std::ios::out | std::ios::trunc);
  if (!out.is_open()) {
    if (error) *error = "Failed to open daemon config for writing: " + path.string();
    return false;
  }

  out << "# StudioCast daemon (studiocastd) configuration\n";
  out << "# This file is managed by the StudioCast GUI / studiocastctl.\n\n";

  out << "video.enabled = " << (s.video_enabled ? "true" : "false") << "\n";
  if (!s.video_input_device.empty()) out << "video.input = " << s.video_input_device << "\n";
  if (!s.video_output_device.empty()) out << "video.output = " << s.video_output_device << "\n";
  out << "video.width = " << s.video_width << "\n";
  out << "video.height = " << s.video_height << "\n";
  out << "video.fps = " << s.video_fps << "\n";
  out << "video.mirror = " << (s.video_mirror ? "true" : "false") << "\n\n";

  out << "# Video effects (Maxine-only)\n";
  out << "video.effects.engine = " << s.video_effects_engine << "\n\n";

  out << "video.effects.virtual_background.mode = " << s.video_effects_virtual_background_mode << "\n";
  out << "video.effects.virtual_background.blur_strength = "
      << s.video_effects_virtual_background_blur_strength << "\n";
  if (!s.video_effects_virtual_background_remove_color.empty()) {
    out << "video.effects.virtual_background.remove_color = "
        << s.video_effects_virtual_background_remove_color << "\n";
  }
  if (!s.video_effects_virtual_background_replace_path.empty()) {
    out << "video.effects.virtual_background.replace_path = "
        << s.video_effects_virtual_background_replace_path << "\n";
  }
  out << "\n";

  out << "video.effects.auto_frame.enabled = " << (s.video_effects_auto_frame_enabled ? "true" : "false")
      << "\n";
  out << "video.effects.auto_frame.zoom = " << s.video_effects_auto_frame_zoom << "\n";
  out << "video.effects.auto_frame.smoothing = " << s.video_effects_auto_frame_smoothing << "\n";
  out << "video.effects.auto_frame.headroom = " << s.video_effects_auto_frame_headroom << "\n\n";

  out << "video.effects.eye_contact.enabled = " << (s.video_effects_eye_contact_enabled ? "true" : "false")
      << "\n";
  out << "video.effects.eye_contact.strength = " << s.video_effects_eye_contact_strength << "\n";
  out << "video.effects.eye_contact.look_away = "
      << (s.video_effects_eye_contact_look_away ? "true" : "false") << "\n\n";

  out << "video.effects.video_noise_removal.enabled = "
      << (s.video_effects_video_noise_removal_enabled ? "true" : "false") << "\n";
  out << "video.effects.video_noise_removal.strength = " << s.video_effects_video_noise_removal_strength
      << "\n\n";

  out << "video.effects.virtual_key_light.enabled = "
      << (s.video_effects_virtual_key_light_enabled ? "true" : "false") << "\n";
  out << "video.effects.virtual_key_light.intensity = " << s.video_effects_virtual_key_light_intensity
      << "\n";
  out << "video.effects.virtual_key_light.temperature_preset = "
      << s.video_effects_virtual_key_light_temperature_preset << "\n";
  out << "video.effects.virtual_key_light.pan = " << s.video_effects_virtual_key_light_pan << "\n";
  if (!s.video_effects_virtual_key_light_hdri_path.empty()) {
    out << "video.effects.virtual_key_light.hdri_path = " << s.video_effects_virtual_key_light_hdri_path
        << "\n";
  }
  out << "\n";

  out << "video.effects.vignette.enabled = " << (s.video_effects_vignette_enabled ? "true" : "false")
      << "\n";
  out << "video.effects.vignette.intensity = " << s.video_effects_vignette_intensity << "\n";
  out << "video.effects.vignette.center_on_face = "
      << (s.video_effects_vignette_center_on_face ? "true" : "false") << "\n\n";

  out << "service.consumer_poll_ms = " << s.consumer_poll_ms << "\n";
  out << "service.stop_grace_ms = " << s.stop_grace_ms << "\n";
  out << "service.always_on = " << (s.always_on ? "true" : "false") << "\n";

  return true;
}

studiocast::video::VirtualCameraServiceConfig ToVideoServiceConfig(const DaemonConfig& s) {
  studiocast::video::VirtualCameraServiceConfig cfg;
  cfg.enabled = s.video_enabled;
  cfg.pipeline.input_device = s.video_input_device;
  cfg.pipeline.output_device = s.video_output_device;
  cfg.pipeline.width = s.video_width;
  cfg.pipeline.height = s.video_height;
  cfg.pipeline.fps = s.video_fps;
  cfg.pipeline.effects.mirror = s.video_mirror;

  // Parse the persisted config fields into the strongly-typed effect config.
  {
    // Maxine-only engine preference (ignore CPU legacy).
    cfg.pipeline.effects.background_backend = studiocast::video::effects::EffectBackend::auto_select;
    {
      std::string raw = s.video_effects_engine;
      std::transform(raw.begin(), raw.end(), raw.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      if (raw == "maxine") {
        cfg.pipeline.effects.background_backend = studiocast::video::effects::EffectBackend::maxine;
      } else {
        cfg.pipeline.effects.background_backend = studiocast::video::effects::EffectBackend::auto_select;
      }
    }

    // Auto Frame and Virtual Background are mutually exclusive. Auto Frame wins if enabled.
    if (s.video_effects_auto_frame_enabled) {
      cfg.pipeline.effects.background = studiocast::video::effects::BackgroundEffect::auto_frame;
      cfg.pipeline.effects.auto_frame.strength = std::max(0, std::min(100, s.video_effects_auto_frame_zoom));
      cfg.pipeline.effects.auto_frame.smoothing =
          std::max(0, std::min(100, s.video_effects_auto_frame_smoothing));
      cfg.pipeline.effects.auto_frame.headroom =
          std::max(0.0f, std::min(1.0f, s.video_effects_auto_frame_headroom));
    } else {
      studiocast::video::effects::BackgroundEffect bg = studiocast::video::effects::BackgroundEffect::none;
      if (studiocast::video::effects::ParseBackgroundEffect(s.video_effects_virtual_background_mode, &bg)) {
        if (bg == studiocast::video::effects::BackgroundEffect::auto_frame) {
          bg = studiocast::video::effects::BackgroundEffect::none;
        }
        cfg.pipeline.effects.background = bg;
      }
    }

    cfg.pipeline.effects.background_strength =
        std::max(1, std::min(64, s.video_effects_virtual_background_blur_strength));

    cfg.pipeline.effects.background_replace_image = s.video_effects_virtual_background_replace_path;
    std::uint32_t rgb = 0;
    if (ParseRgbHex(s.video_effects_virtual_background_remove_color, &rgb)) {
      cfg.pipeline.effects.background_remove_color_rgb = rgb;
    }

    cfg.pipeline.effects.denoise = s.video_effects_video_noise_removal_enabled;
    cfg.pipeline.effects.denoise_strength =
        std::max(0, std::min(100, s.video_effects_video_noise_removal_strength));

    cfg.pipeline.effects.virtual_key_light.enabled = s.video_effects_virtual_key_light_enabled;
    cfg.pipeline.effects.virtual_key_light.intensity =
        static_cast<float>(std::max(0, std::min(100, s.video_effects_virtual_key_light_intensity))) / 100.0f;
    cfg.pipeline.effects.virtual_key_light.temperature_preset =
        ParseKeyLightTemperaturePreset(s.video_effects_virtual_key_light_temperature_preset,
                                       cfg.pipeline.effects.virtual_key_light.temperature_preset);
    cfg.pipeline.effects.virtual_key_light.direction_pan_degrees =
        static_cast<float>(std::max(-180, std::min(180, s.video_effects_virtual_key_light_pan)));
    cfg.pipeline.effects.virtual_key_light.hdri_path = s.video_effects_virtual_key_light_hdri_path;

    cfg.pipeline.effects.eye_contact.enabled = s.video_effects_eye_contact_enabled;
    cfg.pipeline.effects.eye_contact.strength =
        std::max(0, std::min(100, s.video_effects_eye_contact_strength));
    cfg.pipeline.effects.eye_contact.look_away_enabled = s.video_effects_eye_contact_look_away;

    cfg.pipeline.effects.vignette.enabled = s.video_effects_vignette_enabled;
    cfg.pipeline.effects.vignette.intensity =
        static_cast<float>(std::max(0, std::min(100, s.video_effects_vignette_intensity))) / 100.0f;
    cfg.pipeline.effects.vignette.center_on_tracked_face = s.video_effects_vignette_center_on_face;
  }

  cfg.consumer_poll_ms = s.consumer_poll_ms;
  cfg.stop_grace_ms = s.stop_grace_ms;
  cfg.always_on = s.always_on;
  return cfg;
}

void ApplyVideoServiceConfigToDaemonConfig(const studiocast::video::VirtualCameraServiceConfig& cfg,
                                          DaemonConfig* out) {
  if (!out) return;
  out->video_enabled = cfg.enabled;
  out->video_input_device = cfg.pipeline.input_device;
  out->video_output_device = cfg.pipeline.output_device;
  out->video_width = cfg.pipeline.width;
  out->video_height = cfg.pipeline.height;
  out->video_fps = cfg.pipeline.fps;
  out->video_mirror = cfg.pipeline.effects.mirror;

  // Effects engine preference (Maxine-only; ignore legacy CPU selection).
  out->video_effects_engine = "auto";
  if (cfg.pipeline.effects.background_backend == studiocast::video::effects::EffectBackend::maxine) {
    out->video_effects_engine = "maxine";
  }

  // Auto Frame vs Virtual Background.
  out->video_effects_auto_frame_enabled =
      (cfg.pipeline.effects.background == studiocast::video::effects::BackgroundEffect::auto_frame);
  out->video_effects_auto_frame_zoom = std::max(0, std::min(100, cfg.pipeline.effects.auto_frame.strength));
  out->video_effects_auto_frame_smoothing = std::max(0, std::min(100, cfg.pipeline.effects.auto_frame.smoothing));
  out->video_effects_auto_frame_headroom =
      std::max(0.0f, std::min(1.0f, cfg.pipeline.effects.auto_frame.headroom));

  if (out->video_effects_auto_frame_enabled) {
    out->video_effects_virtual_background_mode = "none";
  } else {
    out->video_effects_virtual_background_mode =
        studiocast::video::effects::ToString(cfg.pipeline.effects.background);
  }
  out->video_effects_virtual_background_blur_strength = cfg.pipeline.effects.background_strength;
  out->video_effects_virtual_background_remove_color =
      FormatRgbHex(cfg.pipeline.effects.background_remove_color_rgb);
  out->video_effects_virtual_background_replace_path = cfg.pipeline.effects.background_replace_image.string();

  out->video_effects_video_noise_removal_enabled = cfg.pipeline.effects.denoise;
  out->video_effects_video_noise_removal_strength =
      std::max(0, std::min(100, cfg.pipeline.effects.denoise_strength));

  out->video_effects_virtual_key_light_enabled = cfg.pipeline.effects.virtual_key_light.enabled;
  out->video_effects_virtual_key_light_intensity =
      std::max(0, std::min(100, static_cast<int>(cfg.pipeline.effects.virtual_key_light.intensity * 100.0f)));
  out->video_effects_virtual_key_light_temperature_preset =
      FormatKeyLightTemperaturePreset(cfg.pipeline.effects.virtual_key_light.temperature_preset);
  out->video_effects_virtual_key_light_pan =
      std::max(-180, std::min(180, static_cast<int>(cfg.pipeline.effects.virtual_key_light.direction_pan_degrees)));
  out->video_effects_virtual_key_light_hdri_path = cfg.pipeline.effects.virtual_key_light.hdri_path.string();

  out->video_effects_eye_contact_enabled = cfg.pipeline.effects.eye_contact.enabled;
  out->video_effects_eye_contact_strength = std::max(0, std::min(100, cfg.pipeline.effects.eye_contact.strength));
  out->video_effects_eye_contact_look_away = cfg.pipeline.effects.eye_contact.look_away_enabled;

  out->video_effects_vignette_enabled = cfg.pipeline.effects.vignette.enabled;
  out->video_effects_vignette_intensity =
      std::max(0, std::min(100, static_cast<int>(cfg.pipeline.effects.vignette.intensity * 100.0f)));
  out->video_effects_vignette_center_on_face = cfg.pipeline.effects.vignette.center_on_tracked_face;

  out->consumer_poll_ms = cfg.consumer_poll_ms;
  out->stop_grace_ms = cfg.stop_grace_ms;
  out->always_on = cfg.always_on;
}

}  // namespace studiocast::config
