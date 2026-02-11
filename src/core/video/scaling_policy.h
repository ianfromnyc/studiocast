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

}  // namespace studiocast::video
