#pragma once

#include <optional>
#include <string_view>

namespace studiocast::video::effects::contract {

inline int TemperaturePresetFromString(const std::string_view s) {
  if (s == "warm")
    return 1;
  if (s == "cool")
    return 2;
  return 0;
}

inline std::string_view TemperaturePresetToString(const int preset) {
  switch (preset) {
  case 1:
    return "warm";
  case 2:
    return "cool";
  default:
    return "neutral";
  }
}

inline std::optional<int> KelvinFromTemperaturePreset(const int preset) {
  switch (preset) {
  case 1:
    return 3200;
  case 2:
    return 6500;
  default:
    return 4500;
  }
}

inline std::optional<int> KelvinFromTemperaturePreset(const std::string_view preset) {
  if (preset == "neutral")
    return 4500;
  if (preset == "warm")
    return 3200;
  if (preset == "cool")
    return 6500;
  return std::nullopt;
}

} // namespace studiocast::video::effects::contract
