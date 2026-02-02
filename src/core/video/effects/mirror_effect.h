#pragma once

#include "core/video/effects/effect.h"

namespace studiocast::video::effects {

class MirrorEffect final : public IVideoEffect {
 public:
  const char* Id() const override { return "mirror"; }
  const char* DisplayName() const override { return "Mirror"; }

  void Apply(const Rgb24FrameView& frame, EffectContext* ctx) override;
};

}  // namespace studiocast::video::effects
