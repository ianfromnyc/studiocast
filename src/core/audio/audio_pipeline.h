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
  std::uint32_t frame_samples = 480; // 10ms @ 48kHz
  std::uint32_t channels = 1;
};

struct AudioPipelineStats {
  bool running = false;
  std::uint64_t frames_processed = 0;

  // Time spent inside AudioProcessor::Process().
  std::uint64_t process_time_us_sum = 0;
  std::uint64_t process_time_us_max = 0;
  std::uint64_t process_time_us_last = 0;

  // Number of frames where the processing time exceeded the frame duration.
  std::uint64_t process_overruns = 0;

  std::string last_error;
};

// Real-time audio pipeline:
//  Pulse (capture) -> AudioProcessor (Process) -> Pulse (playback into a sink).
//
// MVP format: float32 @ 48kHz, 10ms frames (1ch for mic, 2ch supported for
// speakers).
class AudioPipeline {
public:
  explicit AudioPipeline(AudioProcessor *processor);
  ~AudioPipeline();

  AudioPipeline(const AudioPipeline &) = delete;
  AudioPipeline &operator=(const AudioPipeline &) = delete;

  bool Start(const AudioPipelineConfig &cfg, std::string *error);
  void Stop();

  AudioPipelineStats GetStats() const;

private:
  void ThreadMain(AudioPipelineConfig cfg);
  void SetLastError(std::string msg);

  AudioProcessor *processor_ = nullptr; // not owned

  // Only used to guard the last_error string.
  mutable std::mutex mu_;
  std::string last_error_;

  // Hot-path stats are atomics to avoid lock contention on the real-time
  // thread.
  std::atomic<bool> running_{false};
  std::atomic<std::uint64_t> frames_processed_{0};
  std::atomic<std::uint64_t> process_time_us_sum_{0};
  std::atomic<std::uint64_t> process_time_us_max_{0};
  std::atomic<std::uint64_t> process_time_us_last_{0};
  std::atomic<std::uint64_t> process_overruns_{0};

  std::atomic<bool> stop_{false};
  std::thread thread_;
};

} // namespace studiocast::audio
