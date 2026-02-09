#pragma once

#include <cstddef>
#include <filesystem>
#include <cstdint>
#include <string>

#include "core/util/dynlib.h"

// Minimal CUDA Driver API loader (runtime only).
//
// StudioCast must build on systems without CUDA installed, so we declare only
// the function-pointer surface we need and resolve symbols via dlopen/dlsym.

namespace studiocast::maxine {

// Forward-declare CUDA opaque handles (match CUDA driver ABI).
struct CUctx_st;
struct CUmod_st;
struct CUfunc_st;
struct CUstream_st;
struct CUarray_st;

using CUcontext = CUctx_st*;
using CUmodule = CUmod_st*;
using CUfunction = CUfunc_st*;
using CUstream = CUstream_st*;
using CUarray = CUarray_st*;
using CUdeviceptr = unsigned long long;

// Minimal CUDA Driver API memcopy ABI surface.
//
// Matches the CUDA driver headers (cuda.h) layout.
// We only declare what we need and rely on runtime symbol resolution.
using CUmemorytype = unsigned int;
inline constexpr CUmemorytype CU_MEMORYTYPE_HOST = 0x01;
inline constexpr CUmemorytype CU_MEMORYTYPE_DEVICE = 0x02;
inline constexpr CUmemorytype CU_MEMORYTYPE_ARRAY = 0x03;
inline constexpr CUmemorytype CU_MEMORYTYPE_UNIFIED = 0x04;

struct CUDA_MEMCPY2D {
  std::size_t srcXInBytes;
  std::size_t srcY;
  CUmemorytype srcMemoryType;
  const void* srcHost;
  CUdeviceptr srcDevice;
  CUarray srcArray;
  std::size_t srcPitch;

  std::size_t dstXInBytes;
  std::size_t dstY;
  CUmemorytype dstMemoryType;
  void* dstHost;
  CUdeviceptr dstDevice;
  CUarray dstArray;
  std::size_t dstPitch;

  std::size_t WidthInBytes;
  std::size_t Height;
};

using CUresult = int;
inline constexpr CUresult CUDA_SUCCESS = 0;

class CudaDriverApi {
public:
  using cuInit_t = CUresult (*)(unsigned int flags);
  using cuModuleLoadData_t = CUresult (*)(CUmodule* module, const void* image);
  using cuModuleGetFunction_t = CUresult (*)(CUfunction* hfunc, CUmodule hmod, const char* name);
  using cuLaunchKernel_t = CUresult (*)(CUfunction f,
                                       unsigned int gridDimX,
                                       unsigned int gridDimY,
                                       unsigned int gridDimZ,
                                       unsigned int blockDimX,
                                       unsigned int blockDimY,
                                       unsigned int blockDimZ,
                                       unsigned int sharedMemBytes,
                                       CUstream hStream,
                                       void** kernelParams,
                                       void** extra);
  using cuMemAllocPitch_t = CUresult (*)(CUdeviceptr* dptr,
                                        std::size_t* pPitch,
                                        std::size_t WidthInBytes,
                                        std::size_t Height,
                                        unsigned int ElementSizeBytes);
  using cuMemFree_t = CUresult (*)(CUdeviceptr dptr);
  using cuMemcpy2DAsync_t = CUresult (*)(const CUDA_MEMCPY2D* pCopy, CUstream hStream);
  using cuStreamCreate_t = CUresult (*)(CUstream* phStream, unsigned int Flags);
  using cuStreamDestroy_t = CUresult (*)(CUstream hStream);
  using cuStreamSynchronize_t = CUresult (*)(CUstream hStream);
  using cuGetErrorString_t = CUresult (*)(CUresult error, const char** pStr);

  struct Functions {
    cuInit_t cuInit = nullptr;
    cuModuleLoadData_t cuModuleLoadData = nullptr;
    cuModuleGetFunction_t cuModuleGetFunction = nullptr;
    cuLaunchKernel_t cuLaunchKernel = nullptr;
    cuMemAllocPitch_t cuMemAllocPitch = nullptr;
    cuMemFree_t cuMemFree = nullptr;
    cuMemcpy2DAsync_t cuMemcpy2DAsync = nullptr;
    cuStreamCreate_t cuStreamCreate = nullptr;
    cuStreamDestroy_t cuStreamDestroy = nullptr;
    cuStreamSynchronize_t cuStreamSynchronize = nullptr;
    cuGetErrorString_t cuGetErrorString = nullptr;
  };

  CudaDriverApi();
  ~CudaDriverApi();

  CudaDriverApi(const CudaDriverApi&) = delete;
  CudaDriverApi& operator=(const CudaDriverApi&) = delete;

  CudaDriverApi(CudaDriverApi&&) noexcept;
  CudaDriverApi& operator=(CudaDriverApi&&) noexcept;

  bool Initialize(std::string* error_out);
  bool IsInitialized() const { return initialized_; }

  const Functions& f() const { return f_; }

  std::string StatusToString(CUresult code) const;
  const std::string& error() const { return error_; }

  struct PitchAllocation {
    CUdeviceptr ptr = 0;
    std::size_t pitch = 0;
  };

  bool AllocatePitch(std::size_t width_bytes,
                     std::size_t height,
                     PitchAllocation* out,
                     std::string* error_out,
                     unsigned int element_size_bytes = 1);

  bool Free(CUdeviceptr ptr, std::string* error_out);

  bool MemcpyHtoD2DAsync(CUdeviceptr dst,
                         std::size_t dst_pitch,
                         const void* src,
                         std::size_t src_pitch,
                         std::size_t width_bytes,
                         std::size_t height,
                         CUstream stream,
                         std::string* error_out);

  bool MemcpyDtoH2DAsync(void* dst,
                         std::size_t dst_pitch,
                         CUdeviceptr src,
                         std::size_t src_pitch,
                         std::size_t width_bytes,
                         std::size_t height,
                         CUstream stream,
                         std::string* error_out);

  bool CreateStream(CUstream* out, std::string* error_out, unsigned int flags = 0);
  bool DestroyStream(CUstream stream, std::string* error_out);
  bool StreamSynchronize(CUstream stream, std::string* error_out);

private:
  bool LoadSymbols(std::string* error_out);

  bool initialized_ = false;
  util::DynLib lib_{std::filesystem::path("libcuda.so.1")};
  Functions f_{};
  std::string error_;
};

}  // namespace studiocast::maxine
