#include "core/maxine/reason_codes.h"

#include <string>

namespace studiocast::maxine::reasons {

static bool StartsWith(std::string_view s, std::string_view prefix) {
  return s.size() >= prefix.size() && s.substr(0, prefix.size()) == prefix;
}

std::string ToEnglish(std::string_view code) {
  if (code == kNone) {
    return "OK";
  }

  if (code == kGpuMissing) {
    return "No NVIDIA GPU detected.";
  }
  if (code == kGpuNotSelected) {
    return "No NVIDIA GPU selected.";
  }
  if (code == kGpuUnsupported) {
    return "Selected NVIDIA GPU unsupported for Maxine.";
  }
  if (code == kGpuSelectionFailed) {
    return "Failed to select an NVIDIA GPU for Maxine.";
  }

  if (code == kDriverMissing) {
    return "NVIDIA driver not detected.";
  }
  if (code == kDriverTooOld) {
    return "NVIDIA driver too old for Maxine.";
  }

  if (code == kMissingVfxSdk) {
    return "Maxine VideoFX SDK not found.";
  }
  if (code == kMissingArSdk) {
    return "Maxine AR SDK not found.";
  }
  if (code == kMissingAfxSdk) {
    return "Maxine Audio Effects SDK not found.";
  }

  if (StartsWith(code, kMissingVfxFeaturePrefix)) {
    const auto feat = code.substr(kMissingVfxFeaturePrefix.size());
    return "Missing VideoFX feature: " + std::string(feat);
  }
  if (StartsWith(code, kMissingArFeaturePrefix)) {
    const auto feat = code.substr(kMissingArFeaturePrefix.size());
    return "Missing AR feature: " + std::string(feat);
  }
  if (StartsWith(code, kMissingAfxFeaturePrefix)) {
    const auto feat = code.substr(kMissingAfxFeaturePrefix.size());
    return "Missing Audio Effects feature: " + std::string(feat);
  }

  // Allow suffixes like "dlopen_failed:vfx".
  if (StartsWith(code, kDlopenFailed)) {
    return "Maxine SDK library could not be loaded.";
  }
  if (StartsWith(code, kSymbolMissing)) {
    return "Maxine SDK library missing required symbols.";
  }

  if (code == kUnknown) {
    return "Unavailable (unknown reason).";
  }

  return std::string(code);
}

} // namespace studiocast::maxine::reasons
