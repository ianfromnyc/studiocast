#include "settings.h"

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

std::string ToLowerCopy(std::string s) {
  for (char &c : s) {
    if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

std::map<std::string, std::string>
ParseKeyValueFile(const std::string &content) {
  std::map<std::string, std::string> kv;
  for (const auto &lineRaw : util::SplitLines(content)) {
    auto line = util::TrimCopy(lineRaw);
    if (line.empty())
      continue;
    if (line[0] == '#')
      continue;

    const auto pos = line.find('=');
    if (pos == std::string::npos)
      continue;

    auto key = util::TrimCopy(line.substr(0, pos));
    auto val = util::TrimCopy(line.substr(pos + 1));
    if (key.empty())
      continue;

    kv[key] = val;
  }
  return kv;
}

void ApplyEnvOverrides(Settings *s) {
  // Optional env overrides (useful for CI or one-off runs).
  const char *mode = std::getenv("STUDIOCAST_GPU_MODE");
  const char *uuid = std::getenv("STUDIOCAST_GPU_UUID");
  const char *idx = std::getenv("STUDIOCAST_GPU_INDEX");

  if (mode && *mode) {
    s->gpu.mode = ParseGpuSelectMode(mode);
  }
  if (uuid && *uuid) {
    s->gpu.uuid = uuid;
    s->gpu.mode = GpuSelectMode::Uuid;
    s->gpu.index.reset();
  }
  if (idx && *idx) {
    s->gpu.index = std::atoi(idx);
    s->gpu.mode = GpuSelectMode::Index;
    s->gpu.uuid.clear();
  }
}

} // namespace

std::filesystem::path SettingsPath() {
  const auto dir = util::StudioCastConfigDir();
  if (dir.empty())
    return {};
  return dir / "settings.conf";
}

std::string ToString(GpuSelectMode m) {
  switch (m) {
  case GpuSelectMode::Auto:
    return "auto";
  case GpuSelectMode::Index:
    return "index";
  case GpuSelectMode::Uuid:
    return "uuid";
  }
  return "auto";
}

GpuSelectMode ParseGpuSelectMode(const std::string &raw) {
  const auto s = ToLowerCopy(util::TrimCopy(raw));
  if (s == "auto")
    return GpuSelectMode::Auto;
  if (s == "index")
    return GpuSelectMode::Index;
  if (s == "uuid")
    return GpuSelectMode::Uuid;
  return GpuSelectMode::Auto;
}

Settings LoadSettings() {
  Settings s; // defaults to auto

  const auto path = SettingsPath();
  if (!path.empty()) {
    if (auto content = util::ReadTextFile(path.string())) {
      const auto kv = ParseKeyValueFile(*content);

      if (auto it = kv.find("gpu.mode"); it != kv.end()) {
        s.gpu.mode = ParseGpuSelectMode(it->second);
      }
      if (auto it = kv.find("gpu.index"); it != kv.end()) {
        s.gpu.index = std::atoi(it->second.c_str());
      }
      if (auto it = kv.find("gpu.uuid"); it != kv.end()) {
        s.gpu.uuid = it->second;
      }

      // Normalize: mode wins
      if (s.gpu.mode == GpuSelectMode::Uuid) {
        s.gpu.index.reset();
      } else if (s.gpu.mode == GpuSelectMode::Index) {
        s.gpu.uuid.clear();
      } else {
        s.gpu.index.reset();
        s.gpu.uuid.clear();
      }
    }
  }

  ApplyEnvOverrides(&s);
  return s;
}

bool SaveSettings(const Settings &s, std::string *error) {
  const auto path = SettingsPath();
  if (path.empty()) {
    if (error)
      *error = "SettingsPath() is empty (HOME/XDG_CONFIG_HOME not available).";
    return false;
  }

  std::error_code ec;
  fs::create_directories(path.parent_path(), ec);
  if (ec) {
    if (error)
      *error = "Failed to create config dir: " + ec.message();
    return false;
  }

  std::ofstream out(path, std::ios::out | std::ios::trunc);
  if (!out.is_open()) {
    if (error)
      *error = "Failed to open settings file for writing: " + path.string();
    return false;
  }

  out << "# StudioCast settings\n";
  out << "# Values: gpu.mode = auto | index | uuid\n\n";
  out << "gpu.mode = " << ToString(s.gpu.mode) << "\n";
  if (s.gpu.mode == GpuSelectMode::Index && s.gpu.index) {
    out << "gpu.index = " << *s.gpu.index << "\n";
  }
  if (s.gpu.mode == GpuSelectMode::Uuid && !s.gpu.uuid.empty()) {
    out << "gpu.uuid = " << s.gpu.uuid << "\n";
  }

  return true;
}

} // namespace studiocast::config
