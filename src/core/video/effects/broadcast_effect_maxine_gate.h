#pragma once

#include <set>
#include <string>
#include <string_view>

#include "core/maxine/availability.h"
#include "core/maxine/maxine_manager.h"
#include "core/video/effects/broadcast_effect_contract.h"
#include "core/video/effects/broadcast_effect_rules.h"

namespace studiocast::video::effects {

struct MaxineGateDecision {
  bool ok = true;
  studiocast::maxine::MaxineNeed need = studiocast::maxine::MaxineNeed::any;
  std::string message;
};

// Returns true when the locally-planned effect chain includes at least one
// Maxine-backed effect.
inline bool WantsMaxineForPlannedEffects(const BroadcastCameraEffects& fx) {
  const auto plan = BuildBroadcastEffectsPlan(fx);
  const std::set<std::string> planned(plan.ordered_effect_ids.begin(), plan.ordered_effect_ids.end());

  const auto has = [&](std::string_view id) {
    return planned.count(std::string(id)) != 0;
  };

  return has(contract::kEffectIdVirtualBackgroundBlur) ||
         has(contract::kEffectIdVirtualBackgroundRemove) ||
         has(contract::kEffectIdVirtualBackgroundReplace) ||
         has(contract::kEffectIdVideoNoiseRemoval) ||
         has(contract::kEffectIdVirtualKeyLight) ||
         has(contract::kEffectIdEyeContact) ||
         has(contract::kEffectIdAutoFrame);
}

// Evaluates whether the Maxine-backed effects currently enabled by local rules
// are runnable on this system (as described by `MaxineDiagnostics`).
//
// This is a pure decision helper: it does not probe the system.
inline MaxineGateDecision EvaluateMaxineGate(const BroadcastCameraEffects& fx,
                                             const studiocast::maxine::MaxineDiagnostics& diag) {
  MaxineGateDecision out;

  const auto plan = BuildBroadcastEffectsPlan(fx);
  const std::set<std::string> planned(plan.ordered_effect_ids.begin(), plan.ordered_effect_ids.end());

  const auto has = [&](std::string_view id) {
    return planned.count(std::string(id)) != 0;
  };

  const bool wants_vfx =
      has(contract::kEffectIdVirtualBackgroundBlur) ||
      has(contract::kEffectIdVirtualBackgroundRemove) ||
      has(contract::kEffectIdVirtualBackgroundReplace) ||
      has(contract::kEffectIdVideoNoiseRemoval) ||
      has(contract::kEffectIdVirtualKeyLight);

  const bool wants_ar = has(contract::kEffectIdEyeContact) || has(contract::kEffectIdAutoFrame);

  if (!wants_vfx && !wants_ar) {
    return out;
  }

  const std::set<std::string> avail(diag.available_effects.begin(), diag.available_effects.end());

  const auto set_blocked = [&](studiocast::maxine::MaxineNeed need) {
    out.ok = false;
    out.need = need;
    const auto c = studiocast::maxine::BuildCanonicalMaxineBlockedCopy(diag, need);
    out.message = studiocast::maxine::FormatCanonicalMaxineBlockedCopy(c);
    if (out.message.empty()) {
      out.message = c.summary;
    }
  };

  // AR effects.
  if (has(contract::kEffectIdEyeContact) &&
      !avail.count(std::string(contract::kEffectIdEyeContact))) {
    set_blocked(studiocast::maxine::MaxineNeed::ar);
    return out;
  }
  if (has(contract::kEffectIdAutoFrame) && !avail.count(std::string(contract::kEffectIdAutoFrame))) {
    set_blocked(studiocast::maxine::MaxineNeed::ar);
    return out;
  }

  // VFX effects.
  if (has(contract::kEffectIdVirtualBackgroundBlur) &&
      !avail.count(std::string(contract::kEffectIdVirtualBackgroundBlur))) {
    set_blocked(studiocast::maxine::MaxineNeed::vfx);
    return out;
  }
  if (has(contract::kEffectIdVirtualBackgroundRemove) &&
      !avail.count(std::string(contract::kEffectIdVirtualBackgroundRemove))) {
    set_blocked(studiocast::maxine::MaxineNeed::vfx);
    return out;
  }
  if (has(contract::kEffectIdVirtualBackgroundReplace) &&
      !avail.count(std::string(contract::kEffectIdVirtualBackgroundReplace))) {
    set_blocked(studiocast::maxine::MaxineNeed::vfx);
    return out;
  }
  if (has(contract::kEffectIdVideoNoiseRemoval) &&
      !avail.count(std::string(contract::kEffectIdVideoNoiseRemoval))) {
    set_blocked(studiocast::maxine::MaxineNeed::vfx);
    return out;
  }
  if (has(contract::kEffectIdVirtualKeyLight) &&
      !avail.count(std::string(contract::kEffectIdVirtualKeyLight))) {
    set_blocked(studiocast::maxine::MaxineNeed::vfx);
    return out;
  }

  return out;
}

}  // namespace studiocast::video::effects
