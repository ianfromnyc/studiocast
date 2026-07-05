// Reference CUDA C kernels for the Open CUDA virtual background path.
//
// These kernels are not compiled as part of the StudioCast build.
// Instead, we compile them to PTX (e.g. via `nvcc -ptx`) and embed the PTX
// into a C++ translation unit loaded via the CUDA Driver API.
//
// Keep this file in sync with the embedded PTX module in:
//   core/cuda/kernels/open_cuda_vb_kernels_ptx.cpp

#include <cuda_runtime.h>

namespace {

__device__ __forceinline__ int clampi(int v, int lo, int hi) {
  return v < lo ? lo : (v > hi ? hi : v);
}

__device__ __forceinline__ float clamp01(float v) {
  return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

} // namespace

extern "C" __global__ void
resize_bilinear_f32_1(const void *srcPtr, unsigned int srcPitchBytes,
                      unsigned int srcW, unsigned int srcH, void *dstPtr,
                      unsigned int dstPitchBytes, unsigned int dstW,
                      unsigned int dstH) {
  const int x = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
  const int y = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
  if (x >= static_cast<int>(dstW) || y >= static_cast<int>(dstH))
    return;

  const float sx = static_cast<float>(srcW) / static_cast<float>(dstW);
  const float sy = static_cast<float>(srcH) / static_cast<float>(dstH);

  const float srcX = (static_cast<float>(x) + 0.5f) * sx - 0.5f;
  const float srcY = (static_cast<float>(y) + 0.5f) * sy - 0.5f;

  int x0 = static_cast<int>(floorf(srcX));
  int y0 = static_cast<int>(floorf(srcY));
  const float tx = srcX - static_cast<float>(x0);
  const float ty = srcY - static_cast<float>(y0);

  x0 = clampi(x0, 0, static_cast<int>(srcW) - 1);
  y0 = clampi(y0, 0, static_cast<int>(srcH) - 1);
  const int x1 = clampi(x0 + 1, 0, static_cast<int>(srcW) - 1);
  const int y1 = clampi(y0 + 1, 0, static_cast<int>(srcH) - 1);

  const auto *srcBase = static_cast<const unsigned char *>(srcPtr);
  const auto *row0 = reinterpret_cast<const float *>(
      srcBase + static_cast<size_t>(y0) * srcPitchBytes);
  const auto *row1 = reinterpret_cast<const float *>(
      srcBase + static_cast<size_t>(y1) * srcPitchBytes);
  const float v00 = row0[x0];
  const float v10 = row0[x1];
  const float v01 = row1[x0];
  const float v11 = row1[x1];

  const float v0 = v00 + (v10 - v00) * tx;
  const float v1 = v01 + (v11 - v01) * tx;
  const float v = v0 + (v1 - v0) * ty;

  auto *dstBase = static_cast<unsigned char *>(dstPtr);
  auto *dstRow = reinterpret_cast<float *>(dstBase + static_cast<size_t>(y) *
                                                         dstPitchBytes);
  dstRow[x] = v;
}

extern "C" __global__ void
box_blur_h_u8x3(const void *srcPtr, unsigned int srcPitchBytes, unsigned int w,
                unsigned int h, void *dstPtr, unsigned int dstPitchBytes,
                int radius) {
  const int x = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
  const int y = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
  if (x >= static_cast<int>(w) || y >= static_cast<int>(h))
    return;

  radius = radius < 0 ? 0 : radius;
  const int count = radius * 2 + 1;

  const auto *srcBase = static_cast<const unsigned char *>(srcPtr) +
                        static_cast<size_t>(y) * srcPitchBytes;
  int sum0 = 0, sum1 = 0, sum2 = 0;
  for (int dx = -radius; dx <= radius; ++dx) {
    const int xx = clampi(x + dx, 0, static_cast<int>(w) - 1);
    const int off = xx * 3;
    sum0 += static_cast<int>(srcBase[off + 0]);
    sum1 += static_cast<int>(srcBase[off + 1]);
    sum2 += static_cast<int>(srcBase[off + 2]);
  }

  auto *dstBase = static_cast<unsigned char *>(dstPtr) +
                  static_cast<size_t>(y) * dstPitchBytes;
  const int off = x * 3;
  dstBase[off + 0] = static_cast<unsigned char>((sum0 + count / 2) / count);
  dstBase[off + 1] = static_cast<unsigned char>((sum1 + count / 2) / count);
  dstBase[off + 2] = static_cast<unsigned char>((sum2 + count / 2) / count);
}

extern "C" __global__ void
box_blur_v_u8x3(const void *srcPtr, unsigned int srcPitchBytes, unsigned int w,
                unsigned int h, void *dstPtr, unsigned int dstPitchBytes,
                int radius) {
  const int x = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
  const int y = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
  if (x >= static_cast<int>(w) || y >= static_cast<int>(h))
    return;

  radius = radius < 0 ? 0 : radius;
  const int count = radius * 2 + 1;

  int sum0 = 0, sum1 = 0, sum2 = 0;
  for (int dy = -radius; dy <= radius; ++dy) {
    const int yy = clampi(y + dy, 0, static_cast<int>(h) - 1);
    const auto *row = static_cast<const unsigned char *>(srcPtr) +
                      static_cast<size_t>(yy) * srcPitchBytes;
    const int off = x * 3;
    sum0 += static_cast<int>(row[off + 0]);
    sum1 += static_cast<int>(row[off + 1]);
    sum2 += static_cast<int>(row[off + 2]);
  }

  auto *dstRow = static_cast<unsigned char *>(dstPtr) +
                 static_cast<size_t>(y) * dstPitchBytes;
  const int off = x * 3;
  dstRow[off + 0] = static_cast<unsigned char>((sum0 + count / 2) / count);
  dstRow[off + 1] = static_cast<unsigned char>((sum1 + count / 2) / count);
  dstRow[off + 2] = static_cast<unsigned char>((sum2 + count / 2) / count);
}

extern "C" __global__ void
box_blur_h_f32_1(const void *srcPtr, unsigned int srcPitchBytes, unsigned int w,
                 unsigned int h, void *dstPtr, unsigned int dstPitchBytes,
                 int radius) {
  const int x = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
  const int y = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
  if (x >= static_cast<int>(w) || y >= static_cast<int>(h))
    return;

  radius = radius < 0 ? 0 : radius;
  const int count = radius * 2 + 1;
  const float inv = 1.0f / static_cast<float>(count);

  const auto *srcBaseU8 = static_cast<const unsigned char *>(srcPtr) +
                          static_cast<size_t>(y) * srcPitchBytes;
  const auto *srcRow = reinterpret_cast<const float *>(srcBaseU8);
  float sum = 0.0f;
  for (int dx = -radius; dx <= radius; ++dx) {
    const int xx = clampi(x + dx, 0, static_cast<int>(w) - 1);
    sum += srcRow[xx];
  }

  auto *dstBaseU8 = static_cast<unsigned char *>(dstPtr) +
                    static_cast<size_t>(y) * dstPitchBytes;
  auto *dstRow = reinterpret_cast<float *>(dstBaseU8);
  dstRow[x] = sum * inv;
}

extern "C" __global__ void
box_blur_v_f32_1(const void *srcPtr, unsigned int srcPitchBytes, unsigned int w,
                 unsigned int h, void *dstPtr, unsigned int dstPitchBytes,
                 int radius) {
  const int x = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
  const int y = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
  if (x >= static_cast<int>(w) || y >= static_cast<int>(h))
    return;

  radius = radius < 0 ? 0 : radius;
  const int count = radius * 2 + 1;
  const float inv = 1.0f / static_cast<float>(count);

  float sum = 0.0f;
  for (int dy = -radius; dy <= radius; ++dy) {
    const int yy = clampi(y + dy, 0, static_cast<int>(h) - 1);
    const auto *rowU8 = static_cast<const unsigned char *>(srcPtr) +
                        static_cast<size_t>(yy) * srcPitchBytes;
    const auto *row = reinterpret_cast<const float *>(rowU8);
    sum += row[x];
  }

  auto *dstU8 = static_cast<unsigned char *>(dstPtr) +
                static_cast<size_t>(y) * dstPitchBytes;
  auto *dstRow = reinterpret_cast<float *>(dstU8);
  dstRow[x] = sum * inv;
}

extern "C" __global__ void
composite_alpha_u8x3_bg(const void *fgPtr, unsigned int fgPitchBytes,
                        const void *bgPtr, unsigned int bgPitchBytes,
                        const void *alphaPtr, unsigned int alphaPitchBytes,
                        unsigned int w, unsigned int h, void *outPtr,
                        unsigned int outPitchBytes) {
  const int x = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
  const int y = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
  if (x >= static_cast<int>(w) || y >= static_cast<int>(h))
    return;

  const auto *fgRow = static_cast<const unsigned char *>(fgPtr) +
                      static_cast<size_t>(y) * fgPitchBytes;
  const auto *bgRow = static_cast<const unsigned char *>(bgPtr) +
                      static_cast<size_t>(y) * bgPitchBytes;

  const auto *aRowU8 = static_cast<const unsigned char *>(alphaPtr) +
                       static_cast<size_t>(y) * alphaPitchBytes;
  const auto *aRow = reinterpret_cast<const float *>(aRowU8);
  const float a = clamp01(aRow[x]);
  const float ia = 1.0f - a;

  auto *outRow = static_cast<unsigned char *>(outPtr) +
                 static_cast<size_t>(y) * outPitchBytes;
  const int off = x * 3;

  for (int c = 0; c < 3; ++c) {
    const float f = static_cast<float>(fgRow[off + c]);
    const float b = static_cast<float>(bgRow[off + c]);
    float v = f * a + b * ia;
    v = v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v);
    outRow[off + c] = static_cast<unsigned char>(v + 0.5f);
  }
}

extern "C" __global__ void
composite_alpha_u8x3_solid(const void *fgPtr, unsigned int fgPitchBytes,
                           const void *alphaPtr, unsigned int alphaPitchBytes,
                           unsigned int w, unsigned int h, unsigned char bgR,
                           unsigned char bgG, unsigned char bgB, void *outPtr,
                           unsigned int outPitchBytes) {
  const int x = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
  const int y = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
  if (x >= static_cast<int>(w) || y >= static_cast<int>(h))
    return;

  const auto *fgRow = static_cast<const unsigned char *>(fgPtr) +
                      static_cast<size_t>(y) * fgPitchBytes;
  const auto *aRowU8 = static_cast<const unsigned char *>(alphaPtr) +
                       static_cast<size_t>(y) * alphaPitchBytes;
  const auto *aRow = reinterpret_cast<const float *>(aRowU8);
  const float a = clamp01(aRow[x]);
  const float ia = 1.0f - a;

  auto *outRow = static_cast<unsigned char *>(outPtr) +
                 static_cast<size_t>(y) * outPitchBytes;
  const int off = x * 3;

  const unsigned char bg[3] = {bgR, bgG, bgB};
  for (int c = 0; c < 3; ++c) {
    const float f = static_cast<float>(fgRow[off + c]);
    const float b = static_cast<float>(bg[c]);
    float v = f * a + b * ia;
    v = v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v);
    outRow[off + c] = static_cast<unsigned char>(v + 0.5f);
  }
}

extern "C" __global__ void
key_light_u8x3(const void *srcPtr, unsigned int srcPitchBytes,
               const void *alphaPtr, unsigned int alphaPitchBytes,
               unsigned int w, unsigned int h, float targetR, float targetG,
               float targetB, float intensity, float direction, void *outPtr,
               unsigned int outPitchBytes) {
  const int x = static_cast<int>(blockIdx.x * blockDim.x + threadIdx.x);
  const int y = static_cast<int>(blockIdx.y * blockDim.y + threadIdx.y);
  if (x >= static_cast<int>(w) || y >= static_cast<int>(h))
    return;

  const auto *srcRow = static_cast<const unsigned char *>(srcPtr) +
                       static_cast<size_t>(y) * srcPitchBytes;
  auto *outRow = static_cast<unsigned char *>(outPtr) +
                 static_cast<size_t>(y) * outPitchBytes;
  const auto *aRowU8 = static_cast<const unsigned char *>(alphaPtr) +
                       static_cast<size_t>(y) * alphaPitchBytes;
  const auto *aRow = reinterpret_cast<const float *>(aRowU8);

  const int off = x * 3;
  const float a = clamp01(aRow[x]);
  if (a <= 0.02f || intensity <= 0.0001f) {
    outRow[off + 0] = srcRow[off + 0];
    outRow[off + 1] = srcRow[off + 1];
    outRow[off + 2] = srcRow[off + 2];
    return;
  }

  const float cx = static_cast<float>(w) * 0.5f;
  const float invCx = (cx > 1.0f) ? (1.0f / cx) : 0.0f;
  const float xNorm = (static_cast<float>(x) - cx) * invCx;
  float field = 1.0f + direction * xNorm * 0.35f;
  field = field < 0.65f ? 0.65f : (field > 1.35f ? 1.35f : field);
  const float t = clamp01(intensity * a * field);

  const float target[3] = {targetR, targetG, targetB};
  for (int c = 0; c < 3; ++c) {
    const float in = static_cast<float>(srcRow[off + c]);
    float v = in + (target[c] - in) * t;
    v = v < 0.0f ? 0.0f : (v > 255.0f ? 255.0f : v);
    outRow[off + c] = static_cast<unsigned char>(v + 0.5f);
  }
}
