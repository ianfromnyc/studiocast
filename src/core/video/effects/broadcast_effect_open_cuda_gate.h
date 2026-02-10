#pragma once

#include <set>
#include <string>
#include <string_view>

#include "core/open_cuda/open_cuda_diagnostics.h"
#include "core/video/effects/broadcast_effect_contract.h"
#include "core/video/effects/broadcast_effect_rules.h"

namespace studiocast::video::effects {

// Safety-net policy: Open CUDA virtual background failures during application (e.g. missing/corrupt
// model pack, transient runtime/ORT errors) should NOT abort the camera pipeline.
//
// The pipeline should keep producing pass-through frames while surfacing the error to the user.
inline bool ShouldAbortPipelineOnOpenCudaVbApplyFailure() {
  return false;
}

struct OpenCudaGateDecision {
  bool ok = true;
  std::string message;
};

// Returns true when the locally-planned effect chain includes at least one
// Open CUDA-backed effect.
inline bool WantsOpenCudaForPlannedEffects(const BroadcastCameraEffects& fx) {
  const auto plan = BuildBroadcastEffectsPlan(fx);
  const std::set<std::string> planned(plan.ordered_effect_ids.begin(), plan.ordered_effect_ids.end());

  const auto has = [&](std::string_view id) {
    return planned.count(std::string(id)) != 0;
  };

  return has(contract::kEffectIdVirtualBackgroundBlur) ||
         has(contract::kEffectIdVirtualBackgroundRemove) ||
         has(contract::kEffectIdVirtualBackgroundReplace);
}

// Evaluates whether the Open CUDA-backed effects currently enabled by local
// rules are runnable on this system (as described by `OpenCudaDiagnostics`).
//
// This is a pure decision helper: it does not probe the system.
inline OpenCudaGateDecision EvaluateOpenCudaGate(const BroadcastCameraEffects& fx,
                                                 const studiocast::open_cuda::OpenCudaDiagnostics& diag) {
  OpenCudaGateDecision out;

  const auto plan = BuildBroadcastEffectsPlan(fx);
  const std::set<std::string> planned(plan.ordered_effect_ids.begin(), plan.ordered_effect_ids.end());

  const auto has = [&](std::string_view id) {
    return planned.count(std::string(id)) != 0;
  };

  const bool wants_vb =
      has(contract::kEffectIdVirtualBackgroundBlur) ||
      has(contract::kEffectIdVirtualBackgroundRemove) ||
      has(contract::kEffectIdVirtualBackgroundReplace);
  if (!wants_vb) {
    return out;
  }

  const std::set<std::string> avail(diag.available_effects.begin(), diag.available_effects.end());

  auto format_blocked = [&](std::string_view effect_id) {
    std::string msg = "Open CUDA backend blocked.";
    const auto it = diag.blocked_effects.find(std::string(effect_id));
    if (it != diag.blocked_effects.end()) {
      msg += " " + std::string(effect_id) + " unavailable (" + it->second + ").";
    } else {
      msg += " " + std::string(effect_id) + " unavailable.";
    }
    for (const auto& h : diag.install_hints) {
      if (!h.empty()) msg += "\n" + h;
    }
    return msg;
  };

  if (has(contract::kEffectIdVirtualBackgroundBlur) &&
      !avail.count(std::string(contract::kEffectIdVirtualBackgroundBlur))) {
    out.ok = false;
    out.message = format_blocked(contract::kEffectIdVirtualBackgroundBlur);
    return out;
  }
  if (has(contract::kEffectIdVirtualBackgroundRemove) &&
      !avail.count(std::string(contract::kEffectIdVirtualBackgroundRemove))) {
    out.ok = false;
    out.message = format_blocked(contract::kEffectIdVirtualBackgroundRemove);
    return out;
  }
  if (has(contract::kEffectIdVirtualBackgroundReplace) &&
      !avail.count(std::string(contract::kEffectIdVirtualBackgroundReplace))) {
    out.ok = false;
    out.message = format_blocked(contract::kEffectIdVirtualBackgroundReplace);
    return out;
  }

  return out;
}

}  // namespace studiocast::video::effects
