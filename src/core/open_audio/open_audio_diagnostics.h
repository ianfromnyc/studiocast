#pragma once

#include <map>
#include <string>
#include <vector>

namespace studiocast::open_audio {

struct OpenAudioDiagnostics {
  bool ok = false;

  // Best-effort ONNX Runtime details (useful for debugging provider
  // availability).
  std::string onnxruntime_version;
  std::vector<std::string> onnxruntime_providers;
  bool onnxruntime_cuda_provider_present = false;
  bool onnxruntime_tensorrt_provider_present = false;
  bool onnxruntime_cpu_provider_present = false;
  bool onnxruntime_cuda_ep_v2_build = false;
  std::string onnxruntime_library_path;
  std::vector<std::string> onnxruntime_warnings;

  // Best-effort expected execution mode for Open Audio model sessions.
  std::string acceleration_likely;

  // Model pack IDs discovered/usable by this engine.
  std::vector<std::string> installed_models;

  struct ModelInfo {
    std::string id;
    std::string display_name;
    std::vector<std::string> effects;
    int sample_rate = 0;
    int channels = 0;
  };
  std::vector<ModelInfo> models;

  // Deterministic default model id computed by the daemon.
  std::string default_model_id;

  // model_id -> reason string.
  std::map<std::string, std::string> missing_models;

  // Stable effect IDs.
  std::vector<std::string> available_effects;

  // effect_id -> reason_code string.
  std::map<std::string, std::string> blocked_effects;

  // Human-actionable strings.
  std::vector<std::string> install_hints;

  std::string ToJson() const;
};

} // namespace studiocast::open_audio
