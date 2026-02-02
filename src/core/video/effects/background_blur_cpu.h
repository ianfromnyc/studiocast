#pragma once

#include <cstdint>
#include <vector>

#include "core/video/effects/effect.h"

namespace studiocast::video::effects {

// CPU placeholder for "Background Blur".
//
// Today this uses a simple center-focus mask:
//   - the center region is left sharp
//   - the outer region is blurred
//
// Later, the mask will come from an AI segmentation model (e.g. Maxine).
class BackgroundBlurCpuEffect final : public IVideoEffect {
 public:
  explicit BackgroundBlurCpuEffect(int strength);

  const char* Id() const override { return "background_blur"; }
  const char* DisplayName() const override { return "Background Blur"; }
  const char* Backend() const override { return "cpu"; }

  void Apply(const Rgb24FrameView& frame, EffectContext* ctx) override;

 private:
  int strength_ = 8;  // blur radius

  std::vector<std::uint8_t> blurred_;   // tight RGB24 copy
  std::vector<std::uint8_t> scratch_;   // used by box blur
};

}  // namespace studiocast::video::effects
