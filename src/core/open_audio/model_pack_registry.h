#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace studiocast::open_audio {

struct ModelPack {
  // Schema v1
  std::string id;
  std::string display_name;
  std::string onnx_filename;

  // Optional: which effects this model claims to support.
  // Values should use stable effect IDs (see core/audio/effects/broadcast_audio_effect_contract.h).
  std::vector<std::string> effects;

  // Optional metadata for UI / future validation.
  int sample_rate = 16000;
  int channels = 1;

  struct OnnxIo {
    // Expected frame size in samples at the model sample rate.
    // For 10ms frames this is sample_rate / 100 (e.g., 160 @ 16k).
    int frame_samples = 0;

    // Optional explicit tensor names for the primary waveform I/O.
    // Empty means 'use session first input/output'.
    std::string audio_input;
    std::string audio_output;

    // Optional state tensor names for streaming models.
    // If empty, the engine will attempt to treat all non-audio inputs/outputs as state.
    std::vector<std::string> state_inputs;
    std::vector<std::string> state_outputs;
  };

  bool has_onnx_io = false;
  OnnxIo onnx_io{};

  // Derived from install layout
  std::filesystem::path root_dir;
  std::filesystem::path manifest_path;
  std::filesystem::path onnx_path;
  std::optional<std::filesystem::path> license_path;
};

// Registry for model packs under:
//   <models_root>/open_audio/<model_id>/
// where <models_root> is normally ~/.local/share/studiocast/models.
class ModelPackRegistry {
 public:
  // Scan the given Open Audio models directory (the directory containing <model_id>/...).
  //
  // Any pack that fails to load/validate is recorded in Problems() with a reason string.
  // Valid packs are available via ListModels()/ResolveModel().
  static ModelPackRegistry Scan(const std::filesystem::path& open_audio_models_dir);

  // Convenience for scanning the default XDG location.
  static ModelPackRegistry ScanDefault();

  const std::vector<ModelPack>& ListModels() const { return models_; }
  std::optional<ModelPack> ResolveModel(const std::string& id) const;

  // Deterministic default selection.
  // Current behavior:
  //  - Return first installed model ID (sorted).
  //  - Else return empty string.
  std::string DefaultModelId() const;

  // Key is best-effort model id; if unknown, the directory name is used.
  const std::map<std::string, std::string>& Problems() const { return problems_; }

 private:
  std::filesystem::path root_;
  std::vector<ModelPack> models_;                // sorted by id
  std::map<std::string, std::string> problems_;  // sorted by key
};

}  // namespace studiocast::open_audio
