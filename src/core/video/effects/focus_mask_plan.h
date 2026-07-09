#pragma once

#include <vector>

namespace studiocast::video::effects {

class FocusMaskPlan final {
public:
  bool Configure(int width, int height);

  int width() const { return width_; }
  int height() const { return height_; }
  int feather() const { return feather_; }
  int focus_left() const { return left_; }
  int focus_right() const { return right_; }

  const std::vector<int> &x_distances() const { return x_distances_; }
  const std::vector<int> &y_distances() const { return y_distances_; }

private:
  int width_ = 0;
  int height_ = 0;
  int left_ = 0;
  int right_ = 0;
  int top_ = 0;
  int bottom_ = 0;
  int feather_ = 0;
  std::vector<int> x_distances_;
  std::vector<int> y_distances_;
};

} // namespace studiocast::video::effects
