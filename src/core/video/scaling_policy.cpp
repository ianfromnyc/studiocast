#include "scaling_policy.h"

#include <sstream>

namespace studiocast::video {

std::string OutputResizeDisallowedErrorMessage(int src_w, int src_h, int out_w, int out_h) {
  std::ostringstream oss;
  oss << "Output resize required (" << src_w << "x" << src_h << " -> " << out_w << "x" << out_h
      << ") but CPU resize is disabled and GPU resize is unavailable or failed. "
         "Use follow_capture output policy or enable GPU resize.";
  return oss.str();
}

bool CheckOutputResizeAllowed(int src_w,
                             int src_h,
                             int out_w,
                             int out_h,
                             bool gpu_resize_available,
                             bool allow_cpu_resize,
                             std::string* error) {
  if (src_w == out_w && src_h == out_h) return true;
  if (gpu_resize_available) return true;
  if (allow_cpu_resize) return true;

  if (error) {
    *error = OutputResizeDisallowedErrorMessage(src_w, src_h, out_w, out_h);
  }
  return false;
}

bool ShouldRunStandaloneOpenCudaScaler(bool scaling_needed,
                                      bool gpu_backend_is_open_cuda_or_maxine,
                                      bool have_deferred_gpu_out,
                                      bool allow_cpu_resize,
                                      bool open_cuda_effects_ran) {
  if (!scaling_needed) return false;
  if (!gpu_backend_is_open_cuda_or_maxine) return false;

  const bool should_skip_to_avoid_unnecessary_transfers =
      !have_deferred_gpu_out && allow_cpu_resize && !open_cuda_effects_ran;
  if (should_skip_to_avoid_unnecessary_transfers) return false;

  return true;
}

}  // namespace studiocast::video
