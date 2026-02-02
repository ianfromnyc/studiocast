#pragma once

#include <string>

namespace studiocast::video::effects {

// Video background effects are the hallmark of NVIDIA Broadcast.
//
// StudioCast uses these enums as the stable "shape" of the feature.
// Implementations can be CPU placeholders today, and Maxine/GPU later.

enum class BackgroundEffect {
  none = 0,
  blur = 1,
  remove = 2,
  auto_frame = 3,
};

// Backend preference for an effect.
//
// - auto_select: choose Maxine if available, otherwise CPU
// - cpu: force CPU path
// - maxine: force Maxine path (falls back if unavailable)

enum class EffectBackend {
  auto_select = 0,
  cpu = 1,
  maxine = 2,
};

std::string ToString(BackgroundEffect v);
std::string ToString(EffectBackend v);

// Parsing helpers (case-insensitive). Returns false if the value is unknown.
bool ParseBackgroundEffect(const std::string& s, BackgroundEffect* out);
bool ParseEffectBackend(const std::string& s, EffectBackend* out);

}  // namespace studiocast::video::effects
