#pragma once

#include "diagnostics.h"

namespace studiocast::open_cuda {

// Builds a diagnostic snapshot for the Open CUDA backend using the default
// model pack location.
//
// Intended to be lightweight and deterministic (safe to call in a polling loop
// with caching).
OpenCudaDiagnostics DiagnoseOpenCudaDefault();

} // namespace studiocast::open_cuda
