#pragma once

#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>

#include "core/maxine/afx_api.h"

namespace studiocast::maxine::afx {

namespace fs = std::filesystem;

struct AfxEffectConfig {
  // NvAFX effect selector passed to `NvAFX_CreateEffect`.
  // Examples: "denoiser", "dereverb", "dereverb_denoiser",
  // "studio_voice_low_latency".
  std::string effect_selector;

  // Feature ID used to locate feature assets under
  // `<AFX_ROOT>/features/<feature_id>/...`. Examples: "denoiser", "dereverb",
  // "dereverb_denoiser", "studio_voice".
  std::string feature_id;

  // Absolute path to `<AFX_ROOT>/features`.
  fs::path features_dir;

  // Optional explicit model file path. If empty, StudioCast resolves the model
  // path using the feature layout and the selected GPU's compute capability.
  fs::path model_path;

  // Sample format assumptions for MVP: float PCM.
  int sample_rate = 48000;
  std::uint32_t frame_samples = 480;
  std::uint32_t channels = 1;

  // Effect intensity (implementation-defined by AFX), typically 0..1.
  float intensity = 0.5f;

  // Selected GPU compute capability (major, minor). Required when `model_path`
  // is empty.
  std::optional<std::pair<int, int>> compute_capability;

  // Optional effect versioning and VAD controls (effect-dependent).
  std::optional<std::uint32_t> effect_version;
  bool vad_enabled = false;

  // Optional AFX denoiser v2 model selection.
  bool use_denoiser_v2_model = false;
};

struct PlannedAfxMicrophoneEffect {
  bool enabled = false;
  std::string effect_selector;
  std::string feature_id;
  float intensity = 0.5f;
  bool use_denoiser_v2_model = false;
};

// Enforces Broadcast-equivalent microphone rules:
//  - If Studio Voice is enabled: select `studio_voice_low_latency` and disable
//  denoiser/dereverb.
//  - Else: if noise+echo: use `dereverb_denoiser`.
//  - Else: use `denoiser` or `dereverb`.
//  - A single `strength` value (0..100-ish) is mapped to a single intensity.
PlannedAfxMicrophoneEffect
PlanBroadcastMicrophoneEffect(bool studio_voice_enabled,
                              bool noise_removal_enabled,
                              bool room_echo_removal_enabled, int strength);

class AfxEffect {
public:
  explicit AfxEffect(AfxApi *api);
  ~AfxEffect();

  AfxEffect(const AfxEffect &) = delete;
  AfxEffect &operator=(const AfxEffect &) = delete;

  AfxEffect(AfxEffect &&) = delete;
  AfxEffect &operator=(AfxEffect &&) = delete;

  void SetApi(AfxApi *api) { api_ = api; }

  bool IsConfigured() const { return configured_; }
  bool IsLoaded() const { return loaded_; }

  const AfxEffectConfig &config() const { return cfg_; }
  const fs::path &resolved_model_path() const { return resolved_model_path_; }

  // Validates config and resolves the model path (if not explicitly provided).
  bool Configure(const AfxEffectConfig &cfg, std::string *error_out);

  // Creates the effect handle, sets parameters, and calls `NvAFX_Load`.
  bool Load(std::string *error_out);

  // Update intensity on a loaded effect without recreating the pipeline.
  // Non-fatal; returns false if the parameter could not be set on the current
  // effect.
  bool UpdateIntensity(float intensity, std::string *error_out);

  // Runs one frame. Input/output are float PCM; `num_samples` is the number
  // of samples in `input` and `output`.
  bool Run(const float *input, float *output, std::uint32_t num_samples,
           std::string *error_out);

  void Destroy();

private:
  bool SetU32Any(NvAFX_Handle handle, const char *what,
                 std::initializer_list<const char *> candidates,
                 std::uint32_t v, std::string *error_out);
  bool SetFloatAny(NvAFX_Handle handle, const char *what,
                   std::initializer_list<const char *> candidates, float v,
                   std::string *error_out);
  bool SetStringAny(NvAFX_Handle handle, const char *what,
                    std::initializer_list<const char *> candidates,
                    const std::string &v, std::string *error_out);

  AfxApi *api_ = nullptr;
  NvAFX_Handle handle_ = nullptr;

  bool configured_ = false;
  bool loaded_ = false;

  AfxEffectConfig cfg_{};
  fs::path resolved_model_path_;
  fs::path resolved_feature_lib_dir_;
  fs::path resolved_feature_lib_path_;
};

} // namespace studiocast::maxine::afx
