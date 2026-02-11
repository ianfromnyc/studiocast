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

}  // namespace studiocast::video
