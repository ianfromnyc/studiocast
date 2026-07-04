#pragma once

#include <map>
#include <string>
#include <vector>

namespace studiocast::open_cuda {

struct OpenCudaDiagnostics {
  bool ok = false;

  // Best-effort ONNX Runtime details (cheap provider/runtime discovery only).
  std::string onnxruntime_version;
  std::vector<std::string> onnxruntime_providers;
  bool onnxruntime_cuda_provider_present = false;
  bool onnxruntime_tensorrt_provider_present = false;
  bool onnxruntime_cpu_provider_present = false;
  bool onnxruntime_cuda_ep_v2_build = false;
  std::string onnxruntime_library_path;

  // Best-effort CUDA driver/context probe.
  bool cuda_driver_api_available = false;
  bool cuda_context_available = false;
  int cuda_device_count = -1;
  int cuda_driver_version = 0;
  std::string cuda_driver_error;
  std::string cuda_context_error;

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
