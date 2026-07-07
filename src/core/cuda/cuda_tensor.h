#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "core/maxine/cuda_driver_api.h"

namespace studiocast::cuda {

// Minimal CUDA tensor buffer for model inputs/outputs.
//
// - Maxine-independent.
// - Uses the CUDA Driver API surface loaded by CudaDriverApi.
// - Allocation uses cuMemAllocPitch with Height=1, so the tensor is contiguous
//   for `bytes` active bytes, but may have extra trailing capacity.
struct CudaTensor {
  studiocast::maxine::CUdeviceptr ptr = 0;
  std::size_t pitch =
      0; // bytes between rows in the underlying allocation (Height=1)
  std::size_t bytes = 0; // active bytes used by this tensor

  int n = 0;
  int c = 0;
  int h = 0;
  int w = 0;

  bool owns_memory = false;

  bool Valid() const;
  std::size_t ElementCount() const;

  bool AllocateNchwF32(studiocast::maxine::CudaDriverApi *cuda, int n_in,
                       int c_in, int h_in, int w_in, std::string *error_out);

  bool Free(studiocast::maxine::CudaDriverApi *cuda, std::string *error_out);

  bool ReallocIfNeededNchwF32(studiocast::maxine::CudaDriverApi *cuda, int n_in,
                              int c_in, int h_in, int w_in,
                              std::string *error_out);

  bool UploadFromCpuF32(studiocast::maxine::CudaDriverApi *cuda,
                        const float *src, std::size_t src_floats,
                        studiocast::maxine::CUstream stream,
                        std::string *error_out) const;

  bool DownloadToCpuF32(studiocast::maxine::CudaDriverApi *cuda,
                        std::vector<float> *out,
                        studiocast::maxine::CUstream stream,
                        std::string *error_out) const;
};

} // namespace studiocast::cuda
