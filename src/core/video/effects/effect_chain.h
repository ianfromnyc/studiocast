#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/video/effects/effect.h"

namespace studiocast::video::effects {

class EffectChain final {
 public:
  EffectChain() = default;
  ~EffectChain() = default;

  EffectChain(const EffectChain&) = delete;
  EffectChain& operator=(const EffectChain&) = delete;

  void Clear();

  void Add(std::unique_ptr<IVideoEffect> effect);

  // Applies all effects in order.
  void Apply(const Rgb24FrameView& frame);

  // Debug/status helpers.
  std::string BackendSummary() const;

 private:
  std::vector<std::unique_ptr<IVideoEffect>> effects_;
  mutable EffectContext ctx_;
};

}  // namespace studiocast::video::effects
