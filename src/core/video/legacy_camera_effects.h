#pragma once

#include <cstdint>
#include <filesystem>

#include "core/video/effects/effect_types.h"

namespace studiocast::video {

// Legacy, deprecated runtime effect schema.
//
// This is retained temporarily for migration/compatibility tooling.
// The camera pipeline must consume
// `studiocast::video::effects::BroadcastCameraEffects` instead.
struct CameraEffects {
  // Basic camera transform
  bool mirror = false;

  // NVIDIA Broadcast-style background effects.
  //
  // Product rule: Maxine is the only production effect engine.
  // If Maxine is unavailable/unsupported, these effects are unavailable
  // (no CPU fallback in the camera pipeline).
  studiocast::video::effects::BackgroundEffect background =
      studiocast::video::effects::BackgroundEffect::none;
  studiocast::video::effects::EffectBackend background_backend =
      studiocast::video::effects::EffectBackend::auto_select;

  // Used by background blur (and future AI effects) as an intensity knob.
  int background_strength = 8;

  // Auto Frame (Maxine AR bounding boxes + GPU crop/scale).
  //
  // Enabled when `background == BackgroundEffect::auto_frame`.
  struct AutoFrameSettings {
    // Strength as an integer percentage [0..100].
    // Higher = tighter framing (more zoom).
    int strength = 50;

    // Smoothing as an integer percentage [0..100].
    // Higher = smoother/less jitter, but slower response.
    int smoothing = 70;

    // Fractional extra headroom above the detected subject (0..1).
    float headroom = 0.15f;
  };

  AutoFrameSettings auto_frame{};

  // Virtual background parameters.
  //
  // - remove: subject over a solid color (default black)
  // - replace: subject over a user-provided image
  std::uint32_t background_remove_color_rgb = 0x000000; // 0xRRGGBB
  std::filesystem::path background_replace_image;

  // Video noise removal (Maxine VFX Denoising).
  bool denoise = false;
  // Strength is an integer percentage [0..100].
  int denoise_strength = 50;

  // Green screen (matte generation) parameters used by Maxine VFX.
  // Stored as raw values to avoid build-time dependency on NVIDIA headers.
  struct GreenScreenSettings {
    // NVVFX_MODE: quality/perf mode (numeric value defined by NVIDIA).
    std::uint32_t mode = 0;
    // NVVFX_TEMPORAL: enable temporal consistency.
    bool temporal = true;
  };

  GreenScreenSettings green_screen{};

  // Virtual Key Light (Video Relighting) parameters.
  struct VirtualKeyLightSettings {
    bool enabled = false;

    // Blend amount between relit foreground and the original image.
    // 0 = no effect, 1 = fully relit foreground.
    float intensity = 0.7f; // [0..1]

    // Temperature preset selects an HDRI variant.
    // 0 = neutral, 1 = warm, 2 = cool.
    int temperature_preset = 0;

    // Optional direction control (pan angle, degrees).
    float direction_pan_degrees = 0.0f;

    // Optional HDRI override. Empty = auto/default.
    std::filesystem::path hdri_path;
  };

  VirtualKeyLightSettings virtual_key_light{};

  // Eye Contact (Maxine AR Gaze Redirection) parameters.
  struct EyeContactSettings {
    bool enabled = false;

    // Strength is an integer percentage [0..100].
    int strength = 50;

    // Enable randomized look-away micro movements.
    bool look_away_enabled = true;
  };

  EyeContactSettings eye_contact{};

  // Vignette (CUDA GPU post-process).
  struct VignetteSettings {
    bool enabled = false;

    // Strength of the vignette. 0 = no-op, 1 = strong darkening.
    float intensity = 0.35f; // [0..1]

    // If true and Auto Frame has a valid tracked rect, center the
    // vignette around the tracked subject.
    bool center_on_tracked_face = true;
  };

  VignetteSettings vignette{};
};

inline bool operator==(const CameraEffects &a, const CameraEffects &b) {
  return a.mirror == b.mirror && a.background == b.background &&
         a.background_backend == b.background_backend &&
         a.background_strength == b.background_strength &&
         a.auto_frame.strength == b.auto_frame.strength &&
         a.auto_frame.smoothing == b.auto_frame.smoothing &&
         a.auto_frame.headroom == b.auto_frame.headroom &&
         a.background_remove_color_rgb == b.background_remove_color_rgb &&
         a.background_replace_image == b.background_replace_image &&
         a.denoise == b.denoise && a.denoise_strength == b.denoise_strength &&
         a.green_screen.mode == b.green_screen.mode &&
         a.green_screen.temporal == b.green_screen.temporal &&
         a.virtual_key_light.enabled == b.virtual_key_light.enabled &&
         a.virtual_key_light.intensity == b.virtual_key_light.intensity &&
         a.virtual_key_light.temperature_preset ==
             b.virtual_key_light.temperature_preset &&
         a.virtual_key_light.direction_pan_degrees ==
             b.virtual_key_light.direction_pan_degrees &&
         a.virtual_key_light.hdri_path == b.virtual_key_light.hdri_path &&
         a.eye_contact.enabled == b.eye_contact.enabled &&
         a.eye_contact.strength == b.eye_contact.strength &&
         a.eye_contact.look_away_enabled == b.eye_contact.look_away_enabled &&
         a.vignette.enabled == b.vignette.enabled &&
         a.vignette.intensity == b.vignette.intensity &&
         a.vignette.center_on_tracked_face == b.vignette.center_on_tracked_face;
}

inline bool operator!=(const CameraEffects &a, const CameraEffects &b) {
  return !(a == b);
}

} // namespace studiocast::video
