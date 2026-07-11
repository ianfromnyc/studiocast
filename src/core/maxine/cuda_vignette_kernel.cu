// Reference CUDA C source for the embedded Driver API PTX module in
// cuda_vignette.cpp. This file is compiled only by the PTX freshness validation
// test; StudioCast's normal build uses the embedded PTX string.
//
// PTX_GENERATE: nvcc -ptx -O3 -arch=compute_52 -I src src/core/maxine/cuda_vignette_kernel.cu -o cuda_vignette_kernel.ptx

#include <cuda_runtime.h>

namespace {

__device__ __forceinline__ float Clamp01(float v) {
  return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

__device__ __forceinline__ unsigned char RoundNearestToU8(float v) {
  v = v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v);
  const unsigned int iv = __float2uint_rn(v);
  return static_cast<unsigned char>(iv > 255u ? 255u : iv);
}

} // namespace

extern "C" __global__ void vignette_bgr_u8(
    unsigned char *ptr, unsigned int pitch, unsigned int w, unsigned int h,
    float intensity, float center_x, float center_y, float inv_half_w,
    float inv_half_h) {
  const int x = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
  const int y = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
  if (x >= static_cast<int>(w) || y >= static_cast<int>(h))
    return;

  const float fx =
      (static_cast<float>(x) + 0.5f - center_x) * inv_half_w;
  const float fy =
      (static_cast<float>(y) + 0.5f - center_y) * inv_half_h;
  const float radius = Clamp01(sqrtf(fx * fx + fy * fy) * 0.70710677f);
  float factor = 1.0f - intensity * radius * radius;
  factor = factor < 0.0f ? 0.0f : factor;

  auto *px = ptr + static_cast<unsigned int>(y) * pitch +
             static_cast<unsigned int>(x) * 3u;
  px[0] = RoundNearestToU8(static_cast<float>(px[0]) * factor);
  px[1] = RoundNearestToU8(static_cast<float>(px[1]) * factor);
  px[2] = RoundNearestToU8(static_cast<float>(px[2]) * factor);
}
