#include "effect_types.h"

#include <algorithm>

namespace studiocast::video::effects {
namespace {

std::string ToLowerAscii(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
    if (c >= 'A' && c <= 'Z') return static_cast<char>(c - 'A' + 'a');
    return static_cast<char>(c);
  });
  return s;
}

}  // namespace

std::string ToString(BackgroundEffect v) {
  switch (v) {
    case BackgroundEffect::none:
      return "none";
    case BackgroundEffect::blur:
      return "blur";
    case BackgroundEffect::remove:
      return "remove";
    case BackgroundEffect::auto_frame:
      return "auto_frame";
    case BackgroundEffect::replace:
      return "replace";
  }
  return "none";
}

std::string ToString(EffectBackend v) {
  switch (v) {
    case EffectBackend::auto_select:
      return "auto";
    case EffectBackend::cpu:
      return "cpu";
    case EffectBackend::maxine:
      return "maxine";
  }
  return "auto";
}

bool ParseBackgroundEffect(const std::string& s, BackgroundEffect* out) {
  if (!out) return false;
  const auto v = ToLowerAscii(s);

  if (v.empty() || v == "none" || v == "off" || v == "disabled") {
    *out = BackgroundEffect::none;
    return true;
  }
  if (v == "blur" || v == "background_blur" || v == "bg_blur") {
    *out = BackgroundEffect::blur;
    return true;
  }
  if (v == "remove" || v == "removal" || v == "background_remove" || v == "bg_remove") {
    *out = BackgroundEffect::remove;
    return true;
  }
  if (v == "replace" || v == "background_replace" || v == "bg_replace") {
    *out = BackgroundEffect::replace;
    return true;
  }
  if (v == "auto_frame" || v == "autoframe" || v == "auto") {
    *out = BackgroundEffect::auto_frame;
    return true;
  }

  return false;
}

bool ParseEffectBackend(const std::string& s, EffectBackend* out) {
  if (!out) return false;
  const auto v = ToLowerAscii(s);

  if (v.empty() || v == "auto" || v == "default") {
    *out = EffectBackend::auto_select;
    return true;
  }
  if (v == "cpu" || v == "software") {
    *out = EffectBackend::cpu;
    return true;
  }
  if (v == "maxine" || v == "gpu" || v == "nvidia") {
    *out = EffectBackend::maxine;
    return true;
  }

  return false;
}

}  // namespace studiocast::video::effects
