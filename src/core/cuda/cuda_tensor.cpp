#include "core/cuda/cuda_tensor.h"

namespace studiocast::cuda {

bool CudaTensor::Valid() const {
  return ptr != 0 && bytes > 0 && pitch >= bytes && n > 0 && c > 0 && h > 0 &&
         w > 0;
}

std::size_t CudaTensor::ElementCount() const {
  if (n <= 0 || c <= 0 || h <= 0 || w <= 0)
    return 0;
  return static_cast<std::size_t>(n) * static_cast<std::size_t>(c) *
         static_cast<std::size_t>(h) * static_cast<std::size_t>(w);
}

bool CudaTensor::AllocateNchwF32(studiocast::maxine::CudaDriverApi *cuda,
                                 int n_in, int c_in, int h_in, int w_in,
                                 std::string *error_out) {
  if (error_out)
    error_out->clear();

  if (!cuda || !cuda->IsInitialized()) {
    if (error_out)
      *error_out =
          "CudaTensor::AllocateNchwF32: CUDA driver API not initialized.";
    return false;
  }
  if (n_in <= 0 || c_in <= 0 || h_in <= 0 || w_in <= 0) {
    if (error_out)
      *error_out = "CudaTensor::AllocateNchwF32: invalid shape.";
    return false;
  }

  const std::size_t elem_count =
      static_cast<std::size_t>(n_in) * static_cast<std::size_t>(c_in) *
      static_cast<std::size_t>(h_in) * static_cast<std::size_t>(w_in);
  const std::size_t byte_count = elem_count * sizeof(float);

  studiocast::maxine::CudaDriverApi::PitchAllocation alloc{};
  // Allocate as a single row (Height=1) so the first `byte_count` bytes are
  // contiguous.
  const bool ok = cuda->AllocatePitch(byte_count, 1, &alloc, error_out,
                                      /*element_size_bytes=*/4);
  if (!ok)
    return false;

  ptr = alloc.ptr;
  pitch = alloc.pitch;
  bytes = byte_count;
  n = n_in;
  c = c_in;
  h = h_in;
  w = w_in;
  owns_memory = true;
  return true;
}

bool CudaTensor::Free(studiocast::maxine::CudaDriverApi *cuda,
                      std::string *error_out) {
  if (error_out)
    error_out->clear();

  if (ptr == 0) {
    pitch = 0;
    bytes = 0;
    n = 0;
    c = 0;
    h = 0;
    w = 0;
    owns_memory = false;
    return true;
  }
  if (!owns_memory) {
    if (error_out)
      *error_out = "CudaTensor::Free called for non-owning tensor.";
    return false;
  }
  if (!cuda || !cuda->IsInitialized()) {
    if (error_out)
      *error_out = "CudaTensor::Free: CUDA driver API not initialized.";
    return false;
  }

  const bool ok = cuda->Free(ptr, error_out);
  if (!ok)
    return false;

  ptr = 0;
  pitch = 0;
  bytes = 0;
  n = 0;
  c = 0;
  h = 0;
  w = 0;
  owns_memory = false;
  return true;
}

bool CudaTensor::ReallocIfNeededNchwF32(studiocast::maxine::CudaDriverApi *cuda,
                                        int n_in, int c_in, int h_in, int w_in,
                                        std::string *error_out) {
  if (error_out)
    error_out->clear();

  if (ptr != 0 && n == n_in && c == c_in && h == h_in && w == w_in) {
    // Capacity may be larger than required.
    const std::size_t want_bytes = ElementCount() * sizeof(float);
    if (bytes == want_bytes && pitch >= bytes)
      return true;
  }

  if (ptr != 0) {
    if (!Free(cuda, error_out))
      return false;
  }
  return AllocateNchwF32(cuda, n_in, c_in, h_in, w_in, error_out);
}

bool CudaTensor::UploadFromCpuF32(studiocast::maxine::CudaDriverApi *cuda,
                                  const float *src, std::size_t src_floats,
                                  studiocast::maxine::CUstream stream,
                                  std::string *error_out) const {
  if (error_out)
    error_out->clear();

  if (!cuda || !cuda->IsInitialized()) {
    if (error_out)
      *error_out =
          "CudaTensor::UploadFromCpuF32: CUDA driver API not initialized.";
    return false;
  }
  if (!Valid()) {
    if (error_out)
      *error_out = "CudaTensor::UploadFromCpuF32: invalid tensor.";
    return false;
  }
  if (!src) {
    if (error_out)
      *error_out = "CudaTensor::UploadFromCpuF32: src is null.";
    return false;
  }
  if (src_floats < ElementCount()) {
    if (error_out)
      *error_out = "CudaTensor::UploadFromCpuF32: source buffer too small.";
    return false;
  }

  return cuda->MemcpyHtoD2DAsync(ptr, pitch, src, /*src_pitch=*/bytes,
                                 /*width_bytes=*/bytes, /*height=*/1, stream,
                                 error_out);
}

bool CudaTensor::DownloadToCpuF32(studiocast::maxine::CudaDriverApi *cuda,
                                  std::vector<float> *out,
                                  studiocast::maxine::CUstream stream,
                                  std::string *error_out) const {
  if (error_out)
    error_out->clear();
  if (!out)
    return false;
  out->clear();

  if (!cuda || !cuda->IsInitialized()) {
    if (error_out)
      *error_out =
          "CudaTensor::DownloadToCpuF32: CUDA driver API not initialized.";
    return false;
  }
  if (!Valid()) {
    if (error_out)
      *error_out = "CudaTensor::DownloadToCpuF32: invalid tensor.";
    return false;
  }

  out->resize(ElementCount());
  return cuda->MemcpyDtoH2DAsync(out->data(), /*dst_pitch=*/bytes, ptr, pitch,
                                 /*width_bytes=*/bytes, /*height=*/1, stream,
                                 error_out);
}

} // namespace studiocast::cuda
