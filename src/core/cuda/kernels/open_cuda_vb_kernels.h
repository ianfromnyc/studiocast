#pragma once

#include <cstdint>
#include <string>

#include "core/cuda/cuda_image.h"
#include "core/maxine/cuda_driver_api.h"

namespace studiocast::cuda::kernels {

// Bilinear resize for f32_1 pitched device images.
//
// Sampling convention matches video::ResizeRgb24Bilinear:
//   src_x = (x + 0.5) * (src_w / dst_w) - 0.5
bool ResizeBilinearF32_1(const CudaImage &src, const CudaImage &dst,
                         studiocast::maxine::CUstream stream,
                         std::string *error_out);

// Separable box blur for interleaved u8x3 pitched device images.
//
// - src/tmp/dst must have identical dimensions.
// - src/tmp/dst formats must match and be rgb_u8 or bgr_u8.
// - radius is clamped to >= 0.
bool BoxBlurSeparableU8x3(const CudaImage &src, const CudaImage &tmp,
                          const CudaImage &dst, int radius,
                          studiocast::maxine::CUstream stream,
                          std::string *error_out);

// Separable box blur for f32_1 pitched device images.
bool BoxBlurSeparableF32_1(const CudaImage &src, const CudaImage &tmp,
                           const CudaImage &dst, int radius,
                           studiocast::maxine::CUstream stream,
                           std::string *error_out);

// out = alpha * fg + (1 - alpha) * bg
//
// - fg/bg/out must be rgb_u8 or bgr_u8 and match each other.
// - alpha must be f32_1 with same dimensions; values are clamped to [0..1].
bool CompositeAlphaU8x3(const CudaImage &fg, const CudaImage &bg,
                        const CudaImage &alpha, const CudaImage &out,
                        studiocast::maxine::CUstream stream,
                        std::string *error_out);

// out = alpha * fg + (1 - alpha) * solid_color
bool CompositeAlphaSolidU8x3(const CudaImage &fg, const CudaImage &alpha,
                             std::uint8_t bg_r, std::uint8_t bg_g,
                             std::uint8_t bg_b, const CudaImage &out,
                             studiocast::maxine::CUstream stream,
                             std::string *error_out);

} // namespace studiocast::cuda::kernels
