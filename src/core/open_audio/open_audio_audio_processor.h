#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include "core/audio/audio_processor.h"
#include "core/audio/effects/broadcast_audio_effects.h"

namespace studiocast::open_audio {

// Resolved Open Audio model selection for a given effects config.
//
// In Phase 4 this is used to validate model selection and provide a stub processor
// that keeps the end-to-end backend switching logic working.
//
// In later phases this will also drive the ONNX Runtime session creation.
struct ResolvedOpenAudioModel {
  // Stable model pack ID when resolved from the registry.
  // When resolved from a user-provided path, this may be empty.
  std::string model_id;

  // Human-friendly name (best-effort).
  std::string display_name;

  // Fully resolved ONNX file path.
  std::filesystem::path onnx_path;

  // Indicates whether the ONNX file came from a user-specified path.
  bool is_user_path = false;
};

// Resolve the Open Audio model selection for the microphone effects.
//
// Resolution order:
//  1) microphone.model_path (file .onnx or directory containing model.json)
//  2) microphone.model_id (installed pack id)
//  3) default installed pack id
//
// Returns false with an actionable error string if no model can be resolved.
bool ResolveOpenAudioModelForMicrophone(const studiocast::audio::effects::BroadcastAudioEffects& fx,
                                       ResolvedOpenAudioModel* out,
                                       std::string* error);

// Open Audio processor (Phase 4 stub).
//
// For Phase 4 this processor is pass-through, but it validates model selection
// and exposes the resolved model path for status/UI.
class OpenAudioAudioProcessor final : public studiocast::audio::AudioProcessor {
 public:
  // Creates a processor for microphone effects.
  // Returns nullptr and fills error if model selection cannot be resolved.
  static std::unique_ptr<OpenAudioAudioProcessor> CreateForMicrophone(
      const studiocast::audio::effects::BroadcastAudioEffects& fx,
      ResolvedOpenAudioModel* resolved_out,
      std::string* error);

  explicit OpenAudioAudioProcessor(ResolvedOpenAudioModel model);

  const ResolvedOpenAudioModel& model() const { return model_; }

  bool Process(const float* in,
               float* out,
               std::uint32_t frames,
               std::uint32_t channels,
               std::string* error) override;

 private:
  ResolvedOpenAudioModel model_;
};

}  // namespace studiocast::open_audio
