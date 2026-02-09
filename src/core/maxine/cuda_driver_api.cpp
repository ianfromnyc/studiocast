#include "core/maxine/cuda_driver_api.h"

#include <filesystem>

namespace studiocast::maxine {

CudaDriverApi::CudaDriverApi() = default;

CudaDriverApi::~CudaDriverApi() { lib_.Close(); }

CudaDriverApi::CudaDriverApi(CudaDriverApi&&) noexcept = default;

CudaDriverApi& CudaDriverApi::operator=(CudaDriverApi&&) noexcept = default;

bool CudaDriverApi::Initialize(std::string* error_out) {
  if (initialized_) return true;

  std::string err;
  if (!lib_.Open(util::DynLib::Scope::Local, &err)) {
    error_ = err;
    if (error_out) *error_out = err;
    return false;
  }

  if (!LoadSymbols(&err)) {
    error_ = err;
    if (error_out) *error_out = err;
    lib_.Close();
    return false;
  }

  const CUresult st = f_.cuInit ? f_.cuInit(0) : -1;
  if (st != CUDA_SUCCESS) {
    err = "cuInit failed: " + StatusToString(st);
    error_ = err;
    if (error_out) *error_out = err;
    return false;
  }

  initialized_ = true;
  return true;
}

bool CudaDriverApi::LoadSymbols(std::string* error_out) {
  std::string err;

  if (!lib_.GetSymbol("cuInit", &f_.cuInit, &err) ||
      !lib_.GetSymbol("cuModuleLoadData", &f_.cuModuleLoadData, &err) ||
      !lib_.GetSymbol("cuModuleGetFunction", &f_.cuModuleGetFunction, &err) ||
      !lib_.GetSymbol("cuLaunchKernel", &f_.cuLaunchKernel, &err) ||
      !lib_.GetSymbol("cuMemAllocPitch", &f_.cuMemAllocPitch, &err) ||
      !lib_.GetSymbol("cuMemFree", &f_.cuMemFree, &err) ||
      !lib_.GetSymbol("cuMemcpy2DAsync", &f_.cuMemcpy2DAsync, &err) ||
      !lib_.GetSymbol("cuStreamCreate", &f_.cuStreamCreate, &err) ||
      !lib_.GetSymbol("cuStreamDestroy", &f_.cuStreamDestroy, &err) ||
      !lib_.GetSymbol("cuStreamSynchronize", &f_.cuStreamSynchronize, &err)) {
    if (error_out) *error_out = err;
    return false;
  }

  // Optional: cuGetErrorString.
  (void)lib_.GetSymbol("cuGetErrorString", &f_.cuGetErrorString, nullptr);
  return true;
}

std::string CudaDriverApi::StatusToString(CUresult code) const {
  if (code == CUDA_SUCCESS) return "CUDA_SUCCESS";
  if (f_.cuGetErrorString) {
    const char* s = nullptr;
    if (f_.cuGetErrorString(code, &s) == CUDA_SUCCESS && s) {
      return std::string(s);
    }
  }
  return "CUDA error " + std::to_string(code);
}

bool CudaDriverApi::AllocatePitch(std::size_t width_bytes,
                                 std::size_t height,
                                 PitchAllocation* out,
                                 std::string* error_out,
                                 unsigned int element_size_bytes) {
  if (error_out) error_out->clear();
  if (!initialized_) {
    if (error_out) *error_out = "CudaDriverApi not initialized.";
    return false;
  }
  if (!out) {
    if (error_out) *error_out = "AllocatePitch called with null out pointer.";
    return false;
  }
  if (!f_.cuMemAllocPitch) {
    if (error_out) *error_out = "cuMemAllocPitch symbol not loaded.";
    return false;
  }

  out->ptr = 0;
  out->pitch = 0;
  const CUresult st = f_.cuMemAllocPitch(&out->ptr, &out->pitch, width_bytes, height, element_size_bytes);
  if (st != CUDA_SUCCESS) {
    if (error_out) *error_out = "cuMemAllocPitch failed: " + StatusToString(st);
    return false;
  }
  return true;
}

bool CudaDriverApi::Free(CUdeviceptr ptr, std::string* error_out) {
  if (error_out) error_out->clear();
  if (!initialized_) {
    if (error_out) *error_out = "CudaDriverApi not initialized.";
    return false;
  }
  if (!f_.cuMemFree) {
    if (error_out) *error_out = "cuMemFree symbol not loaded.";
    return false;
  }

  const CUresult st = f_.cuMemFree(ptr);
  if (st != CUDA_SUCCESS) {
    if (error_out) *error_out = "cuMemFree failed: " + StatusToString(st);
    return false;
  }
  return true;
}

bool CudaDriverApi::MemcpyHtoD2DAsync(CUdeviceptr dst,
                                     std::size_t dst_pitch,
                                     const void* src,
                                     std::size_t src_pitch,
                                     std::size_t width_bytes,
                                     std::size_t height,
                                     CUstream stream,
                                     std::string* error_out) {
  if (error_out) error_out->clear();
  if (!initialized_) {
    if (error_out) *error_out = "CudaDriverApi not initialized.";
    return false;
  }
  if (!f_.cuMemcpy2DAsync) {
    if (error_out) *error_out = "cuMemcpy2DAsync symbol not loaded.";
    return false;
  }

  CUDA_MEMCPY2D cpy{};
  cpy.srcXInBytes = 0;
  cpy.srcY = 0;
  cpy.srcMemoryType = CU_MEMORYTYPE_HOST;
  cpy.srcHost = src;
  cpy.srcDevice = 0;
  cpy.srcArray = nullptr;
  cpy.srcPitch = src_pitch;

  cpy.dstXInBytes = 0;
  cpy.dstY = 0;
  cpy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
  cpy.dstHost = nullptr;
  cpy.dstDevice = dst;
  cpy.dstArray = nullptr;
  cpy.dstPitch = dst_pitch;

  cpy.WidthInBytes = width_bytes;
  cpy.Height = height;

  const CUresult st = f_.cuMemcpy2DAsync(&cpy, stream);
  if (st != CUDA_SUCCESS) {
    if (error_out) *error_out = "cuMemcpy2DAsync (HtoD) failed: " + StatusToString(st);
    return false;
  }
  return true;
}

bool CudaDriverApi::MemcpyDtoH2DAsync(void* dst,
                                     std::size_t dst_pitch,
                                     CUdeviceptr src,
                                     std::size_t src_pitch,
                                     std::size_t width_bytes,
                                     std::size_t height,
                                     CUstream stream,
                                     std::string* error_out) {
  if (error_out) error_out->clear();
  if (!initialized_) {
    if (error_out) *error_out = "CudaDriverApi not initialized.";
    return false;
  }
  if (!f_.cuMemcpy2DAsync) {
    if (error_out) *error_out = "cuMemcpy2DAsync symbol not loaded.";
    return false;
  }

  CUDA_MEMCPY2D cpy{};
  cpy.srcXInBytes = 0;
  cpy.srcY = 0;
  cpy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
  cpy.srcHost = nullptr;
  cpy.srcDevice = src;
  cpy.srcArray = nullptr;
  cpy.srcPitch = src_pitch;

  cpy.dstXInBytes = 0;
  cpy.dstY = 0;
  cpy.dstMemoryType = CU_MEMORYTYPE_HOST;
  cpy.dstHost = dst;
  cpy.dstDevice = 0;
  cpy.dstArray = nullptr;
  cpy.dstPitch = dst_pitch;

  cpy.WidthInBytes = width_bytes;
  cpy.Height = height;

  const CUresult st = f_.cuMemcpy2DAsync(&cpy, stream);
  if (st != CUDA_SUCCESS) {
    if (error_out) *error_out = "cuMemcpy2DAsync (DtoH) failed: " + StatusToString(st);
    return false;
  }
  return true;
}

bool CudaDriverApi::CreateStream(CUstream* out, std::string* error_out, unsigned int flags) {
  if (error_out) error_out->clear();
  if (!initialized_) {
    if (error_out) *error_out = "CudaDriverApi not initialized.";
    return false;
  }
  if (!out) {
    if (error_out) *error_out = "CreateStream called with null out pointer.";
    return false;
  }
  if (!f_.cuStreamCreate) {
    if (error_out) *error_out = "cuStreamCreate symbol not loaded.";
    return false;
  }

  *out = nullptr;
  const CUresult st = f_.cuStreamCreate(out, flags);
  if (st != CUDA_SUCCESS) {
    if (error_out) *error_out = "cuStreamCreate failed: " + StatusToString(st);
    return false;
  }
  return true;
}

bool CudaDriverApi::DestroyStream(CUstream stream, std::string* error_out) {
  if (error_out) error_out->clear();
  if (!initialized_) {
    if (error_out) *error_out = "CudaDriverApi not initialized.";
    return false;
  }
  if (!f_.cuStreamDestroy) {
    if (error_out) *error_out = "cuStreamDestroy symbol not loaded.";
    return false;
  }

  const CUresult st = f_.cuStreamDestroy(stream);
  if (st != CUDA_SUCCESS) {
    if (error_out) *error_out = "cuStreamDestroy failed: " + StatusToString(st);
    return false;
  }
  return true;
}

bool CudaDriverApi::StreamSynchronize(CUstream stream, std::string* error_out) {
  if (error_out) error_out->clear();
  if (!initialized_) {
    if (error_out) *error_out = "CudaDriverApi not initialized.";
    return false;
  }
  if (!f_.cuStreamSynchronize) {
    if (error_out) *error_out = "cuStreamSynchronize symbol not loaded.";
    return false;
  }

  const CUresult st = f_.cuStreamSynchronize(stream);
  if (st != CUDA_SUCCESS) {
    if (error_out) *error_out = "cuStreamSynchronize failed: " + StatusToString(st);
    return false;
  }
  return true;
}

}  // namespace studiocast::maxine
