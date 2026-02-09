#pragma once

#include <string>

#include "core/cuda/cuda_image.h"
#include "core/maxine/cuda_driver_api.h"

namespace studiocast::cuda::kernels {

// Bilinear resize for interleaved RGB/BGR U8 pitched device images.
//
// - Source and destination formats must match and be rgb_u8 or bgr_u8.
// - Uses the same sampling convention as video::ResizeRgb24Bilinear:
//     src_x = (x + 0.5) * (src_w / dst_w) - 0.5
//
// The operation is enqueued on the provided stream and does not synchronize.
bool ResizeBilinear(const CudaImage& src,
                    const CudaImage& dst,
                    studiocast::maxine::CUstream stream,
                    std::string* error_out);

}  // namespace studiocast::cuda::kernels
