#pragma once

#include <string>

namespace studiocast::maxine {

// Compile-time availability of the Maxine backend.
//
// StudioCast's long-term plan is to load Maxine features dynamically (dlopen)
// so that CPU-only builds still work.
//
// For now this is a simple build-time flag.
bool BackendBuilt();

// Runtime availability check (best-effort).
// Returns false with a human-friendly reason if unavailable.
bool RuntimeAvailable(std::string* reason);

}  // namespace studiocast::maxine
