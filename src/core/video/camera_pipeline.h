#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/video/effects/broadcast_effects.h"
#include "core/video/v4l2_capture.h"
#include "core/video/v4l2_writer.h"

namespace studiocast::video {

    enum class CaptureMode {
        // Use the requested `width`/`height` (must be > 0).
        requested,

        // Auto-select a good capture mode. `width`/`height` may be <= 0 (sentinel).
        auto_best,
    };

    enum class ScalingBackendPreference {
        auto_select,
        cpu,
        gpu,
    };

    struct CameraPipelineConfig {
        std::string input_device;   // e.g. /dev/video0
        std::string output_device;  // e.g. /dev/video10 (v4l2loopback)

        CaptureMode capture_mode = CaptureMode::requested;

        int width = 1280;
        int height = 720;
        int fps = 30;

        bool prefer_mjpeg = true;

        // Output scaling backend selection.
        // - auto_select: use GPU scaling when available, otherwise CPU.
        // - cpu: force CPU scaling.
        // - gpu: prefer GPU scaling.
        //
        // Note: CPU resize can be hard-disabled via `allow_cpu_resize` to prevent
        // silent high-latency scaling paths.
        ScalingBackendPreference scaling_backend = ScalingBackendPreference::auto_select;

        // When false (default), the pipeline will NOT perform CPU resizing when
        // output dimensions differ from the source frame.
        //
        // If an output resize is required and GPU resize is unavailable, the
        // pipeline reports an explicit error and stops instead of silently
        // spending tens of milliseconds per frame.
        bool allow_cpu_resize = false;

        studiocast::video::effects::BroadcastCameraEffects effects{};
    };

    struct CameraPipelineStatus {
        bool running = false;
        bool starting = false;

        std::string input_device;
        std::string output_device;

        CaptureFormat capture{};
        ActualFormat output{};

        std::string scaling_backend_active;  // "cpu" | "gpu" (empty when idle)
        CaptureFormat scaling_from{};
        ActualFormat scaling_to{};

        int frame_index = 0;

        struct MsPerFrame {
            double capture = 0.0;
            double scale = 0.0;
            double effects = 0.0;
            double write = 0.0;
        };

        // Rolling averages of per-frame stage times (milliseconds).
        MsPerFrame ms_per_frame{};
        double fps_actual = 0.0;
        int perf_sample_frames = 0;

        // Debug/status for effects.
        std::string effects_backends;  // e.g. "mirror:builtin,virtual_background.blur:maxine"
        std::string effects_note;      // e.g. "Maxine requested but unavailable; effects disabled"

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
        void SetEffects(const studiocast::video::effects::BroadcastCameraEffects& effects);

        // Convenience for legacy callers.
        void SetMirrorEnabled(bool enabled);

    private:
        // Opens (or reuses) the loopback writer.
        //
        // If `out_opened_or_renegotiated` is non-null, it will be set to true when we actually
        // performed an open/renegotiation (i.e. the output may have been reset), and false when
        // the existing writer was reused without changes.
        bool OpenOutputLocked(const std::string& outDev,
                              int width,
                              int height,
                              int fps,
                              bool* out_opened_or_renegotiated,
                              std::string* error);

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
        std::string scaling_backend_active_;
        CaptureFormat scaling_from_{};
        ActualFormat scaling_to_{};
        int frame_index_ = 0;

        CameraPipelineStatus::MsPerFrame ms_per_frame_{};
        double fps_actual_ = 0.0;
        int perf_sample_frames_ = 0;

        // Effects: updated live by SetEffects.
        mutable std::mutex effects_mu_;
        studiocast::video::effects::BroadcastCameraEffects effects_{};

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
