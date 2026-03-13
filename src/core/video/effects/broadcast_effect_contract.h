#pragma once

#include <string_view>

namespace studiocast::video::effects::contract {

// Canonical effect model contract (stable IDs + parameter IDs).
//
// Hard rule: These IDs are stable across daemon status, IPC/JSON, GUI, and CLI.
// Do not rename existing IDs. If an effect must be replaced, add a new ID.
//
// Effect IDs (stable):
//   - mirror
//   - auto_frame
//   - eye_contact
//   - video_noise_removal
//   - virtual_key_light
//   - vignette
//   - virtual_background.blur
//   - virtual_background.remove
//   - virtual_background.replace
//
// Mutual exclusion:
//   - virtual_background.* are mutually exclusive with each other
//   - auto_frame can run alongside virtual_background.* (daemon decides
//   ordering)

// ---- Effect IDs ----
inline constexpr std::string_view kEffectIdMirror = "mirror";

inline constexpr std::string_view kEffectIdVirtualBackgroundBlur =
    "virtual_background.blur";
inline constexpr std::string_view kEffectIdVirtualBackgroundRemove =
    "virtual_background.remove";
inline constexpr std::string_view kEffectIdVirtualBackgroundReplace =
    "virtual_background.replace";

inline constexpr std::string_view kEffectIdAutoFrame = "auto_frame";
inline constexpr std::string_view kEffectIdEyeContact = "eye_contact";

inline constexpr std::string_view kEffectIdVideoNoiseRemoval =
    "video_noise_removal";
inline constexpr std::string_view kEffectIdVirtualKeyLight =
    "virtual_key_light";
inline constexpr std::string_view kEffectIdVignette = "vignette";

// ---- Mutex groups (stable) ----
inline constexpr std::string_view kMutexGroupVirtualBackgroundMode =
    "virtual_background_mode";
inline constexpr std::string_view kMutexGroupBackgroundOrAutoFrame =
    "background_or_auto_frame";

// ---- Parameter IDs (stable) ----
namespace param {
inline constexpr std::string_view kEnabled = "enabled";

// Generic common knobs.
inline constexpr std::string_view kStrength = "strength";
inline constexpr std::string_view kSmoothing = "smoothing";
inline constexpr std::string_view kHeadroom = "headroom"; // float 0..1

// Open CUDA model pack selection (where applicable).
inline constexpr std::string_view kModelId = "model_id";

// Virtual background.
inline constexpr std::string_view kVbRemoveColor = "remove_color"; // "#RRGGBB"
inline constexpr std::string_view kVbReplacePath = "replace_path";
inline constexpr std::string_view kGreenscreenMode = "greenscreen_mode";
inline constexpr std::string_view kGreenscreenTemporal = "greenscreen_temporal";

// Eye contact.
inline constexpr std::string_view kLookAwayEnabled = "look_away_enabled";

// Virtual key light.
inline constexpr std::string_view kIntensity =
    "intensity"; // integer percent 0..100
inline constexpr std::string_view kTemperaturePreset =
    "temperature_preset"; // "neutral"|"warm"|"cool"
inline constexpr std::string_view kDirectionPanDegrees =
    "direction_pan_degrees"; // degrees -180..180
inline constexpr std::string_view kHdriPath = "hdri_path";

// Vignette.
inline constexpr std::string_view kCenterOnTrackedFace =
    "center_on_tracked_face";
} // namespace param

// ---- Ranges / defaults ----
// These are the canonical ranges/defaults for IPC/UI and should match
// `CameraEffects` defaults and Maxine expectations.

// Virtual background blur strength (and shared VB strength knob).
inline constexpr int kVbStrengthMin = 1;
inline constexpr int kVbStrengthMax = 64;
inline constexpr int kVbStrengthDefault = 8;

// Auto frame.
inline constexpr int kAutoFrameStrengthMin = 0;
inline constexpr int kAutoFrameStrengthMax = 100;
inline constexpr int kAutoFrameStrengthDefault = 50;
inline constexpr int kAutoFrameSmoothingDefault = 70;
inline constexpr float kAutoFrameHeadroomMin = 0.0f;
inline constexpr float kAutoFrameHeadroomMax = 1.0f;
inline constexpr float kAutoFrameHeadroomDefault = 0.15f;

// Eye contact.
inline constexpr int kEyeContactStrengthDefault = 50;

// Video noise removal.
inline constexpr int kVideoNoiseRemovalStrengthDefault = 50;

// Virtual key light.
inline constexpr int kVirtualKeyLightIntensityDefault = 70;
inline constexpr int kVirtualKeyLightPanMin = -180;
inline constexpr int kVirtualKeyLightPanMax = 180;

// Vignette.
inline constexpr int kVignetteIntensityDefault = 35;

// Green screen defaults.
inline constexpr int kGreenscreenModeDefault = 0;
inline constexpr bool kGreenscreenTemporalDefault = true;

} // namespace studiocast::video::effects::contract
