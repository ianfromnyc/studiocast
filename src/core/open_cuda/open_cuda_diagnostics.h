#pragma once

#include <map>
#include <string>
#include <vector>

namespace studiocast::open_cuda {

struct OpenCudaDiagnostics {
  bool ok = false;

  // Model pack IDs discovered/usable by this engine.
  std::vector<std::string> installed_models;

  // model_id -> reason string (stable-ish; human-readable for now).
  std::map<std::string, std::string> missing_models;

  // Stable effect IDs (see `core/video/effects/broadcast_effect_contract.h`).
  std::vector<std::string> available_effects;

  // effect_id -> reason_code string.
  std::map<std::string, std::string> blocked_effects;

  // Human-actionable strings.
  std::vector<std::string> install_hints;

  std::string ToJson() const;
};

}  // namespace studiocast::open_cuda
