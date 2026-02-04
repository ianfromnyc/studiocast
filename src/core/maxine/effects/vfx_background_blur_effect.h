#pragma once

#include <filesystem>
#include <string>

#include "core/maxine/effects/maxine_effect.h"
#include "core/maxine/nvcv_api.h"
#include "core/maxine/vfx_api.h"

namespace studiocast::maxine::effects {

// Wrapper for Maxine VFX "Background Blur" effect.
//
// VFX docs:
//   - Input:  BGRu8 chunky (srcImage)
//   - Input:  Au8 chunky (matte)
//   - Output: BGRu8 chunky (dstImage)
//   - Parameter: NVVFX_STRENGTH in [0..1]
class VfxBackgroundBlurEffect final : public IVfxEffect {
 public:
  struct Config {
    float strength = 0.5f;  // [0..1]
  };

  VfxBackgroundBlurEffect(maxine::vfx::VfxApi* vfx,
                          maxine::NvcvApi* nvcv,
                          std::filesystem::path model_dir);
  ~VfxBackgroundBlurEffect() override;

  VfxBackgroundBlurEffect(const VfxBackgroundBlurEffect&) = delete;
  VfxBackgroundBlurEffect& operator=(const VfxBackgroundBlurEffect&) = delete;

  const char* Id() const override { return "background_blur"; }
  const char* DisplayName() const override { return "Background Blur"; }

  // Initializes the underlying NvVFX effect (lazy; also called by Process).
  bool Initialize(std::string* error);

  bool Configure(const studiocast::video::CameraEffects& settings, std::string* error) override;

  // Runs background blur on `frame.nvcv_gpu` using `frame.matte_gpu`.
  // Output is stored in an internal GPU `NvCVImage`.
  NvCV_Status Process(studiocast::video::GpuFrame& frame, std::string* error) override;

  // Most recent output image (GPU). Valid after a successful Process.
  const maxine::NvCVImage* OutputGpu() const { return output_ready_ ? &output_gpu_ : nullptr; }

  maxine::CUstream cuda_stream() const { return stream_; }

  const Config& config() const { return cfg_; }
  void SetConfig(const Config& cfg);

 private:
  bool EnsureEffectCreated(std::string* error);
  bool ApplyConfigLocked(std::string* error);
  bool EnsureOutputImage(unsigned width, unsigned height, std::string* error);
  bool BindMatte(const maxine::NvCVImage* matte, std::string* error);

  void Destroy();

  maxine::vfx::VfxApi* vfx_ = nullptr;
  maxine::NvcvApi* nvcv_ = nullptr;
  std::filesystem::path model_dir_;

  maxine::vfx::NvVFX_Handle handle_ = nullptr;
  maxine::CUstream stream_ = nullptr;
  bool own_stream_ = false;

  Config cfg_{};
  bool cfg_dirty_ = true;

  maxine::NvCVImage output_gpu_{};
  bool output_ready_ = false;
  bool output_allocated_ = false;
};

}  // namespace studiocast::maxine::effects
