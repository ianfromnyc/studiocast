#pragma once

#include <filesystem>
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

using CUcontext = CUctx_st*;
using CUmodule = CUmod_st*;
using CUfunction = CUfunc_st*;
using CUstream = CUstream_st*;
using CUdeviceptr = unsigned long long;

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
  using cuGetErrorString_t = CUresult (*)(CUresult error, const char** pStr);

  struct Functions {
    cuInit_t cuInit = nullptr;
    cuModuleLoadData_t cuModuleLoadData = nullptr;
    cuModuleGetFunction_t cuModuleGetFunction = nullptr;
    cuLaunchKernel_t cuLaunchKernel = nullptr;
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

private:
  bool LoadSymbols(std::string* error_out);

  bool initialized_ = false;
  util::DynLib lib_{std::filesystem::path("libcuda.so.1")};
  Functions f_{};
  std::string error_;
};

}  // namespace studiocast::maxine
