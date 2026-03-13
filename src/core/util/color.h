#pragma once

#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>

namespace studiocast::util::color {

enum class HexColorHashMode {
  required,
  optional,
};

struct HexColorParseOptions {
  HexColorHashMode hash_mode = HexColorHashMode::required;
  std::string_view expected_format_error = "expected '#RRGGBB'";
  std::string_view invalid_hex_error = "invalid hex digit";
};

inline bool ParseHexColorRgb(std::string_view s, std::uint32_t *out_rgb,
                             std::string *error,
                             HexColorParseOptions options = {}) {
  if (!out_rgb)
    return false;

  *out_rgb = 0;
  std::string_view value = s;
  if (options.hash_mode == HexColorHashMode::required) {
    if (value.size() != 7 || value.front() != '#') {
      if (error)
        *error = std::string(options.expected_format_error);
      return false;
    }
    value.remove_prefix(1);
  } else {
    if (!value.empty() && value.front() == '#')
      value.remove_prefix(1);
    if (value.size() != 6) {
      if (error)
        *error = std::string(options.expected_format_error);
      return false;
    }
  }

  std::uint32_t rgb = 0;
  for (const char c : value) {
    std::uint32_t nibble = 0;
    if (c >= '0' && c <= '9')
      nibble = static_cast<std::uint32_t>(c - '0');
    else if (c >= 'a' && c <= 'f')
      nibble = static_cast<std::uint32_t>(10 + (c - 'a'));
    else if (c >= 'A' && c <= 'F')
      nibble = static_cast<std::uint32_t>(10 + (c - 'A'));
    else {
      if (error)
        *error = std::string(options.invalid_hex_error);
      return false;
    }
    rgb = (rgb << 4u) | nibble;
  }

  *out_rgb = rgb;
  return true;
}

inline std::string RgbToHexColor(std::uint32_t rgb) {
  std::ostringstream oss;
  oss << '#';
  oss << std::hex << std::setw(6) << std::setfill('0') << (rgb & 0x00ffffffu);
  return oss.str();
}

} // namespace studiocast::util::color
