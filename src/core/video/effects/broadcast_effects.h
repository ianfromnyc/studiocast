#pragma once

#include <string>

namespace studiocast::video::effects {

inline constexpr int kBroadcastEffectsSchemaVersion = 1;

// User-facing preference for effect engine selection.
// Note: CPU is intentionally not represented here.
enum class EffectsEnginePreference {
    auto_select = 0,
    maxine = 1,
};

std::string ToString(EffectsEnginePreference v);
bool ParseEffectsEnginePreference(const std::string& s, EffectsEnginePreference* out);

enum class VirtualBackgroundMode {
    none = 0,
    blur = 1,
    remove = 2,
    replace = 3,
};

std::string ToString(VirtualBackgroundMode v);
bool ParseVirtualBackgroundMode(const std::string& s, VirtualBackgroundMode* out);

struct VirtualBackgroundSettings {
    VirtualBackgroundMode mode = VirtualBackgroundMode::none;

    // Used by blur (and future AI effects) as an intensity knob.
    // Interpreted as a blur radius for the current CPU placeholder.
    int strength = 8;

    // Used when mode==replace.
    std::string replace_path;
};

inline bool operator==(const VirtualBackgroundSettings& a, const VirtualBackgroundSettings& b) {
    return a.mode == b.mode && a.strength == b.strength && a.replace_path == b.replace_path;
}

inline bool operator!=(const VirtualBackgroundSettings& a, const VirtualBackgroundSettings& b) { return !(a == b); }

struct AutoFrameSettings {
    bool enabled = false;

    // 0..100-ish user knob (implementation-defined).
    int strength = 50;

    // 0..100-ish smoothing (implementation-defined).
    int smoothing = 50;
};

inline bool operator==(const AutoFrameSettings& a, const AutoFrameSettings& b) {
    return a.enabled == b.enabled && a.strength == b.strength && a.smoothing == b.smoothing;
}

inline bool operator!=(const AutoFrameSettings& a, const AutoFrameSettings& b) { return !(a == b); }

struct EyeContactSettings {
    bool enabled = false;
    int strength = 50;
    bool look_away_enabled = true;
};

inline bool operator==(const EyeContactSettings& a, const EyeContactSettings& b) {
    return a.enabled == b.enabled && a.strength == b.strength && a.look_away_enabled == b.look_away_enabled;
}

inline bool operator!=(const EyeContactSettings& a, const EyeContactSettings& b) { return !(a == b); }

struct VideoNoiseRemovalSettings {
    bool enabled = false;
    int strength = 50;
};

inline bool operator==(const VideoNoiseRemovalSettings& a, const VideoNoiseRemovalSettings& b) {
    return a.enabled == b.enabled && a.strength == b.strength;
}

inline bool operator!=(const VideoNoiseRemovalSettings& a, const VideoNoiseRemovalSettings& b) { return !(a == b); }

struct VirtualKeyLightSettings {
    bool enabled = false;
    int intensity = 50;

    // In Kelvin (roughly). UI may map presets.
    int temperature = 4500;
};

inline bool operator==(const VirtualKeyLightSettings& a, const VirtualKeyLightSettings& b) {
    return a.enabled == b.enabled && a.intensity == b.intensity && a.temperature == b.temperature;
}

inline bool operator!=(const VirtualKeyLightSettings& a, const VirtualKeyLightSettings& b) { return !(a == b); }

struct VignetteSettings {
    bool enabled = false;
    int intensity = 25;
};

inline bool operator==(const VignetteSettings& a, const VignetteSettings& b) {
    return a.enabled == b.enabled && a.intensity == b.intensity;
}

inline bool operator!=(const VignetteSettings& a, const VignetteSettings& b) { return !(a == b); }

// Canonical, versioned representation of Broadcast-style camera effect settings.
// This type is intended to be used across config persistence, IPC, pipeline config,
// and GUI state.
struct BroadcastCameraEffects {
    int schema_version = kBroadcastEffectsSchemaVersion;

    bool mirror = false;

    // Global engine preference for effects that can run on Maxine.
    EffectsEnginePreference engine = EffectsEnginePreference::auto_select;

    VirtualBackgroundSettings virtual_background{};
    AutoFrameSettings auto_frame{};
    EyeContactSettings eye_contact{};
    VideoNoiseRemovalSettings video_noise_removal{};
    VirtualKeyLightSettings virtual_key_light{};
    VignetteSettings vignette{};
};

inline bool operator==(const BroadcastCameraEffects& a, const BroadcastCameraEffects& b) {
    return a.schema_version == b.schema_version &&
           a.mirror == b.mirror &&
           a.engine == b.engine &&
           a.virtual_background == b.virtual_background &&
           a.auto_frame == b.auto_frame &&
           a.eye_contact == b.eye_contact &&
           a.video_noise_removal == b.video_noise_removal &&
           a.virtual_key_light == b.virtual_key_light &&
           a.vignette == b.vignette;
}

inline bool operator!=(const BroadcastCameraEffects& a, const BroadcastCameraEffects& b) { return !(a == b); }

}  // namespace studiocast::video::effects
