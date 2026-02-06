#pragma once

#include <string>
#include <vector>

#include "core/audio/effects/broadcast_audio_effects.h"

namespace studiocast::audio::effects {

struct PlannedBroadcastAecEffect {
    bool enabled = false;

    // Planned reference source name (Pulse source name). Empty if disabled.
    std::string reference_source;

    // If `enabled` is false, a deterministic human-readable reason string.
    // Intended for logs/GUI.
    std::string reason;
};

struct PlannedBroadcastSuperresEffect {
    bool enabled = false;
    SuperresMode mode = SuperresMode::k16kTo48k;
    std::string reason;
};

struct PlannedBroadcastAudioEffects {
    PlannedBroadcastAecEffect microphone_aec;
    PlannedBroadcastSuperresEffect microphone_superres;
    PlannedBroadcastSuperresEffect speaker_superres;
};

struct BroadcastAudioEffectsPlanInputs {
    // If non-empty, used to validate AEC reference sources.
    std::vector<std::string> available_pulse_sources;

    // Maxine AFX MVP currently assumes float PCM.
    bool float32_pcm = true;
};

// Deterministic validation/planning of the canonical Broadcast audio settings model.
//
// This planner does not perform environment probing on its own; callers should
// provide any environment facts (e.g. Pulse source list).
PlannedBroadcastAudioEffects PlanBroadcastAudioEffects(const BroadcastAudioEffects& fx,
                                                      const BroadcastAudioEffectsPlanInputs& in);

}  // namespace studiocast::audio::effects
