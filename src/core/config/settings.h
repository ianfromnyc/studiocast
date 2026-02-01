#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace studiocast::config {

enum class GpuSelectMode { Auto, Index, Uuid };

struct GpuSelection {
  GpuSelectMode mode = GpuSelectMode::Auto;
  std::optional<int> index;
  std::string uuid;
};

struct Settings {
  GpuSelection gpu;
};

// ~/.config/studiocast/settings.conf (respecting XDG_CONFIG_HOME)
std::filesystem::path SettingsPath();

Settings LoadSettings();
bool SaveSettings(const Settings &s, std::string *error);

std::string ToString(GpuSelectMode m);
GpuSelectMode ParseGpuSelectMode(const std::string &raw);

} // namespace studiocast::config
