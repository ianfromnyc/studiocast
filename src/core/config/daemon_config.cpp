#include "daemon_config.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

#include "core/util/fs.h"
#include "core/util/strings.h"
#include "core/util/xdg.h"

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

  out->consumer_poll_ms = cfg.consumer_poll_ms;
  out->stop_grace_ms = cfg.stop_grace_ms;
  out->always_on = cfg.always_on;
}

}  // namespace studiocast::config
