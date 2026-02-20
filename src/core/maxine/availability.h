#pragma once

#include <string>
#include <vector>

namespace studiocast::maxine {

struct MaxineDiagnostics;

// Which Maxine component is required for the caller's blocked-message.
enum class MaxineNeed {
  any,
  vfx,
  ar,
  afx,
};

struct CanonicalMaxineBlockedCopy {
  std::string summary;
  std::vector<std::string> steps; // 1–3 actionable next steps
};

// Builds the canonical "Maxine unavailable: ..." summary + remediation bullets.
// Pure formatting only: does not probe the system.
CanonicalMaxineBlockedCopy
BuildCanonicalMaxineBlockedCopy(const MaxineDiagnostics &d,
                                MaxineNeed need = MaxineNeed::any);

// Formats `summary` + `steps` into a single human-friendly multi-line string.
std::string
FormatCanonicalMaxineBlockedCopy(const CanonicalMaxineBlockedCopy &c);

// Compile-time availability of the Maxine backend.
//
// StudioCast's long-term plan is to load Maxine features dynamically (dlopen)
// so that CPU-only builds still work.
//
// For now this is a simple build-time flag.
bool BackendBuilt();

// Runtime availability check (best-effort).
// Returns false with a human-friendly reason if unavailable.
bool RuntimeAvailable(std::string *reason);

} // namespace studiocast::maxine
