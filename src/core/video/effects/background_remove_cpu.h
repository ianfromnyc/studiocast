#pragma once

#include <cstdint>

#include "core/video/effects/effect.h"
#include "core/video/effects/focus_mask_plan.h"

namespace studiocast::video::effects {

// CPU placeholder for "Background Removal".
//
// Like BackgroundBlurCpuEffect, this uses a center-focus mask. Pixels outside
// the focus region are replaced with a solid color (green).
//
// Later, the focus mask will come from AI segmentation (e.g. Maxine).
class BackgroundRemoveCpuEffect final : public IVideoEffect {
public:
  BackgroundRemoveCpuEffect() = default;

  const char *Id() const override { return "background_remove"; }
  const char *DisplayName() const override { return "Background Removal"; }
  const char *Backend() const override { return "cpu"; }

  void Apply(const Rgb24FrameView &frame, EffectContext *ctx) override;

private:
  FocusMaskPlan mask_plan_;
};

} // namespace studiocast::video::effects
