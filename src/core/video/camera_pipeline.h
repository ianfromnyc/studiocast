#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/video/effects/effect_types.h"
#include "core/video/v4l2_capture.h"
#include "core/video/v4l2_writer.h"

namespace studiocast::video {

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
        std::uint32_t background_remove_color_rgb = 0x000000;  // 0xRRGGBB
        std::filesystem::path background_replace_image;

        // Video noise removal (Maxine VFX Denoising).
        //
        // Pipeline format strategy (see docs/task notes): baseline GPU images are
        // BGRu8 chunky; when denoise is enabled we insert a Transfer stage to
        // convert to BGRf32 planar normalized, run Denoising, then Transfer back.
        bool denoise = false;
        // Strength is an integer percentage [0..100]. The VFX effect supports
        // discrete strength levels, so the runtime quantizes to supported steps.
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
        //
        // These are used by the Maxine VFX relighting stage. If Maxine/VFX is
        // unavailable, the daemon should report "functionality unavailable"
        // rather than silently falling back.
        struct VirtualKeyLightSettings {
            bool enabled = false;

            // Blend amount between relit foreground and the original image.
            // 0 = no effect, 1 = fully relit foreground.
            float intensity = 0.7f;  // [0..1]

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

            // Strength is an integer percentage [0..100]. The AR feature may
            // expose different underlying knobs; the runtime performs best-effort
            // mapping.
            int strength = 50;

            // Enable randomized look-away micro movements.
            bool look_away_enabled = true;
        };

        EyeContactSettings eye_contact{};

        // Vignette (CUDA GPU post-process).
        //
        // This is not Maxine-specific, but we only enable it when the Maxine GPU
        // pipeline is active (no CPU fallback stance).
        struct VignetteSettings {
            bool enabled = false;

            // Strength of the vignette. 0 = no-op, 1 = strong darkening.
            float intensity = 0.35f;  // [0..1]

            // If true and Auto Frame has a valid tracked rect, center the
            // vignette around the tracked subject.
            bool center_on_tracked_face = true;
        };

        VignetteSettings vignette{};
    };

    inline bool operator==(const CameraEffects& a, const CameraEffects& b) {
        return a.mirror == b.mirror &&
               a.background == b.background &&
               a.background_backend == b.background_backend &&
               a.background_strength == b.background_strength &&
               a.auto_frame.strength == b.auto_frame.strength &&
               a.auto_frame.smoothing == b.auto_frame.smoothing &&
               a.auto_frame.headroom == b.auto_frame.headroom &&
               a.background_remove_color_rgb == b.background_remove_color_rgb &&
               a.background_replace_image == b.background_replace_image &&
               a.denoise == b.denoise &&
               a.denoise_strength == b.denoise_strength &&
               a.green_screen.mode == b.green_screen.mode &&
               a.green_screen.temporal == b.green_screen.temporal &&
               a.virtual_key_light.enabled == b.virtual_key_light.enabled &&
               a.virtual_key_light.intensity == b.virtual_key_light.intensity &&
               a.virtual_key_light.temperature_preset == b.virtual_key_light.temperature_preset &&
               a.virtual_key_light.direction_pan_degrees == b.virtual_key_light.direction_pan_degrees &&
               a.virtual_key_light.hdri_path == b.virtual_key_light.hdri_path &&
               a.eye_contact.enabled == b.eye_contact.enabled &&
               a.eye_contact.strength == b.eye_contact.strength &&
               a.eye_contact.look_away_enabled == b.eye_contact.look_away_enabled &&
               a.vignette.enabled == b.vignette.enabled &&
               a.vignette.intensity == b.vignette.intensity &&
               a.vignette.center_on_tracked_face == b.vignette.center_on_tracked_face;
    }

    inline bool operator!=(const CameraEffects& a, const CameraEffects& b) { return !(a == b); }

    struct CameraPipelineConfig {
        std::string input_device;   // e.g. /dev/video0
        std::string output_device;  // e.g. /dev/video10 (v4l2loopback)

        int width = 1280;
        int height = 720;
        int fps = 30;

        CameraEffects effects{};
    };

    struct CameraPipelineStatus {
        bool running = false;
        bool starting = false;

        std::string input_device;
        std::string output_device;

        CaptureFormat capture{};
        ActualFormat output{};

        int frame_index = 0;

        // Debug/status for effects.
        std::string effects_backends;  // e.g. "mirror:cpu,background_blur:cpu"
        std::string effects_note;      // e.g. "Maxine requested but unavailable; using CPU placeholder"

        std::string last_error;
    };

    class CameraPipeline final {
    public:
        CameraPipeline() = default;
        ~CameraPipeline();

        CameraPipeline(const CameraPipeline&) = delete;
        CameraPipeline& operator=(const CameraPipeline&) = delete;

        bool Start(const CameraPipelineConfig& cfg, std::string* error);
        void Stop();

        // Opens (and keeps open) the v4l2loopback output device without starting
        // camera capture / processing.
        //
        // This is important when v4l2loopback is loaded with exclusive_caps=1:
        // many applications will not list the device as a capture source unless a
        // producer has it open.
        bool EnsureOutputOpen(const CameraPipelineConfig& cfg, std::string* error);
        void CloseOutput();

        CameraPipelineStatus Status() const;

        // Live update of effects while running.
        void SetEffects(const CameraEffects& effects);

        // Convenience for legacy callers.
        void SetMirrorEnabled(bool enabled);

    private:
        bool OpenOutputLocked(const std::string& outDev, int width, int height, int fps, std::string* error);

        void ThreadMain(CameraPipelineConfig cfg);

        mutable std::mutex mu_;
        std::condition_variable cv_;
        std::thread th_;
        std::atomic_bool stop_{false};

        bool running_ = false;
        bool starting_ = false;
        bool start_notified_ = false;

        std::string input_device_;
        std::string output_device_;
        CaptureFormat capture_{};
        ActualFormat output_{};
        int frame_index_ = 0;

        // Effects: updated live by SetEffects.
        mutable std::mutex effects_mu_;
        CameraEffects effects_{};

        // Effect runtime info (written by pipeline thread when effects chain changes).
        std::string effects_backends_;
        std::string effects_note_;

        std::string last_error_;

        // Keep the output open across starts/stops to avoid v4l2loopback edge
        // cases (especially with exclusive_caps=1) and to make the virtual
        // camera visible to apps even when we're idle.
        V4l2Writer writer_;
        std::string writer_device_;
    };

}  // namespace studiocast::video
