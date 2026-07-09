#include "focus_mask_plan.h"

#include <algorithm>
#include <cstddef>

namespace studiocast::video::effects {

bool FocusMaskPlan::Configure(int width, int height) {
  if (width <= 0 || height <= 0)
    return false;
  if (width == width_ && height == height_)
    return true;

  width_ = width;
  height_ = height;

  left_ = static_cast<int>(width * 0.25);
  right_ = static_cast<int>(width * 0.75);
  top_ = static_cast<int>(height * 0.15);
  bottom_ = static_cast<int>(height * 0.95);
  feather_ = std::max(8, std::min(width, height) / 20);

  x_distances_.resize(static_cast<std::size_t>(width));
  y_distances_.resize(static_cast<std::size_t>(height));

  for (int x = 0; x < width; ++x) {
    int dx = 0;
    if (x < left_)
      dx = left_ - x;
    else if (x > right_)
      dx = x - right_;
    x_distances_[static_cast<std::size_t>(x)] = dx;
  }

  for (int y = 0; y < height; ++y) {
    int dy = 0;
    if (y < top_)
      dy = top_ - y;
    else if (y > bottom_)
      dy = y - bottom_;
    y_distances_[static_cast<std::size_t>(y)] = dy;
  }

  return true;
}

} // namespace studiocast::video::effects
