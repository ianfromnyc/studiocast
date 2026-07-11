#pragma once

#include <string>
#include <utility>

#include "core/maxine/cuda_driver_api.h"

namespace studiocast::cuda::kernels::detail {

enum class CurrentContextState {
  current,
  none,
  error,
};

inline CurrentContextState
ClassifyCurrentContext(studiocast::maxine::CUresult result,
                       studiocast::maxine::CUcontext context) {
  if (result != studiocast::maxine::CUDA_SUCCESS)
    return CurrentContextState::error;
  return context ? CurrentContextState::current : CurrentContextState::none;
}

// PTX helpers are used by both probes and effect code. Keep driver/context state
// thread-local so diagnostics cannot hold a process-wide driver lock while the
// live path is trying to enqueue work.
struct ThreadCudaDriverState {
  bool attempted = false;
  bool ok = false;
  std::string err;
  studiocast::maxine::CudaDriverApi cuda;
};

inline ThreadCudaDriverState &ThreadCudaDriver() {
  thread_local ThreadCudaDriverState state;
  return state;
}

inline bool EnsureCudaReady(studiocast::maxine::CudaDriverApi **out_cuda,
                            std::string *error_out) {
  if (error_out)
    error_out->clear();
  if (out_cuda)
    *out_cuda = nullptr;

  ThreadCudaDriverState &st = ThreadCudaDriver();
  if (!st.attempted) {
    std::string e;
    st.ok = st.cuda.Initialize(&e);
    st.err = st.ok ? std::string() : std::move(e);
    st.attempted = true;
  }

  if (!st.ok) {
    if (error_out)
      *error_out = st.err.empty() ? "CUDA unavailable" : st.err;
    return false;
  }

  if (!st.cuda.f().cuCtxGetCurrent) {
    if (error_out)
      *error_out = "cuCtxGetCurrent symbol not loaded.";
    return false;
  }

  studiocast::maxine::CUcontext cur = nullptr;
  const auto cur_result = st.cuda.f().cuCtxGetCurrent(&cur);
  switch (ClassifyCurrentContext(cur_result, cur)) {
  case CurrentContextState::current:
    // Preserve caller-owned contexts, including nonzero-device contexts set by
    // ORT/Open CUDA setup. Module loading and launches must use that context.
    if (out_cuda)
      *out_cuda = &st.cuda;
    return true;
  case CurrentContextState::error:
    if (error_out)
      *error_out =
          "cuCtxGetCurrent failed: " + st.cuda.StatusToString(cur_result);
    return false;
  case CurrentContextState::none:
    break;
  }

  // Probe/standalone helpers can still run without a caller-owned context; in
  // that case use this thread's fallback driver context.
  std::string e;
  if (!st.cuda.EnsureContext(&e)) {
    if (error_out)
      *error_out = e;
    return false;
  }

  if (out_cuda)
    *out_cuda = &st.cuda;
  return true;
}

inline bool GetCurrentContext(studiocast::maxine::CudaDriverApi *cuda,
                              studiocast::maxine::CUcontext *out,
                              std::string *error_out) {
  if (out)
    *out = nullptr;
  if (!cuda || !cuda->IsInitialized()) {
    if (error_out)
      *error_out = "CUDA driver API is not initialized.";
    return false;
  }

  const auto &f = cuda->f();
  if (!f.cuCtxGetCurrent) {
    if (error_out)
      *error_out = "cuCtxGetCurrent symbol not loaded.";
    return false;
  }

  studiocast::maxine::CUcontext cur = nullptr;
  const auto st = f.cuCtxGetCurrent(&cur);
  if (st != studiocast::maxine::CUDA_SUCCESS || !cur) {
    if (error_out)
      *error_out = "cuCtxGetCurrent failed: " + cuda->StatusToString(st);
    return false;
  }

  if (out)
    *out = cur;
  return true;
}

} // namespace studiocast::cuda::kernels::detail
