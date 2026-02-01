#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

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

    private:
        void ThreadMain(CameraPipelineConfig cfg);

        mutable std::mutex mu_;
        std::condition_variable cv_;
        std::thread th_;
        std::atomic_bool stop_{false};

        std::atomic_bool mirror_{false};

        bool running_ = false;
        bool starting_ = false;
        bool start_notified_ = false;

        std::string input_device_;
        std::string output_device_;
        CaptureFormat capture_{};
        ActualFormat output_{};
        int frame_index_ = 0;
        std::string last_error_;
    };

}  // namespace studiocast::video
