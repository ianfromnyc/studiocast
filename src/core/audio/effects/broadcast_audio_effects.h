#pragma once

namespace studiocast::audio::effects {

inline constexpr int kBroadcastAudioEffectsSchemaVersion = 1;

// Canonical, versioned representation of Broadcast-style audio effect settings.
// This type is intended to be used across config persistence, IPC, and GUI state.

struct BroadcastMicrophoneEffects {
    // Broadcast-style mic processing knobs.
    bool noise_removal_enabled = false;
    bool room_echo_removal_enabled = false;

    // Shared 0..100-ish knob used by both noise removal and echo removal.
    int strength = 50;

    // Mutually exclusive with (noise_removal_enabled || room_echo_removal_enabled).
    bool studio_voice_enabled = false;
};

inline bool operator==(const BroadcastMicrophoneEffects& a, const BroadcastMicrophoneEffects& b) {
    return a.noise_removal_enabled == b.noise_removal_enabled &&
           a.room_echo_removal_enabled == b.room_echo_removal_enabled &&
           a.strength == b.strength &&
           a.studio_voice_enabled == b.studio_voice_enabled;
}

inline bool operator!=(const BroadcastMicrophoneEffects& a, const BroadcastMicrophoneEffects& b) { return !(a == b); }

struct BroadcastSpeakerEffects {
    bool noise_removal_enabled = false;

    // 0..100-ish user knob (implementation-defined).
    int strength = 50;
};

inline bool operator==(const BroadcastSpeakerEffects& a, const BroadcastSpeakerEffects& b) {
    return a.noise_removal_enabled == b.noise_removal_enabled && a.strength == b.strength;
}

inline bool operator!=(const BroadcastSpeakerEffects& a, const BroadcastSpeakerEffects& b) { return !(a == b); }

struct BroadcastAudioEffects {
    int schema_version = kBroadcastAudioEffectsSchemaVersion;

    BroadcastMicrophoneEffects microphone{};
    BroadcastSpeakerEffects speaker{};
};

inline bool operator==(const BroadcastAudioEffects& a, const BroadcastAudioEffects& b) {
    return a.schema_version == b.schema_version && a.microphone == b.microphone && a.speaker == b.speaker;
}

inline bool operator!=(const BroadcastAudioEffects& a, const BroadcastAudioEffects& b) { return !(a == b); }

}  // namespace studiocast::audio::effects
