#include "effect_chain.h"

#include <sstream>

namespace studiocast::video::effects {

void EffectChain::Clear() {
  effects_.clear();
  ctx_.scratch.clear();
}

void EffectChain::Add(std::unique_ptr<IVideoEffect> effect) {
  if (!effect) return;
  effects_.push_back(std::move(effect));
}

void EffectChain::Apply(const Rgb24FrameView& frame) {
  if (!frame.Valid()) return;
  for (auto& fx : effects_) {
    if (fx) fx->Apply(frame, &ctx_);
  }
}

std::string EffectChain::BackendSummary() const {
  if (effects_.empty()) return "";
  std::ostringstream oss;
  bool first = true;
  for (const auto& fx : effects_) {
    if (!fx) continue;
    if (!first) oss << ",";
    first = false;
    oss << fx->Id() << ":" << fx->Backend();
  }
  return oss.str();
}

}  // namespace studiocast::video::effects
