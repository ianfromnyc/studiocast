// Reference CUDA C source for the embedded Driver API PTX modules in
// cuda_crop_scale.cpp. This file is compiled only by the PTX freshness
// validation test; StudioCast's normal build uses the embedded PTX strings.
//
// PTX_GENERATE: nvcc -ptx -O3 -arch=compute_52 -I src src/core/maxine/cuda_crop_scale_kernels.cu -o cuda_crop_scale_kernels.ptx

#include <cuda_runtime.h>

namespace {

__device__ __forceinline__ int ClampInt(int v, int lo, int hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

__device__ __forceinline__ unsigned char RoundClampHalfUpToU8(float v) {
  v = v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v);
  return static_cast<unsigned char>(static_cast<unsigned int>(v + 0.5f));
}

} // namespace

extern "C" __global__ void
crop_scale_bgr_u8(const unsigned char *src, unsigned int src_pitch,
                  unsigned int src_w, unsigned int src_h, unsigned char *dst,
                  unsigned int dst_pitch, unsigned int dst_w,
                  unsigned int dst_h, float crop_x, float crop_y, float crop_w,
                  float crop_h) {
  const int x = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
  const int y = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
  if (x >= static_cast<int>(dst_w) || y >= static_cast<int>(dst_h))
    return;

  const float fx =
      crop_x + (static_cast<float>(x) + 0.5f) * crop_w /
                   static_cast<float>(dst_w);
  const float fy =
      crop_y + (static_cast<float>(y) + 0.5f) * crop_h /
                   static_cast<float>(dst_h);

  const int ix = ClampInt(static_cast<int>(fx), 0, static_cast<int>(src_w) - 1);
  const int iy = ClampInt(static_cast<int>(fy), 0, static_cast<int>(src_h) - 1);

  const auto *src_px =
      src + static_cast<unsigned int>(iy) * src_pitch +
      static_cast<unsigned int>(ix) * 3u;
  auto *dst_px = dst + static_cast<unsigned int>(y) * dst_pitch +
                 static_cast<unsigned int>(x) * 3u;

  dst_px[0] = src_px[0];
  dst_px[1] = src_px[1];
  dst_px[2] = src_px[2];
}

extern "C" __global__ void
resize_bilinear_bgr_u8(const unsigned char *src, unsigned int src_pitch,
                       unsigned int src_w, unsigned int src_h,
                       unsigned char *dst, unsigned int dst_pitch,
                       unsigned int dst_w, unsigned int dst_h) {
  const int x = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
  const int y = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
  if (x >= static_cast<int>(dst_w) || y >= static_cast<int>(dst_h))
    return;

  const float scale_x = static_cast<float>(src_w) / static_cast<float>(dst_w);
  const float scale_y = static_cast<float>(src_h) / static_cast<float>(dst_h);

  float src_x = (static_cast<float>(x) + 0.5f) * scale_x - 0.5f;
  float src_y = (static_cast<float>(y) + 0.5f) * scale_y - 0.5f;
  src_x = src_x < 0.0f
              ? 0.0f
              : (src_x > static_cast<float>(src_w - 1u)
                     ? static_cast<float>(src_w - 1u)
                     : src_x);
  src_y = src_y < 0.0f
              ? 0.0f
              : (src_y > static_cast<float>(src_h - 1u)
                     ? static_cast<float>(src_h - 1u)
                     : src_y);

  const int x0 = static_cast<int>(src_x);
  const int y0 = static_cast<int>(src_y);
  const int x1 = ClampInt(x0 + 1, 0, static_cast<int>(src_w) - 1);
  const int y1 = ClampInt(y0 + 1, 0, static_cast<int>(src_h) - 1);
  const float tx = src_x - static_cast<float>(x0);
  const float ty = src_y - static_cast<float>(y0);

  const auto *row0 = src + static_cast<unsigned int>(y0) * src_pitch;
  const auto *row1 = src + static_cast<unsigned int>(y1) * src_pitch;
  const auto *p00 = row0 + static_cast<unsigned int>(x0) * 3u;
  const auto *p10 = row0 + static_cast<unsigned int>(x1) * 3u;
  const auto *p01 = row1 + static_cast<unsigned int>(x0) * 3u;
  const auto *p11 = row1 + static_cast<unsigned int>(x1) * 3u;

  auto *dst_px = dst + static_cast<unsigned int>(y) * dst_pitch +
                 static_cast<unsigned int>(x) * 3u;
  for (int c = 0; c < 3; ++c) {
    const float v0 =
        static_cast<float>(p00[c]) +
        tx * (static_cast<float>(p10[c]) - static_cast<float>(p00[c]));
    const float v1 =
        static_cast<float>(p01[c]) +
        tx * (static_cast<float>(p11[c]) - static_cast<float>(p01[c]));
    const float v = v0 + ty * (v1 - v0);
    dst_px[c] = RoundClampHalfUpToU8(v);
  }
}
