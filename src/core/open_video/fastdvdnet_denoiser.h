#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "core/open_video/model_pack_registry.h"
#include "core/onnx/ort_session.h"

namespace studiocast::open_video {

// Streaming FastDVDnet video denoiser (ONNX Runtime).
//
// This is used as the preferred open-source implementation for the
// "Video Noise Removal" effect when a compatible Open Video model pack is
// installed.
//
// The expected model signature is:
//   noisy:     [1, 15, H, W] float32 (5 RGB frames concatenated on channels)
//   noise_map: [1, 1,  H, W] float32 (sigma/255)
//   denoised:  [1, 3,  H, W] float32 (RGB in 0..1)
//
// For real-time use we implement a causal approximation of the 5-frame window:
//   [t-2, t-1, t, t, t]
// (future frames are repeated as the current frame).
class FastDvdnetDenoiser {
 public:
  FastDvdnetDenoiser();
  ~FastDvdnetDenoiser();

  FastDvdnetDenoiser(const FastDvdnetDenoiser&) = delete;
  FastDvdnetDenoiser& operator=(const FastDvdnetDenoiser&) = delete;

  // Attempt to initialize the denoiser using the default Open Video model root.
  //
  // The chosen model pack is selected from installed packs under task="video_denoise".
  // If multiple packs are installed, a deterministic preference order is used.
  bool EnsureInitialized(int src_w, int src_h, std::string* error);

  // Reset temporal windowing state (but keep the loaded model/session).
  void ResetTemporalState();

  // Apply denoising in-place on an RGB24 frame.
  //
  // Returns true on success, false on fatal errors (in which case the caller
  // should bypass the effect).
  bool ApplyRgbInPlace(std::uint64_t capture_sequence,
                       std::uint8_t* rgb,
                       int width,
                       int height,
                       std::size_t stride,
                       int strength,
                       std::string* error);

  // Diagnostics.
  bool initialized() const { return initialized_; }
  bool disabled() const { return disabled_; }
  bool using_cpu_fallback() const { return using_cpu_fallback_; }
  const std::string& active_model_id() const { return active_model_id_; }
  const std::filesystem::path& active_model_path() const { return active_model_path_; }
  const std::string& sticky_warning() const { return sticky_warning_; }

 private:
  struct LoadedModel {
    std::string id;
    std::filesystem::path onnx;
    int default_sigma = 25;  // best-effort hint, not required
  };

  static int AlignUp(int v, int align);
  static float Clamp01(float x);

  static std::string ChoosePreferredModelId(const ModelPackRegistry& reg);
  static bool LoadDefaultSigmaFromManifest(const std::filesystem::path& manifest_path, int* out_sigma);
  static bool ResolveModelFromRegistry(const ModelPackRegistry& reg, LoadedModel* out, std::string* error);

  bool EnsureSessionForModel(const LoadedModel& model, std::string* error);
  bool RefreshGeometry(int src_w, int src_h, std::string* error);
  bool DetectIoNames(std::string* error);

  void PreprocessRgbToChwPadded(const std::uint8_t* rgb,
                               int width,
                               int height,
                               std::size_t stride,
                               std::vector<float>* out_chw) const;

  void BuildNoisyTensorFromHistory(std::vector<float>* out_noisy) const;
  void EnsureNoiseMap(float sigma_over_255);
  void PostprocessToRgbInPlace(std::uint8_t* rgb,
                              int width,
                              int height,
                              std::size_t stride) const;

  void DisableAfterFailure(const std::string& why);

  bool initialized_ = false;
  bool disabled_ = false;

  // Model/session state.
  ModelPackRegistry registry_;
  std::string active_model_id_;
  std::filesystem::path active_model_path_;
  int model_default_sigma_ = 25;

  studiocast::onnx::OrtSessionInfo session_info_;
  std::unique_ptr<studiocast::onnx::OrtSession> ort_session_cuda_;
  std::unique_ptr<studiocast::onnx::OrtSession> ort_session_cpu_;
  studiocast::onnx::OrtSession* ort_session_active_ = nullptr;
  bool using_cpu_fallback_ = false;

  std::string noisy_name_;
  std::string noise_map_name_;
  std::string denoised_name_;

  // Geometry for the current session run.
  int proc_w_ = 0;
  int proc_h_ = 0;
  int src_w_ = 0;
  int src_h_ = 0;

  std::vector<int64_t> noisy_shape_;
  std::vector<int64_t> noise_map_shape_;
  std::vector<int64_t> denoised_shape_;

  // Temporal history (preprocessed CHW frames, proc_w_*proc_h_).
  static constexpr int kHistoryFrames = 3;  // t-2, t-1, t
  std::vector<std::vector<float>> history_;
  int history_filled_ = 0;
  int history_write_idx_ = 0;
  std::uint64_t last_capture_sequence_ = 0;
  bool have_last_sequence_ = false;

  // ORT I/O buffers.
  std::vector<float> noisy_tensor_;
  std::vector<float> noise_map_tensor_;
  std::vector<float> denoised_tensor_;
  float last_noise_map_value_ = -1.0f;

  // Scratch for ORT bindings.
  std::vector<studiocast::onnx::OrtSession::RunInput> ort_inputs_;
  std::vector<studiocast::onnx::OrtSession::RunOutput> ort_outputs_;

  // Sticky warning surfaced through the daemon status (best-effort).
  std::string sticky_warning_;
  int runtime_failures_ = 0;
};

}  // namespace studiocast::open_video
