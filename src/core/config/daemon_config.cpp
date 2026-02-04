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

      if (auto it = kv.find("video.background"); it != kv.end()) {
        s.video_background = it->second;
      }
      if (auto it = kv.find("video.background_backend"); it != kv.end()) {
        s.video_background_backend = it->second;
      }
      if (auto it = kv.find("video.background_strength"); it != kv.end()) {
        s.video_background_strength = ParseInt(it->second, s.video_background_strength);
      }
      if (auto it = kv.find("video.background_remove_color"); it != kv.end()) {
        s.video_background_remove_color = it->second;
      }
      if (auto it = kv.find("video.background_replace_image"); it != kv.end()) {
        s.video_background_replace_image = it->second;
      }

      if (auto it = kv.find("video.virtual_key_light"); it != kv.end()) {
        s.video_virtual_key_light = ParseBool(it->second, s.video_virtual_key_light);
      }
      if (auto it = kv.find("video.virtual_key_light_intensity"); it != kv.end()) {
        s.video_virtual_key_light_intensity = ParseInt(it->second, s.video_virtual_key_light_intensity);
      }
      if (auto it = kv.find("video.virtual_key_light_temperature"); it != kv.end()) {
        s.video_virtual_key_light_temperature = it->second;
      }
      if (auto it = kv.find("video.virtual_key_light_pan"); it != kv.end()) {
        s.video_virtual_key_light_pan = ParseInt(it->second, s.video_virtual_key_light_pan);
      }
      if (auto it = kv.find("video.virtual_key_light_hdri"); it != kv.end()) {
        s.video_virtual_key_light_hdri = it->second;
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

  out << "video.background = " << s.video_background << "\n";
  out << "video.background_backend = " << s.video_background_backend << "\n";
  out << "video.background_strength = " << s.video_background_strength << "\n\n";

  if (!s.video_background_remove_color.empty()) {
    out << "video.background_remove_color = " << s.video_background_remove_color << "\n";
  }
  if (!s.video_background_replace_image.empty()) {
    out << "video.background_replace_image = " << s.video_background_replace_image << "\n";
  }
  out << "\n";

  out << "video.virtual_key_light = " << (s.video_virtual_key_light ? "true" : "false") << "\n";
  out << "video.virtual_key_light_intensity = " << s.video_virtual_key_light_intensity << "\n";
  out << "video.virtual_key_light_temperature = " << s.video_virtual_key_light_temperature << "\n";
  out << "video.virtual_key_light_pan = " << s.video_virtual_key_light_pan << "\n";
  if (!s.video_virtual_key_light_hdri.empty()) {
    out << "video.virtual_key_light_hdri = " << s.video_virtual_key_light_hdri << "\n";
  }
  out << "\n";

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

  // Parse the persisted string fields into the strongly-typed effect config.
  {
    studiocast::video::effects::BackgroundEffect bg = studiocast::video::effects::BackgroundEffect::none;
    if (studiocast::video::effects::ParseBackgroundEffect(s.video_background, &bg)) {
      cfg.pipeline.effects.background = bg;
    }

    studiocast::video::effects::EffectBackend be = studiocast::video::effects::EffectBackend::auto_select;
    if (studiocast::video::effects::ParseEffectBackend(s.video_background_backend, &be)) {
      cfg.pipeline.effects.background_backend = be;
    }

    cfg.pipeline.effects.background_strength = std::max(1, std::min(64, s.video_background_strength));

    cfg.pipeline.effects.background_replace_image = s.video_background_replace_image;
    std::uint32_t rgb = 0;
    if (ParseRgbHex(s.video_background_remove_color, &rgb)) {
      cfg.pipeline.effects.background_remove_color_rgb = rgb;
    }

    cfg.pipeline.effects.virtual_key_light.enabled = s.video_virtual_key_light;
    cfg.pipeline.effects.virtual_key_light.intensity =
        std::max(0, std::min(100, s.video_virtual_key_light_intensity)) / 100.0f;
    cfg.pipeline.effects.virtual_key_light.temperature_preset =
        ParseKeyLightTemperaturePreset(s.video_virtual_key_light_temperature,
                                       cfg.pipeline.effects.virtual_key_light.temperature_preset);
    cfg.pipeline.effects.virtual_key_light.direction_pan_degrees =
        static_cast<float>(std::max(-180, std::min(180, s.video_virtual_key_light_pan)));
    cfg.pipeline.effects.virtual_key_light.hdri_path = s.video_virtual_key_light_hdri;
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

  out->video_background = studiocast::video::effects::ToString(cfg.pipeline.effects.background);
  out->video_background_backend = studiocast::video::effects::ToString(cfg.pipeline.effects.background_backend);
  out->video_background_strength = cfg.pipeline.effects.background_strength;

  out->video_background_remove_color = FormatRgbHex(cfg.pipeline.effects.background_remove_color_rgb);
  out->video_background_replace_image = cfg.pipeline.effects.background_replace_image.string();

  out->video_virtual_key_light = cfg.pipeline.effects.virtual_key_light.enabled;
  out->video_virtual_key_light_intensity =
      std::max(0, std::min(100, static_cast<int>(cfg.pipeline.effects.virtual_key_light.intensity * 100.0f)));
  out->video_virtual_key_light_temperature =
      FormatKeyLightTemperaturePreset(cfg.pipeline.effects.virtual_key_light.temperature_preset);
  out->video_virtual_key_light_pan =
      std::max(-180, std::min(180, static_cast<int>(cfg.pipeline.effects.virtual_key_light.direction_pan_degrees)));
  out->video_virtual_key_light_hdri = cfg.pipeline.effects.virtual_key_light.hdri_path.string();

  out->consumer_poll_ms = cfg.consumer_poll_ms;
  out->stop_grace_ms = cfg.stop_grace_ms;
  out->always_on = cfg.always_on;
}

}  // namespace studiocast::config
