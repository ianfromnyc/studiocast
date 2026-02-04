#pragma once

#include <cstddef>
#include <cstdint>

#include "core/maxine/nvcv_types.h"
#include "core/video/v4l2_writer.h"

namespace studiocast::video {

// CPU-accessible frame view used as a staging buffer for v4l2loopback output.
// The pipeline owns the backing memory; effects must not reallocate it.
struct CpuFrameView {
  std::uint8_t* data = nullptr;
  int width = 0;
  int height = 0;
  std::size_t stride_bytes = 0;  // bytes per row
  PixelFormat format = PixelFormat::rgb24;

  std::size_t MinStrideBytes() const {
    switch (format) {
      case PixelFormat::yuyv: return static_cast<std::size_t>(width) * 2u;
      case PixelFormat::rgb24: return static_cast<std::size_t>(width) * 3u;
    }
    return 0;
  }

  bool Valid() const {
    return data && width > 0 && height > 0 && stride_bytes >= MinStrideBytes();
  }
};

// Combined CPU/GPU view of a video frame intended for Maxine-backed processing.
//
// Ownership/lifetime:
// - The pipeline owns all pointers referenced by this struct.
// - Effects may read/write GPU buffers and may populate the CPU staging buffer.
// - Effects must not allocate/deallocate `NvCVImage` memory via these pointers
//   unless explicitly documented by a concrete implementation.
struct GpuFrame {
  int width = 0;
  int height = 0;

  // CPU staging buffer for loopback write (typically RGB24).
  CpuFrameView cpu{};

  // Optional: GPU image view for Maxine operations (commonly `memSpace = NVCV_GPU`).
  // The image may be used as input and/or output depending on the effect.
  studiocast::maxine::NvCVImage* nvcv_gpu = nullptr;

  // Optional: CPU-side NvCVImage view (commonly `memSpace = NVCV_CPU`) describing
  // the same backing memory as `cpu` (useful for NvCV transfers).
  studiocast::maxine::NvCVImage* nvcv_cpu = nullptr;

  // Optional: scratch image storage (used by some NvCV transfer/conversion paths).
  studiocast::maxine::NvCVImage* nvcv_tmp = nullptr;

  // Optional raw CUDA device pointer view for pipelines that do not use NvCVImage.
  void* device_ptr = nullptr;
  std::size_t device_pitch_bytes = 0;

  // Optional stream for async GPU work.
  studiocast::maxine::CUstream cuda_stream = nullptr;

  bool ValidDimensions() const { return width > 0 && height > 0; }
  bool HasCpuStaging() const { return cpu.Valid(); }
  bool HasGpuImage() const { return nvcv_gpu != nullptr || device_ptr != nullptr; }
};

}  // namespace studiocast::video
