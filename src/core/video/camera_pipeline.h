#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <cstdint>

#include "core/video/v4l2_capture.h"
#include "core/video/v4l2_writer.h"

namespace studiocast::video {

    struct CameraEffects {
        bool mirror = false;
    };

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

        CameraPipelineStatus Status() const;

        void SetMirrorEnabled(bool enabled);
        // Preview support: GUI can poll for the latest processed RGB frame (after effects, before output packing).
        // This keeps Qt out of core. Frames are copied into an internal buffer only when preview is enabled.
        void SetPreviewEnabled(bool enabled);

        // Copies the latest RGB frame into `out_rgb` (Format: packed RGB24, stride = width*3).
        // Returns true if a frame is available.
        bool GetLatestRgbFrame(std::vector<std::uint8_t>* out_rgb,
                               int* out_width,
                               int* out_height,
                               std::size_t* out_stride,
                               std::uint64_t* out_sequence) const;


    private:
        void ThreadMain(CameraPipelineConfig cfg);

        mutable std::mutex mu_;
        std::condition_variable cv_;
        std::thread th_;
        std::atomic_bool stop_{false};

        std::atomic_bool mirror_{false};
        std::atomic_bool preview_enabled_{false};

        // Last processed RGB frame for preview (owned by core; GUI pulls via GetLatestRgbFrame).
        mutable std::mutex preview_mu_;
        std::vector<std::uint8_t> preview_rgb_;
        int preview_w_ = 0;
        int preview_h_ = 0;
        std::size_t preview_stride_ = 0;
        std::uint64_t preview_seq_ = 0;


        bool running_ = false;
        bool starting_ = false;
        bool start_notified_ = false;

        std::string input_device_;
        std::string output_device_;
        CaptureFormat capture_{};
        ActualFormat output_{};
        int frame_index_ = 0;
        std::string last_error_;

        V4l2Writer writer_;
        std::string writer_device_;
    };

}  // namespace studiocast::video
