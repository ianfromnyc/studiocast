#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

namespace studiocast::audio {

class AudioProcessor;

struct AudioPipelineConfig {
    // Empty = Pulse default source.
    std::string source_name;

    // Target sink to write processed audio to.
    // This should typically be the StudioCast null sink ("studiocast_sink").
    std::string sink_name = "studiocast_sink";

    int sample_rate = 48000;
    std::uint32_t frame_samples = 480;  // 10ms @ 48kHz
    std::uint32_t channels = 1;
};

struct AudioPipelineStats {
    bool running = false;
    std::uint64_t frames_processed = 0;
    std::string last_error;
};

// Real-time audio pipeline:
//  Pulse (capture) -> AudioProcessor (Process) -> Pulse (playback into a sink).
//
// MVP format: mono float32 @ 48kHz, 10ms frames.
class AudioPipeline {
public:
    explicit AudioPipeline(AudioProcessor* processor);
    ~AudioPipeline();

    AudioPipeline(const AudioPipeline&) = delete;
    AudioPipeline& operator=(const AudioPipeline&) = delete;

    bool Start(const AudioPipelineConfig& cfg, std::string* error);
    void Stop();

    AudioPipelineStats GetStats() const;

private:
    void ThreadMain(AudioPipelineConfig cfg);
    void SetLastError(std::string msg);

    AudioProcessor* processor_ = nullptr;  // not owned

    mutable std::mutex mu_;
    AudioPipelineStats stats_;

    std::atomic<bool> stop_{false};
    std::thread thread_;
};

}  // namespace studiocast::audio
