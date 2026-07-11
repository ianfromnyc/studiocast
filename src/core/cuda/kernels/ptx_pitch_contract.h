#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

namespace studiocast::cuda::kernels::detail {

// Several embedded PTX kernels declare pitch parameters as .u32 but use signed
// row-offset arithmetic (for example, mul.wide.s32). Keep the host ABI inside
// the signed 32-bit range so pitches cannot be reinterpreted as negative values
// by PTX address math.
constexpr std::size_t kSignedInt32PtxPitchMaxBytes =
    static_cast<std::size_t>(std::numeric_limits<std::int32_t>::max());

inline bool CheckSignedInt32PtxPitch(std::size_t pitch,
                                     std::size_t row_bytes, const char *what,
                                     std::string *error_out) {
  const char *label = what ? what : "PTX pitch";
  if (pitch > kSignedInt32PtxPitchMaxBytes) {
    if (error_out) {
      *error_out = std::string(label) +
                   ": pitch exceeds signed 32-bit PTX pitch ABI limit "
                   "(INT32_MAX bytes).";
    }
    return false;
  }

  if (pitch < row_bytes) {
    if (error_out) {
      *error_out = std::string(label) +
                   ": pitch is smaller than row bytes for signed 32-bit PTX "
                   "pitch ABI.";
    }
    return false;
  }

  return true;
}

} // namespace studiocast::cuda::kernels::detail
