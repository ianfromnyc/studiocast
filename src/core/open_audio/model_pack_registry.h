#pragma once

#include <cstdint>
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
  // Values should use stable effect IDs (see
  // core/audio/effects/broadcast_audio_effect_contract.h).
  std::vector<std::string> effects;

  // Optional metadata for UI / future validation.
  int sample_rate = 16000;
  int channels = 1;

  struct AuxInput {
    // Name of the tensor input.
    std::string name;

    // Optional range mapping for user-facing strength (0..1 normalized) to
    // model domain. Value fed to the model is: min_value + t * (max_value -
    // min_value)
    float min_value = 0.0f;
    float max_value = 1.0f;

    // Optional explicit tensor shape for scalar inputs. Default is [1].
    // The engine currently supports only scalar-shaped aux inputs
    // (product(shape)==1).
    std::vector<int64_t> shape;
  };

  struct OnnxIo {
    // Expected frame size in samples at the model sample rate.
    // For 10ms frames this is sample_rate / 100 (e.g., 160 @ 16k).
    int frame_samples = 0;

    // Optional explicit tensor names for the primary waveform I/O.
    // Empty means 'use session first input/output'.
    std::string audio_input;
    std::string audio_output;

    // Optional state tensor names for streaming models.
    // If empty, the engine will attempt to treat all non-audio inputs/outputs
    // as state.
    std::vector<std::string> state_inputs;
    std::vector<std::string> state_outputs;

    // Optional auxiliary inputs (e.g., strength control).
    bool has_strength_input = false;
    AuxInput strength_input{};
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
  // Scan the given Open Audio models directory (the directory containing
  // <model_id>/...).
  //
  // Any pack that fails to load/validate is recorded in Problems() with a
  // reason string. Valid packs are available via ListModels()/ResolveModel().
  static ModelPackRegistry
  Scan(const std::filesystem::path &open_audio_models_dir);

  // Convenience for scanning the default XDG location.
  static ModelPackRegistry ScanDefault();

  const std::vector<ModelPack> &ListModels() const { return models_; }
  std::optional<ModelPack> ResolveModel(const std::string &id) const;

  // Deterministic default selection.
  // Current behavior:
  //  - Prefer curated defaults for the requested effect when possible.
  //  - Else return first installed model ID (sorted).
  //  - Else return empty string.
  std::string DefaultModelId() const;

  // Deterministic default selection for a specific effect.
  //
  // effect_id should be one of:
  //   - "noise_removal"
  //   - "room_echo_removal"
  //   - "studio_voice"
  //
  // If effect_id is empty, this behaves like DefaultModelId().
  std::string DefaultModelIdForEffect(const std::string &effect_id) const;

  // Key is best-effort model id; if unknown, the directory name is used.
  const std::map<std::string, std::string> &Problems() const {
    return problems_;
  }

private:
  std::filesystem::path root_;
  std::vector<ModelPack> models_;               // sorted by id
  std::map<std::string, std::string> problems_; // sorted by key
};

} // namespace studiocast::open_audio
