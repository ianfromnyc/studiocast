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
    // User-facing diagnostics: avoid implying CPU effect backends are supported.
    // Some lightweight tail operations (e.g. `mirror`) are still host-side, but
    // we present them as "builtin" rather than "cpu".
    const std::string backend = fx->Backend() ? std::string(fx->Backend()) : std::string();
    oss << fx->Id() << ":" << ((backend == "cpu") ? "builtin" : backend);
  }
  return oss.str();
}

}  // namespace studiocast::video::effects
