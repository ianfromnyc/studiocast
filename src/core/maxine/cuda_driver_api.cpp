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
      !lib_.GetSymbol("cuLaunchKernel", &f_.cuLaunchKernel, &err)) {
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

}  // namespace studiocast::maxine
