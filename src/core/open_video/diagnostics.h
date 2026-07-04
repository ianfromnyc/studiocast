#pragma once

#include <map>
#include <string>
#include <vector>

namespace studiocast::open_cuda {

struct OpenCudaDiagnostics {
  bool ok = false;

  // ONNX Runtime provider diagnostics (additive JSON fields).
  std::string ort_version;
  std::vector<std::string> ort_providers;

  bool tensorrt_supported = false;
  bool tensorrt_available = false;
  bool tensorrt_requested = false;
  std::string tensorrt_cache_path;
  std::string tensorrt_status;

  // Model pack IDs discovered/usable by this engine.
  std::vector<std::string> installed_models;

  // Rich model pack metadata for UI (Open CUDA only).
  struct ModelInfo {
    std::string id;
    std::string display_name;
    std::string task;
    int width = 0;
    int height = 0;
  };
  std::vector<ModelInfo> models;

  // Deterministic default model id computed by the daemon.
  std::string default_model_id;

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

} // namespace studiocast::open_cuda
