#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace studiocast::audio {

class AudioProcessor;
struct AudioPipelineConfig;
struct AudioPipelineStats;

class AudioPipelineRunner {
public:
  virtual ~AudioPipelineRunner() = default;

  virtual bool Start(const AudioPipelineConfig &cfg, std::string *error) = 0;
  virtual void Stop() = 0;
  virtual AudioPipelineStats GetStats() const = 0;
};

class AudioPipelineIo {
public:
  virtual ~AudioPipelineIo() = default;

  virtual bool Open(const AudioPipelineConfig &cfg, std::string *error) = 0;
  virtual bool Read(void *dst, std::size_t bytes, std::string *error) = 0;
  virtual bool Write(const void *src, std::size_t bytes,
                     std::string *error) = 0;
  virtual bool GetCaptureLatencyUs(std::uint64_t *latency_us) = 0;
  virtual bool GetPlaybackLatencyUs(std::uint64_t *latency_us) = 0;
  virtual void Flush() = 0;
  virtual void RequestStop() = 0;
};

struct AudioPipelineHooks {
  std::function<std::unique_ptr<AudioPipelineIo>()> create_io;
};

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

  // Best-effort PulseAudio stream latency estimates (microseconds).
  // These are queried periodically (not per-frame) to avoid adding overhead to
  // the real-time loop.
  std::uint64_t pulse_capture_latency_us_last = 0;
  std::uint64_t pulse_playback_latency_us_last = 0;
  // Max observed latency (best-effort). When both capture and playback
  // latencies are available, this tracks their sum.
  std::uint64_t pulse_latency_us_max = 0;

  // Number of times the pipeline attempted to resync by flushing Pulse buffers.
  std::uint64_t resync_events = 0;

  std::string last_error;
};

// Real-time audio pipeline:
//  Pulse (capture) -> AudioProcessor (Process) -> Pulse (playback into a sink).
//
// MVP format: float32 @ 48kHz, 10ms frames (1ch for mic, 2ch supported for
// speakers).
class AudioPipeline final : public AudioPipelineRunner {
public:
  explicit AudioPipeline(AudioProcessor *processor,
                         AudioPipelineHooks hooks = {});
  ~AudioPipeline();

  AudioPipeline(const AudioPipeline &) = delete;
  AudioPipeline &operator=(const AudioPipeline &) = delete;

  bool Start(const AudioPipelineConfig &cfg, std::string *error) override;
  void Stop() override;

  AudioPipelineStats GetStats() const override;

private:
  void ThreadMain(AudioPipelineConfig cfg);
  void SetLastError(std::string msg);
  std::unique_ptr<AudioPipelineIo> CreateIo() const;
  AudioPipelineIo *GetActiveIo() const;

  AudioProcessor *processor_ = nullptr; // not owned
  AudioPipelineHooks hooks_;

  // Only used to guard the last_error string.
  mutable std::mutex mu_;
  std::string last_error_;

  mutable std::mutex io_mu_;
  std::unique_ptr<AudioPipelineIo> io_;

  // Hot-path stats are atomics to avoid lock contention on the real-time
  // thread.
  std::atomic<bool> running_{false};
  std::atomic<std::uint64_t> frames_processed_{0};
  std::atomic<std::uint64_t> process_time_us_sum_{0};
  std::atomic<std::uint64_t> process_time_us_max_{0};
  std::atomic<std::uint64_t> process_time_us_last_{0};
  std::atomic<std::uint64_t> process_overruns_{0};

  // PulseAudio latency observations and resync counters.
  std::atomic<std::uint64_t> pulse_capture_latency_us_last_{0};
  std::atomic<std::uint64_t> pulse_playback_latency_us_last_{0};
  std::atomic<std::uint64_t> pulse_latency_us_max_{0};
  std::atomic<std::uint64_t> resync_events_{0};

  std::atomic<bool> stop_{false};
  std::thread thread_;
};

} // namespace studiocast::audio
