#pragma once

#include <string>
#include <string_view>

namespace studiocast::audio::effects {

inline constexpr int kBroadcastAudioEffectsSchemaVersion = 2;

enum class SuperresMode {
    k8kTo16k,
    k16kTo48k,
};

inline constexpr std::string_view ToString(SuperresMode m) {
    switch (m) {
        case SuperresMode::k8kTo16k: return "8k_to_16k";
        case SuperresMode::k16kTo48k: return "16k_to_48k";
    }
    return "16k_to_48k";
}

inline bool TryParseSuperresMode(std::string_view s, SuperresMode* out) {
    if (!out) return false;
    if (s == "8k_to_16k") {
        *out = SuperresMode::k8kTo16k;
        return true;
    }
    if (s == "16k_to_48k") {
        *out = SuperresMode::k16kTo48k;
        return true;
    }
    return false;
}

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

    struct Aec {
        bool enabled = false;
        // Pulse source name, typically a monitor source.
        std::string reference_source;
    };

    struct Superres {
        bool enabled = false;
        SuperresMode mode = SuperresMode::k16kTo48k;
    };

    Aec aec{};
    Superres superres{};
};

inline bool operator==(const BroadcastMicrophoneEffects& a, const BroadcastMicrophoneEffects& b) {
    return a.noise_removal_enabled == b.noise_removal_enabled &&
           a.room_echo_removal_enabled == b.room_echo_removal_enabled &&
           a.strength == b.strength &&
           a.studio_voice_enabled == b.studio_voice_enabled &&
           a.aec.enabled == b.aec.enabled &&
           a.aec.reference_source == b.aec.reference_source &&
           a.superres.enabled == b.superres.enabled &&
           a.superres.mode == b.superres.mode;
}

inline bool operator!=(const BroadcastMicrophoneEffects& a, const BroadcastMicrophoneEffects& b) { return !(a == b); }

struct BroadcastSpeakerEffects {
    bool noise_removal_enabled = false;

    // 0..100-ish user knob (implementation-defined).
    int strength = 50;

    struct Superres {
        bool enabled = false;
        SuperresMode mode = SuperresMode::k16kTo48k;
    };

    Superres superres{};
};

inline bool operator==(const BroadcastSpeakerEffects& a, const BroadcastSpeakerEffects& b) {
    return a.noise_removal_enabled == b.noise_removal_enabled && a.strength == b.strength &&
           a.superres.enabled == b.superres.enabled && a.superres.mode == b.superres.mode;
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
