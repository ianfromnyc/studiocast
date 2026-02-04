#pragma once

#include <string>

#include "core/maxine/cuda_driver_api.h"
#include "core/maxine/nvcv_types.h"

namespace studiocast::maxine {

// Crop+scale for chunky BGR U8 GPU images using a tiny CUDA kernel.
//
// This is intentionally minimal for Milestone 1 correctness; quality can be
// improved later (e.g. bilinear, lanczos, etc.).
class CudaBgrCropScale {
public:
  bool Initialize(CudaDriverApi* cuda, std::string* error_out);

  // Crop rectangle is expressed in source pixel coordinates.
  // Destination image must already be allocated and be BGR/U8/GPU.
  bool CropScale(const NvCVImage& src_bgr_gpu,
                 NvCVImage* dst_bgr_gpu,
                 float crop_x,
                 float crop_y,
                 float crop_w,
                 float crop_h,
                 CUstream stream,
                 std::string* error_out);

private:
  bool EnsureKernelLoaded(std::string* error_out);

  CudaDriverApi* cuda_ = nullptr; // non-owning
  CUmodule module_ = nullptr;
  CUfunction fn_ = nullptr;
  bool loaded_ = false;
};

}  // namespace studiocast::maxine
