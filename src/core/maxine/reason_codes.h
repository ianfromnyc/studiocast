#pragma once

#include <string>
#include <string_view>

namespace studiocast::maxine::reasons {

// Stable reason codes for Maxine availability / missing reasons.
//
// These strings are part of the daemon<->GUI/CLI contract. Keep them stable.

// Maxine is supported / not blocked.
inline constexpr std::string_view kNone = "none";

// GPU / driver.
inline constexpr std::string_view kGpuMissing = "gpu_missing";
inline constexpr std::string_view kGpuNotSelected = "gpu_not_selected";
inline constexpr std::string_view kGpuUnsupported = "gpu_unsupported";
inline constexpr std::string_view kGpuSelectionFailed = "gpu_selection_failed";

inline constexpr std::string_view kDriverMissing = "driver_missing";
inline constexpr std::string_view kDriverTooOld = "driver_too_old";

// SDK/component discovery.
inline constexpr std::string_view kMissingVfxSdk = "missing_vfx_sdk";
inline constexpr std::string_view kMissingArSdk = "missing_ar_sdk";
inline constexpr std::string_view kMissingAfxSdk = "missing_afx_sdk";

// Dynamic loading failures.
//
// May include a component suffix (e.g. "dlopen_failed:vfx").
inline constexpr std::string_view kDlopenFailed = "dlopen_failed";
inline constexpr std::string_view kSymbolMissing = "symbol_missing";

// Feature install markers.
//
// These are prefixed reason codes with a stable suffix:
//   missing_vfx_feature:<feature_id>
//   missing_ar_feature:<feature_id>
//   missing_afx_feature:<feature_id>
inline constexpr std::string_view kMissingVfxFeaturePrefix = "missing_vfx_feature:";
inline constexpr std::string_view kMissingArFeaturePrefix = "missing_ar_feature:";
inline constexpr std::string_view kMissingAfxFeaturePrefix = "missing_afx_feature:";

// Fallback.
inline constexpr std::string_view kUnknown = "unknown";

inline std::string MissingVfxFeature(std::string_view id) {
  return std::string(kMissingVfxFeaturePrefix) + std::string(id);
}

inline std::string MissingArFeature(std::string_view id) {
  return std::string(kMissingArFeaturePrefix) + std::string(id);
}

inline std::string MissingAfxFeature(std::string_view id) {
  return std::string(kMissingAfxFeaturePrefix) + std::string(id);
}

// Returns a short, English, human-friendly one-liner for a reason code.
// Unknown codes are returned as-is.
std::string ToEnglish(std::string_view code);

} // namespace studiocast::maxine::reasons
