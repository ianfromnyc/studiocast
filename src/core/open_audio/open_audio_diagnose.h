#pragma once

#include "core/open_audio/open_audio_diagnostics.h"

namespace studiocast::open_audio {

// Builds a diagnostic snapshot for the Open Audio backend using the default
// model pack location.
//
// Intended to be lightweight and deterministic (safe to call in a polling loop
// with caching).
OpenAudioDiagnostics DiagnoseOpenAudioDefault();

}  // namespace studiocast::open_audio
