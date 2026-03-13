#pragma once

#include <algorithm>

namespace studiocast::util {

inline int ClampInt(int v, int lo, int hi) { return std::clamp(v, lo, hi); }

inline float ClampFloat(float v, float lo, float hi) {
  return std::clamp(v, lo, hi);
}

} // namespace studiocast::util
