#pragma once

#include <string>
#include <string_view>

#include "core/audio/effects/broadcast_audio_effects.h"

namespace studiocast::audio {

// High-level active audio processing backend.
//
// Note: "passthrough" means the pipeline is running but audio is unmodified.
enum class AudioBackendKind {
  kPassthrough,
  kMaxine,
  kOpenSource,
};

inline constexpr std::string_view ToString(AudioBackendKind b) {
  switch (b) {
  case AudioBackendKind::kPassthrough:
    return "passthrough";
  case AudioBackendKind::kMaxine:
    return "maxine";
  case AudioBackendKind::kOpenSource:
    return "open_source";
  }
  return "passthrough";
}

struct AudioBackendAvailability {
  bool maxine_ok = false;
  std::string maxine_reason;

  bool open_source_ok = false;
  std::string open_source_reason;
};

struct AudioBackendDecision {
  AudioBackendKind backend = AudioBackendKind::kPassthrough;

  // True when the user requested an effects backend but we had to fall back.
  bool used_fallback = false;

  // Human-friendly note explaining backend selection and/or fallback.
  // Intended for GUI banners and daemon status.
  std::string note;
};

// Returns true if the user has enabled any microphone effect (regardless of
// backend).
bool AnyMicrophoneEffectRequested(
    const studiocast::audio::effects::BroadcastAudioEffects &fx);

// Returns true if the user has enabled any speaker effect (regardless of
// backend).
bool AnySpeakerEffectRequested(
    const studiocast::audio::effects::BroadcastAudioEffects &fx);

// Returns true if any audio effect is requested (microphone or speaker).
inline bool AnyAudioEffectRequested(
    const studiocast::audio::effects::BroadcastAudioEffects &fx) {
  return AnyMicrophoneEffectRequested(fx) || AnySpeakerEffectRequested(fx);
}

// Deterministically chooses the active backend based on:
//  - fx.engine (AUTO/MAXINE/OPEN_SOURCE/OFF)
//  - whether any effects are requested
//  - backend availability.
AudioBackendDecision
ResolveAudioBackend(const studiocast::audio::effects::BroadcastAudioEffects &fx,
                    const AudioBackendAvailability &avail);

} // namespace studiocast::audio
