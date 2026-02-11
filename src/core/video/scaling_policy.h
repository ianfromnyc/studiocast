#pragma once

#include <string>

namespace studiocast::video {

// Returns an actionable error message describing why output resizing is not permitted.
std::string OutputResizeDisallowedErrorMessage(int src_w, int src_h, int out_w, int out_h);

// Checks whether output resizing is permitted under the current policy.
//
// If a resize is required (src != out) and GPU resize is unavailable, this returns false
// when `allow_cpu_resize` is false and sets `error`.
bool CheckOutputResizeAllowed(int src_w,
                             int src_h,
                             int out_w,
                             int out_h,
                             bool gpu_resize_available,
                             bool allow_cpu_resize,
                             std::string* error);

// Returns whether the camera pipeline should run the standalone Open CUDA GPU scaler
// (CPU RGB -> GPU upload, resize on GPU, GPU -> CPU download).
//
// The scaler must be skipped when:
//   - there is no deferred GPU output to read back, AND
//   - CPU resize is allowed, AND
//   - no Open CUDA effects actually ran this frame
//
// This avoids unnecessary GPU transfers when Open CUDA is otherwise inactive.
bool ShouldRunStandaloneOpenCudaScaler(bool scaling_needed,
                                      bool gpu_backend_is_open_cuda_or_maxine,
                                      bool have_deferred_gpu_out,
                                      bool allow_cpu_resize,
                                      bool open_cuda_effects_ran);

}  // namespace studiocast::video
