#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "core/audio/audio_backend_resolver.h"
#include "core/audio/audio_device_safety.h"
#include "core/audio/audio_pipeline.h"
#include "core/audio/audio_processor.h"
#include "core/audio/pulse/pactl.h"
#include "core/audio/virtual_audio_service.h"
#include "core/audio/virtual_mic.h"
#include "core/audio/virtual_mic_state.h"
#include "core/audio/virtual_speaker.h"
#include "core/audio/virtual_speaker_state.h"

namespace {

using studiocast::audio::AudioBackendAvailability;
using studiocast::audio::AudioConsumerSnapshot;
using studiocast::audio::AudioPipeline;
using studiocast::audio::AudioPipelineConfig;
using studiocast::audio::AudioPipelineHooks;
using studiocast::audio::AudioPipelineIo;
using studiocast::audio::AudioPipelineRunner;
using studiocast::audio::AudioPipelineStats;
using studiocast::audio::AudioProcessor;
using studiocast::audio::VirtualAudioService;
using studiocast::audio::VirtualAudioServiceConfig;
using studiocast::audio::VirtualAudioServiceHooks;
using studiocast::audio::VirtualMicState;
using studiocast::audio::VirtualSpeakerState;

using namespace std::chrono_literals;

class ScopedPactlExecHook final {
public:
  explicit ScopedPactlExecHook(
      studiocast::audio::pulse::PactlExecCaptureHook hook) {
    studiocast::audio::pulse::SetPactlExecCaptureHookForTesting(
        std::move(hook));
  }

  ~ScopedPactlExecHook() {
    studiocast::audio::pulse::SetPactlExecCaptureHookForTesting(nullptr);
  }

  ScopedPactlExecHook(const ScopedPactlExecHook &) = delete;
  ScopedPactlExecHook &operator=(const ScopedPactlExecHook &) = delete;
};

class EnvGuard final {
public:
  EnvGuard(const char *name, const std::string &value) : name_(name) {
    if (const char *old = std::getenv(name)) {
      had_old_ = true;
      old_ = old;
    }
    ::setenv(name, value.c_str(), 1);
  }

  ~EnvGuard() {
    if (had_old_) {
      ::setenv(name_, old_.c_str(), 1);
    } else {
      ::unsetenv(name_);
    }
  }

  EnvGuard(const EnvGuard &) = delete;
  EnvGuard &operator=(const EnvGuard &) = delete;

private:
  const char *name_;
  bool had_old_ = false;
  std::string old_;
};

class ScopedXdgStateHome final {
public:
  ScopedXdgStateHome() {
    if (const char *old = std::getenv("XDG_STATE_HOME")) {
      had_old_ = true;
      old_ = old;
    }

    std::error_code ec;
    auto base = std::filesystem::temp_directory_path(ec);
    if (ec)
      base = "/tmp";

    static std::atomic<int> counter{0};
    path_ =
        base /
        ("studiocast-audio-tests-" +
         std::to_string(
             std::chrono::steady_clock::now().time_since_epoch().count()) +
         "-" + std::to_string(counter.fetch_add(1, std::memory_order_relaxed)));

    std::filesystem::create_directories(path_, ec);
    ::setenv("XDG_STATE_HOME", path_.string().c_str(), 1);
  }

  ~ScopedXdgStateHome() {
    if (had_old_) {
      ::setenv("XDG_STATE_HOME", old_.c_str(), 1);
    } else {
      ::unsetenv("XDG_STATE_HOME");
    }

    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  ScopedXdgStateHome(const ScopedXdgStateHome &) = delete;
  ScopedXdgStateHome &operator=(const ScopedXdgStateHome &) = delete;

private:
  bool had_old_ = false;
  std::string old_;
  std::filesystem::path path_;
};

studiocast::util::ExecResult ExecResult(int exit_code,
                                        std::string stdout_str = {}) {
  studiocast::util::ExecResult result;
  result.exit_code = exit_code;
  result.stdout_str = std::move(stdout_str);
  return result;
}

bool CommandWasRun(const std::vector<std::string> &commands,
                   const std::string &needle) {
  for (const auto &command : commands) {
    if (command.find(needle) != std::string::npos)
      return true;
  }
  return false;
}

bool WaitUntil(const std::function<bool()> &pred,
               std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred())
      return true;
    std::this_thread::sleep_for(1ms);
  }
  return pred();
}

AudioConsumerSnapshot ConsumerSnapshot(bool present, int count = 1) {
  AudioConsumerSnapshot out;
  out.present = present;
  out.count = present ? count : 0;
  return out;
}

studiocast::audio::pulse::PactlExecCaptureHook
SafeMicrophoneSourcePactlHook() {
  return [](const std::string &command) {
    if (command == "pactl get-default-source 2>&1")
      return ExecResult(0, "physical_test_mic\n");
    if (command == "pactl list short sources 2>&1") {
      return ExecResult(0,
                        "1\tstudiocast_sink.monitor\tmodule-null-sink.c\t"
                        "s16le 2ch 48000Hz\n"
                        "2\tphysical_test_mic\tmodule-alsa-card.c\t"
                        "s16le 2ch 48000Hz\n");
    }
    return ExecResult(99, "unexpected command: " + command);
  };
}

void HookMicrophoneConsumerFlag(VirtualAudioServiceHooks *hooks,
                                std::atomic<bool> *present) {
  hooks->detect_microphone_consumers = [present] {
    return ConsumerSnapshot(present->load(std::memory_order_relaxed));
  };
}

void HookSpeakerConsumerFlag(VirtualAudioServiceHooks *hooks,
                             std::atomic<bool> *present) {
  hooks->detect_speaker_consumers = [present] {
    return ConsumerSnapshot(present->load(std::memory_order_relaxed));
  };
}

class DeadPipeline final : public AudioPipelineRunner {
public:
  explicit DeadPipeline(std::atomic<int> *stop_calls)
      : stop_calls_(stop_calls) {}

  bool Start(const AudioPipelineConfig &, std::string *error) override {
    if (error)
      error->clear();
    return true;
  }

  void Stop() override {
    if (stop_calls_)
      stop_calls_->fetch_add(1, std::memory_order_relaxed);
  }

  AudioPipelineStats GetStats() const override {
    AudioPipelineStats stats;
    stats.running = false;
    return stats;
  }

private:
  std::atomic<int> *stop_calls_ = nullptr;
};

class StartFailPipeline final : public AudioPipelineRunner {
public:
  bool Start(const AudioPipelineConfig &, std::string *error) override {
    if (error)
      *error = "synthetic start failure";
    return false;
  }

  void Stop() override {}

  AudioPipelineStats GetStats() const override { return {}; }
};

class FixedStatsPipeline final : public AudioPipelineRunner {
public:
  FixedStatsPipeline(bool running, std::string last_error,
                     std::atomic<int> *stop_calls = nullptr)
      : stop_calls_(stop_calls) {
    stats_.running = running;
    stats_.last_error = std::move(last_error);
  }

  FixedStatsPipeline(AudioPipelineStats stats,
                     std::atomic<int> *stop_calls = nullptr)
      : stats_(std::move(stats)), stop_calls_(stop_calls) {}

  bool Start(const AudioPipelineConfig &, std::string *error) override {
    if (error)
      error->clear();
    return true;
  }

  void Stop() override {
    if (stop_calls_)
      stop_calls_->fetch_add(1, std::memory_order_relaxed);
  }

  AudioPipelineStats GetStats() const override { return stats_; }

private:
  AudioPipelineStats stats_{};
  std::atomic<int> *stop_calls_ = nullptr;
};

class CopyProcessor final : public AudioProcessor {
public:
  bool Process(const float *in, float *out, std::uint32_t frames,
               std::uint32_t channels, std::string *error) override {
    if (error)
      error->clear();
    std::memcpy(out, in,
                static_cast<std::size_t>(frames) * channels * sizeof(float));
    return true;
  }
};

class CountingCopyProcessor final : public AudioProcessor {
public:
  explicit CountingCopyProcessor(std::atomic<int> *reset_calls)
      : reset_calls_(reset_calls) {}

  bool Process(const float *in, float *out, std::uint32_t frames,
               std::uint32_t channels, std::string *error) override {
    if (error)
      error->clear();
    std::memcpy(out, in,
                static_cast<std::size_t>(frames) * channels * sizeof(float));
    return true;
  }

  void Reset() override {
    if (reset_calls_)
      reset_calls_->fetch_add(1, std::memory_order_relaxed);
  }

private:
  std::atomic<int> *reset_calls_ = nullptr;
};

enum class BlockMode {
  kRead,
  kWrite,
};

struct BlockingIoState {
  std::mutex mu;
  std::condition_variable cv;
  bool block_entered = false;
  bool stop_requested = false;
  int request_stop_calls = 0;
};

class BlockingIo final : public AudioPipelineIo {
public:
  BlockingIo(std::shared_ptr<BlockingIoState> state, BlockMode mode,
             std::chrono::milliseconds block_timeout)
      : state_(std::move(state)), mode_(mode), block_timeout_(block_timeout) {}

  bool Open(const AudioPipelineConfig &, std::string *error) override {
    if (error)
      error->clear();
    return true;
  }

  bool Read(void *dst, std::size_t bytes, std::string *error) override {
    if (mode_ != BlockMode::kRead) {
      std::memset(dst, 0, bytes);
      if (error)
        error->clear();
      return true;
    }
    return Block("capture read", error);
  }

  bool Write(const void *, std::size_t, std::string *error) override {
    if (mode_ != BlockMode::kWrite) {
      if (error)
        error->clear();
      return true;
    }
    return Block("playback write", error);
  }

  bool GetCaptureLatencyUs(std::uint64_t *) override { return false; }
  bool GetPlaybackLatencyUs(std::uint64_t *) override { return false; }
  void Flush() override {}

  void RequestStop() override {
    std::lock_guard<std::mutex> lock(state_->mu);
    state_->stop_requested = true;
    ++state_->request_stop_calls;
    state_->cv.notify_all();
  }

private:
  bool Block(const char *label, std::string *error) {
    std::unique_lock<std::mutex> lock(state_->mu);
    state_->block_entered = true;
    state_->cv.notify_all();

    const bool stopped = state_->cv.wait_for(
        lock, block_timeout_, [&] { return state_->stop_requested; });
    if (stopped) {
      if (error)
        error->clear();
      return false;
    }

    if (error)
      *error = std::string(label) + " remained blocked";
    return false;
  }

  std::shared_ptr<BlockingIoState> state_;
  BlockMode mode_;
  std::chrono::milliseconds block_timeout_;
};

struct ParkedReadIoState {
  std::mutex mu;
  std::condition_variable cv;
  bool read_entered = false;
  bool released = false;
};

// Capture I/O that parks the pipeline worker inside Read() until the test
// releases it. RequestStop() does not release the worker, so a Stop() caller
// stays inside join() long enough for a second Stop() caller to reach the
// same join.
class ParkedReadIo final : public AudioPipelineIo {
public:
  explicit ParkedReadIo(std::shared_ptr<ParkedReadIoState> state)
      : state_(std::move(state)) {}

  bool Open(const AudioPipelineConfig &, std::string *error) override {
    if (error)
      error->clear();
    return true;
  }

  bool Read(void *, std::size_t, std::string *error) override {
    std::unique_lock<std::mutex> lock(state_->mu);
    state_->read_entered = true;
    state_->cv.notify_all();
    state_->cv.wait(lock, [&] { return state_->released; });
    if (error)
      error->clear();
    return false;
  }

  bool Write(const void *, std::size_t, std::string *error) override {
    if (error)
      error->clear();
    return true;
  }

  bool GetCaptureLatencyUs(std::uint64_t *) override { return false; }
  bool GetPlaybackLatencyUs(std::uint64_t *) override { return false; }
  void Flush() override {}
  void RequestStop() override {}

private:
  std::shared_ptr<ParkedReadIoState> state_;
};

struct ResettingOpenIoState {
  std::mutex mu;
  std::condition_variable cv;
  bool open_started = false;
  bool stop_requested = false;
  int request_stop_calls = 0;
};

struct BlockingFlushIoState {
  std::mutex mu;
  std::condition_variable cv;
  bool flush_entered = false;
  bool stop_requested = false;
  int request_stop_calls = 0;
};

class BlockingFlushIo final : public AudioPipelineIo {
public:
  explicit BlockingFlushIo(std::shared_ptr<BlockingFlushIoState> state,
                           std::chrono::milliseconds block_timeout)
      : state_(std::move(state)), block_timeout_(block_timeout) {}

  void SetStopRequestedFlag(const std::atomic<bool> *stop_requested) override {
    external_stop_requested_ = stop_requested;
  }

  bool Open(const AudioPipelineConfig &, std::string *error) override {
    if (error)
      error->clear();
    return true;
  }

  bool Read(void *dst, std::size_t bytes, std::string *error) override {
    std::memset(dst, 0, bytes);
    if (error)
      error->clear();
    return true;
  }

  bool Write(const void *, std::size_t, std::string *error) override {
    if (error)
      error->clear();
    return true;
  }

  bool GetCaptureLatencyUs(std::uint64_t *) override { return false; }
  bool GetPlaybackLatencyUs(std::uint64_t *) override { return false; }

  void Flush() override {
    std::unique_lock<std::mutex> lock(state_->mu);
    state_->flush_entered = true;
    state_->cv.notify_all();
    state_->cv.wait_for(lock, block_timeout_, [&] {
      return state_->stop_requested ||
             (external_stop_requested_ &&
              external_stop_requested_->load(std::memory_order_acquire));
    });
  }

  void RequestStop() override {
    std::lock_guard<std::mutex> lock(state_->mu);
    state_->stop_requested = true;
    ++state_->request_stop_calls;
    state_->cv.notify_all();
  }

private:
  std::shared_ptr<BlockingFlushIoState> state_;
  const std::atomic<bool> *external_stop_requested_ = nullptr;
  std::chrono::milliseconds block_timeout_;
};

class ResettingOpenIo final : public AudioPipelineIo {
public:
  ResettingOpenIo(std::shared_ptr<ResettingOpenIoState> state,
                  std::chrono::milliseconds reset_delay,
                  std::chrono::milliseconds block_timeout)
      : state_(std::move(state)), reset_delay_(reset_delay),
        block_timeout_(block_timeout) {}

  void SetStopRequestedFlag(const std::atomic<bool> *stop_requested) override {
    external_stop_requested_ = stop_requested;
  }

  bool Open(const AudioPipelineConfig &, std::string *error) override {
    {
      std::lock_guard<std::mutex> lock(state_->mu);
      state_->open_started = true;
      state_->cv.notify_all();
    }

    std::this_thread::sleep_for(reset_delay_);

    std::unique_lock<std::mutex> lock(state_->mu);
    state_->stop_requested = false;
    const bool stopped = state_->cv.wait_for(lock, block_timeout_, [&] {
      return state_->stop_requested ||
             (external_stop_requested_ &&
              external_stop_requested_->load(std::memory_order_acquire));
    });
    if (stopped) {
      if (error)
        error->clear();
      return false;
    }

    if (error)
      *error = "open remained blocked after stop";
    return false;
  }

  bool Read(void *, std::size_t, std::string *) override { return false; }
  bool Write(const void *, std::size_t, std::string *) override {
    return false;
  }
  bool GetCaptureLatencyUs(std::uint64_t *) override { return false; }
  bool GetPlaybackLatencyUs(std::uint64_t *) override { return false; }
  void Flush() override {}

  void RequestStop() override {
    std::lock_guard<std::mutex> lock(state_->mu);
    state_->stop_requested = true;
    ++state_->request_stop_calls;
    state_->cv.notify_all();
  }

private:
  std::shared_ptr<ResettingOpenIoState> state_;
  const std::atomic<bool> *external_stop_requested_ = nullptr;
  std::chrono::milliseconds reset_delay_;
  std::chrono::milliseconds block_timeout_;
};

class OpenFailIo final : public AudioPipelineIo {
public:
  explicit OpenFailIo(std::atomic<int> *open_calls) : open_calls_(open_calls) {}

  bool Open(const AudioPipelineConfig &, std::string *error) override {
    if (open_calls_)
      open_calls_->fetch_add(1, std::memory_order_relaxed);
    if (error)
      *error = "synthetic open failure";
    return false;
  }

  bool Read(void *, std::size_t, std::string *) override { return false; }
  bool Write(const void *, std::size_t, std::string *) override {
    return false;
  }
  bool GetCaptureLatencyUs(std::uint64_t *) override { return false; }
  bool GetPlaybackLatencyUs(std::uint64_t *) override { return false; }
  void Flush() override {}
  void RequestStop() override {}

private:
  std::atomic<int> *open_calls_ = nullptr;
};

class ReadFailIo final : public AudioPipelineIo {
public:
  explicit ReadFailIo(std::atomic<int> *read_calls) : read_calls_(read_calls) {}

  bool Open(const AudioPipelineConfig &, std::string *error) override {
    if (error)
      error->clear();
    return true;
  }

  bool Read(void *, std::size_t, std::string *error) override {
    if (read_calls_)
      read_calls_->fetch_add(1, std::memory_order_relaxed);
    if (error)
      *error = "synthetic Pulse capture stream disconnected";
    return false;
  }

  bool Write(const void *, std::size_t, std::string *) override {
    return false;
  }
  bool GetCaptureLatencyUs(std::uint64_t *) override { return false; }
  bool GetPlaybackLatencyUs(std::uint64_t *) override { return false; }
  void Flush() override {}
  void RequestStop() override {}

private:
  std::atomic<int> *read_calls_ = nullptr;
};

struct LatencyIoState {
  std::atomic<bool> stop_requested{false};
  std::atomic<int> flush_calls{0};
  std::atomic<int> capture_latency_queries{0};
  std::atomic<int> playback_latency_queries{0};
};

class HighLatencyIo final : public AudioPipelineIo {
public:
  explicit HighLatencyIo(std::shared_ptr<LatencyIoState> state)
      : state_(std::move(state)) {}

  void SetStopRequestedFlag(const std::atomic<bool> *stop_requested) override {
    external_stop_requested_ = stop_requested;
  }

  bool Open(const AudioPipelineConfig &, std::string *error) override {
    if (error)
      error->clear();
    return true;
  }

  bool Read(void *dst, std::size_t bytes, std::string *error) override {
    if (ShouldStop()) {
      if (error)
        error->clear();
      return false;
    }
    std::memset(dst, 0, bytes);
    std::this_thread::sleep_for(1ms);
    if (error)
      error->clear();
    return true;
  }

  bool Write(const void *, std::size_t, std::string *error) override {
    if (ShouldStop()) {
      if (error)
        error->clear();
      return false;
    }
    if (error)
      error->clear();
    return true;
  }

  bool GetCaptureLatencyUs(std::uint64_t *latency_us) override {
    state_->capture_latency_queries.fetch_add(1, std::memory_order_relaxed);
    if (latency_us)
      *latency_us = 80000;
    return true;
  }

  bool GetPlaybackLatencyUs(std::uint64_t *latency_us) override {
    state_->playback_latency_queries.fetch_add(1, std::memory_order_relaxed);
    if (latency_us)
      *latency_us = 80000;
    return true;
  }

  void Flush() override {
    state_->flush_calls.fetch_add(1, std::memory_order_relaxed);
  }

  void RequestStop() override {
    state_->stop_requested.store(true, std::memory_order_release);
  }

private:
  bool ShouldStop() const {
    return state_->stop_requested.load(std::memory_order_acquire) ||
           (external_stop_requested_ &&
            external_stop_requested_->load(std::memory_order_acquire));
  }

  std::shared_ptr<LatencyIoState> state_;
  const std::atomic<bool> *external_stop_requested_ = nullptr;
};

struct FlakyLatencyIoState {
  std::atomic<bool> stop_requested{false};
  std::atomic<int> capture_latency_queries{0};
  std::atomic<int> playback_latency_queries{0};
};

class FlakyLatencyIo final : public AudioPipelineIo {
public:
  explicit FlakyLatencyIo(std::shared_ptr<FlakyLatencyIoState> state)
      : state_(std::move(state)) {}

  void SetStopRequestedFlag(const std::atomic<bool> *stop_requested) override {
    external_stop_requested_ = stop_requested;
  }

  bool Open(const AudioPipelineConfig &, std::string *error) override {
    if (error)
      error->clear();
    return true;
  }

  bool Read(void *dst, std::size_t bytes, std::string *error) override {
    if (ShouldStop()) {
      if (error)
        error->clear();
      return false;
    }
    std::memset(dst, 0, bytes);
    std::this_thread::sleep_for(1ms);
    if (error)
      error->clear();
    return true;
  }

  bool Write(const void *, std::size_t, std::string *error) override {
    if (ShouldStop()) {
      if (error)
        error->clear();
      return false;
    }
    if (error)
      error->clear();
    return true;
  }

  bool GetCaptureLatencyUs(std::uint64_t *latency_us) override {
    const int q = state_->capture_latency_queries.fetch_add(
                      1, std::memory_order_relaxed) +
                  1;
    if (q == 1) {
      if (latency_us)
        *latency_us = 12000;
      return true;
    }
    return false;
  }

  bool GetPlaybackLatencyUs(std::uint64_t *latency_us) override {
    const int q = state_->playback_latency_queries.fetch_add(
                      1, std::memory_order_relaxed) +
                  1;
    if (latency_us)
      *latency_us = (q == 1) ? 34000 : 5000;
    return true;
  }

  void Flush() override {}

  void RequestStop() override {
    state_->stop_requested.store(true, std::memory_order_release);
  }

private:
  bool ShouldStop() const {
    return state_->stop_requested.load(std::memory_order_acquire) ||
           (external_stop_requested_ &&
            external_stop_requested_->load(std::memory_order_acquire));
  }

  std::shared_ptr<FlakyLatencyIoState> state_;
  const std::atomic<bool> *external_stop_requested_ = nullptr;
};

struct ScriptedQualityIoState {
  std::mutex mu;
  std::vector<float> input;
  std::vector<float> output;
  std::size_t read_offset_samples = 0;
  std::size_t bytes_per_frame = 0;
  std::uint32_t open_sample_rate = 0;
  std::uint32_t open_channels = 0;
  int read_calls = 0;
  int write_calls = 0;
  int flush_calls = 0;
  bool stop_requested = false;
};

class ScriptedQualityIo final : public AudioPipelineIo {
public:
  explicit ScriptedQualityIo(std::shared_ptr<ScriptedQualityIoState> state)
      : state_(std::move(state)) {}

  void SetStopRequestedFlag(const std::atomic<bool> *stop_requested) override {
    external_stop_requested_ = stop_requested;
  }

  bool Open(const AudioPipelineConfig &cfg, std::string *error) override {
    std::lock_guard<std::mutex> lock(state_->mu);
    state_->open_sample_rate = static_cast<std::uint32_t>(cfg.sample_rate);
    state_->open_channels = cfg.channels;
    state_->bytes_per_frame = static_cast<std::size_t>(cfg.frame_samples) *
                              cfg.channels * sizeof(float);
    state_->read_offset_samples = 0;
    state_->output.clear();
    state_->read_calls = 0;
    state_->write_calls = 0;
    state_->flush_calls = 0;
    state_->stop_requested = false;

    const std::size_t samples_per_frame =
        static_cast<std::size_t>(cfg.frame_samples) * cfg.channels;
    if (samples_per_frame == 0 ||
        (state_->input.size() % samples_per_frame) != 0) {
      if (error)
        *error = "scripted audio input is not frame aligned";
      return false;
    }
    if (error)
      error->clear();
    return true;
  }

  bool Read(void *dst, std::size_t bytes, std::string *error) override {
    std::lock_guard<std::mutex> lock(state_->mu);
    if (ShouldStopLocked()) {
      if (error)
        error->clear();
      return false;
    }
    if (bytes != state_->bytes_per_frame) {
      if (error)
        *error = "unexpected scripted read size";
      return false;
    }
    if (state_->read_offset_samples >= state_->input.size()) {
      if (error)
        error->clear();
      return false;
    }

    const std::size_t samples = bytes / sizeof(float);
    if (state_->read_offset_samples + samples > state_->input.size()) {
      if (error)
        *error = "scripted read exceeded input";
      return false;
    }
    std::memcpy(dst, state_->input.data() + state_->read_offset_samples, bytes);
    state_->read_offset_samples += samples;
    ++state_->read_calls;
    if (error)
      error->clear();
    return true;
  }

  bool Write(const void *src, std::size_t bytes, std::string *error) override {
    std::lock_guard<std::mutex> lock(state_->mu);
    if (ShouldStopLocked()) {
      if (error)
        error->clear();
      return false;
    }
    if (bytes != state_->bytes_per_frame) {
      if (error)
        *error = "unexpected scripted write size";
      return false;
    }
    const auto *samples = static_cast<const float *>(src);
    state_->output.insert(state_->output.end(), samples,
                          samples + (bytes / sizeof(float)));
    ++state_->write_calls;
    if (error)
      error->clear();
    return true;
  }

  bool GetCaptureLatencyUs(std::uint64_t *) override { return false; }
  bool GetPlaybackLatencyUs(std::uint64_t *) override { return false; }

  void Flush() override {
    std::lock_guard<std::mutex> lock(state_->mu);
    ++state_->flush_calls;
  }

  void RequestStop() override {
    std::lock_guard<std::mutex> lock(state_->mu);
    state_->stop_requested = true;
  }

private:
  bool ShouldStopLocked() const {
    return state_->stop_requested ||
           (external_stop_requested_ &&
            external_stop_requested_->load(std::memory_order_acquire));
  }

  std::shared_ptr<ScriptedQualityIoState> state_;
  const std::atomic<bool> *external_stop_requested_ = nullptr;
};

std::vector<float> MakeSyntheticStereoAudio(std::uint32_t frame_samples,
                                            int frame_count) {
  constexpr double kPi = 3.141592653589793238462643383279502884;
  constexpr double kSampleRate = 48000.0;
  const std::size_t total_frames = static_cast<std::size_t>(frame_samples) *
                                   static_cast<std::size_t>(frame_count);
  std::vector<float> audio(total_frames * 2, 0.0f);

  for (std::size_t n = 0; n < total_frames; ++n) {
    const std::size_t packet = n / frame_samples;
    float left = 0.0f;
    float right = 0.0f;
    const double t = static_cast<double>(n) / kSampleRate;

    if (packet == 1 || packet == 2) {
      left = static_cast<float>(0.35 * std::sin(2.0 * kPi * 1000.0 * t));
      right = static_cast<float>(0.25 * std::sin(2.0 * kPi * 440.0 * t));
    } else if (packet == 3) {
      if ((n % frame_samples) == 0) {
        left = 0.75f;
        right = -0.75f;
      }
    } else if (packet > 3) {
      const double sweep01 =
          static_cast<double>(n) /
          static_cast<double>(std::max<std::size_t>(total_frames - 1, 1));
      const double hz = 220.0 + 1780.0 * sweep01;
      left = static_cast<float>(0.2 * std::sin(2.0 * kPi * hz * t));
      right = static_cast<float>(0.2 * std::sin(2.0 * kPi * hz * t + 0.3));
    }

    audio[n * 2 + 0] = left;
    audio[n * 2 + 1] = right;
  }
  return audio;
}

double Rms(const std::vector<float> &samples) {
  if (samples.empty())
    return 0.0;
  double sum = 0.0;
  for (float sample : samples)
    sum += static_cast<double>(sample) * static_cast<double>(sample);
  return std::sqrt(sum / static_cast<double>(samples.size()));
}

bool TestPactlLoadModuleQuotesVectorArguments() {
  std::vector<std::string> commands;
  ScopedPactlExecHook hook([&](const std::string &command) {
    commands.push_back(command);
    studiocast::util::ExecResult result;
    result.exit_code = 0;
    result.stdout_str = "123\n";
    return result;
  });

  std::string err;
  const auto id = studiocast::audio::pulse::LoadModule(
      "module-null-sink",
      {
          "sink_name=studio cast",
          "sink_properties=device.description=\"StudioCast Speakers\"",
          "weird=a'b",
      },
      &err);

  const std::string expected =
      "pactl load-module 'module-null-sink' 'sink_name=studio cast' "
      "'sink_properties=device.description=\"StudioCast Speakers\"' "
      "'weird=a'\"'\"'b' 2>&1";

  if (!id || *id != 123) {
    std::cerr << "expected pactl module id 123; error='" << err << "'\n";
    return false;
  }

  if (commands.size() != 1 || commands[0] != expected) {
    std::cerr << "unexpected pactl load-module command\nexpected: " << expected
              << "\nactual:   "
              << (commands.empty() ? std::string("<none>") : commands[0])
              << "\n";
    return false;
  }

  return true;
}

bool TestPactlLoadModuleStringCompatibilitySplitter() {
  std::vector<std::string> commands;
  ScopedPactlExecHook hook([&](const std::string &command) {
    commands.push_back(command);
    studiocast::util::ExecResult result;
    result.exit_code = 0;
    result.stdout_str = "124\n";
    return result;
  });

  std::string err;
  const auto id = studiocast::audio::pulse::LoadModule(
      "module-loopback",
      "source='studio cast.monitor' sink=\"physical sink\" latency_msec=10 "
      "escaped=hello\\ world",
      &err);

  const std::string expected =
      "pactl load-module 'module-loopback' 'source=studio cast.monitor' "
      "'sink=physical sink' 'latency_msec=10' 'escaped=hello world' 2>&1";

  if (!id || *id != 124) {
    std::cerr << "expected pactl module id 124; error='" << err << "'\n";
    return false;
  }

  if (commands.size() != 1 || commands[0] != expected) {
    std::cerr << "unexpected pactl compatibility command\nexpected: "
              << expected << "\nactual:   "
              << (commands.empty() ? std::string("<none>") : commands[0])
              << "\n";
    return false;
  }

  return true;
}

bool TestPactlDefaultSourceAndSinkFallbackToInfo() {
  std::vector<std::string> commands;
  ScopedPactlExecHook hook([&](const std::string &command) {
    commands.push_back(command);
    if (command == "pactl get-default-source 2>&1")
      return ExecResult(1, "unknown command\n");
    if (command == "pactl get-default-sink 2>&1")
      return ExecResult(1, "unknown command\n");
    if (command == "pactl info 2>&1") {
      return ExecResult(0, "Server String: /run/user/1000/pulse/native\n"
                           "Default Source: alsa_input.usb_test_mic\n"
                           "Default Sink: alsa_output.pci_test_speakers\n");
    }
    return ExecResult(99, "unexpected command: " + command);
  });

  std::string err;
  const auto source = studiocast::audio::pulse::GetDefaultSourceName(&err);
  const auto sink = studiocast::audio::pulse::GetDefaultSinkName(&err);

  const std::vector<std::string> expected = {
      "pactl get-default-source 2>&1",
      "pactl info 2>&1",
      "pactl get-default-sink 2>&1",
      "pactl info 2>&1",
  };

  if (!source || *source != "alsa_input.usb_test_mic" || !sink ||
      *sink != "alsa_output.pci_test_speakers") {
    std::cerr << "default source/sink fallback returned source='"
              << (source ? *source : std::string("<none>")) << "' sink='"
              << (sink ? *sink : std::string("<none>")) << "' error='" << err
              << "'\n";
    return false;
  }

  if (commands != expected) {
    std::cerr << "unexpected default source/sink command sequence\n";
    for (const auto &command : commands)
      std::cerr << "  " << command << "\n";
    return false;
  }

  return true;
}

bool TestPactlProplistCommandsQuoteArgumentsAndDetectFailures() {
  std::vector<std::string> commands;
  ScopedPactlExecHook hook([&](const std::string &command) {
    commands.push_back(command);
    if (commands.size() == 1)
      return ExecResult(0);
    return ExecResult(0, "Failure: No such entity\n");
  });

  std::string err;
  const bool sink_ok = studiocast::audio::pulse::UpdateSinkProplist(
      "sink's name",
      {
          "device.description=StudioCast Speakers",
          "node.description=StudioCast Speakers",
      },
      &err);
  if (!sink_ok) {
    std::cerr << "expected sink proplist update to succeed; error='" << err
              << "'\n";
    return false;
  }

  err.clear();
  const bool source_ok = studiocast::audio::pulse::UpdateSourceProplist(
      "source name",
      {
          "device.description=StudioCast Microphone",
          "node.description=StudioCast Microphone",
      },
      &err);
  if (source_ok || err.find("Failure: No such entity") == std::string::npos) {
    std::cerr << "expected source proplist failure to be detected; ok="
              << source_ok << " error='" << err << "'\n";
    return false;
  }

  const std::vector<std::string> expected = {
      "pactl update-sink-proplist 'sink'\"'\"'s name' "
      "'device.description=StudioCast Speakers' "
      "'node.description=StudioCast Speakers' 2>&1",
      "pactl update-source-proplist 'source name' "
      "'device.description=StudioCast Microphone' "
      "'node.description=StudioCast Microphone' 2>&1",
  };

  if (commands != expected) {
    std::cerr << "unexpected proplist command sequence\nexpected:\n";
    for (const auto &command : expected)
      std::cerr << "  " << command << "\n";
    std::cerr << "actual:\n";
    for (const auto &command : commands)
      std::cerr << "  " << command << "\n";
    return false;
  }

  return true;
}

bool TestPactlConsumerListsParseSourceOutputsAndSinkInputs() {
  std::vector<std::string> commands;
  ScopedPactlExecHook hook([&](const std::string &command) {
    commands.push_back(command);
    if (command == "pactl list short source-outputs 2>&1") {
      return ExecResult(0,
                        "7\t3\t51\tprotocol-native.c\tfloat32le 1ch 48000Hz\n"
                        "8\tstudiocast_mic\t52\tprotocol-native.c\t"
                        "float32le 1ch 48000Hz\n");
    }
    if (command == "pactl list short sink-inputs 2>&1") {
      return ExecResult(0,
                        "9\t4\t61\tprotocol-native.c\tfloat32le 2ch 48000Hz\n"
                        "10\tstudiocast_speakers\t62\tprotocol-native.c\t"
                        "float32le 2ch 48000Hz\n");
    }
    return ExecResult(99, "unexpected command: " + command);
  });

  std::string err;
  const auto outputs = studiocast::audio::pulse::ListSourceOutputs(&err);
  if (!err.empty() || outputs.size() != 2 || outputs[0].id != 7 ||
      outputs[0].source != "3" || outputs[1].source != "studiocast_mic") {
    std::cerr << "source-output parsing failed; count=" << outputs.size()
              << " error='" << err << "'\n";
    return false;
  }

  err.clear();
  const auto inputs = studiocast::audio::pulse::ListSinkInputs(&err);
  if (!err.empty() || inputs.size() != 2 || inputs[0].id != 9 ||
      inputs[0].sink != "4" || inputs[1].sink != "studiocast_speakers") {
    std::cerr << "sink-input parsing failed; count=" << inputs.size()
              << " error='" << err << "'\n";
    return false;
  }

  const std::vector<std::string> expected = {
      "pactl list short source-outputs 2>&1",
      "pactl list short sink-inputs 2>&1",
  };
  if (commands != expected) {
    std::cerr << "unexpected consumer-list command sequence\n";
    for (const auto &command : commands)
      std::cerr << "  " << command << "\n";
    return false;
  }

  return true;
}

bool TestAudioSourceSafetyRejectsVirtualAndMonitorSources() {
  std::string reason;
  if (!studiocast::audio::IsUnsafeInputSourceName("studiocast_mic", &reason) ||
      reason.find("StudioCast virtual source") == std::string::npos) {
    std::cerr << "StudioCast virtual mic source was not rejected; reason='"
              << reason << "'\n";
    return false;
  }

  reason.clear();
  if (!studiocast::audio::IsUnsafeInputSourceName(
          "alsa_output.pci-0000_00_1f.3.analog-stereo.monitor", &reason) ||
      reason.find("monitor source") == std::string::npos) {
    std::cerr << "monitor source was not rejected; reason='" << reason << "'\n";
    return false;
  }

  const auto explicit_virtual =
      studiocast::audio::ResolveSafeInputSourceName("studiocast_mic");
  if (explicit_virtual.ok ||
      explicit_virtual.error.find("StudioCast virtual source") ==
          std::string::npos) {
    std::cerr << "explicit virtual source was not rejected; ok="
              << explicit_virtual.ok << " error='" << explicit_virtual.error
              << "'\n";
    return false;
  }

  const auto explicit_physical =
      studiocast::audio::ResolveSafeInputSourceName("alsa_input.usb_test_mic");
  if (!explicit_physical.ok ||
      explicit_physical.source_name != "alsa_input.usb_test_mic") {
    std::cerr << "explicit physical source was not accepted; ok="
              << explicit_physical.ok << " source='"
              << explicit_physical.source_name << "' error='"
              << explicit_physical.error << "'\n";
    return false;
  }

  return true;
}

bool TestAudioSourceAutoFallsBackFromUnsafeDefaultSource() {
  std::vector<std::string> commands;
  ScopedPactlExecHook hook([&](const std::string &command) {
    commands.push_back(command);
    if (command == "pactl get-default-source 2>&1")
      return ExecResult(0, "studiocast_speakers.monitor\n");
    if (command == "pactl list short sources 2>&1") {
      return ExecResult(0,
                        "0\tstudiocast_speakers.monitor\tmodule-null-sink.c\t"
                        "s16le 2ch 48000Hz\tIDLE\n"
                        "1\talsa_output.pci_test.monitor\tmodule-alsa-card.c\t"
                        "s16le 2ch 48000Hz\tIDLE\n"
                        "2\talsa_input.usb_test_mic\tmodule-alsa-card.c\t"
                        "s16le 1ch 48000Hz\tRUNNING\n");
    }
    return ExecResult(99, "unexpected command: " + command);
  });

  const auto resolved = studiocast::audio::ResolveSafeInputSourceName("");
  if (!resolved.ok || resolved.source_name != "alsa_input.usb_test_mic" ||
      resolved.warnings.empty()) {
    std::cerr << "auto source did not fall back to physical mic; ok="
              << resolved.ok << " source='" << resolved.source_name
              << "' error='" << resolved.error
              << "' warnings=" << resolved.warnings.size() << "\n";
    return false;
  }

  const std::vector<std::string> expected = {
      "pactl get-default-source 2>&1",
      "pactl list short sources 2>&1",
  };
  if (commands != expected) {
    std::cerr << "unexpected source auto fallback command sequence\n";
    for (const auto &command : commands)
      std::cerr << "  " << command << "\n";
    return false;
  }

  return true;
}

bool TestAudioSourceAutoFailsWhenNoSafeSourceExists() {
  ScopedPactlExecHook hook([](const std::string &command) {
    if (command == "pactl get-default-source 2>&1")
      return ExecResult(0, "studiocast_mic\n");
    if (command == "pactl list short sources 2>&1") {
      return ExecResult(0,
                        "0\tstudiocast_mic\tmodule-remap-source.c\t"
                        "s16le 1ch 48000Hz\tIDLE\n"
                        "1\talsa_output.pci_test.monitor\tmodule-alsa-card.c\t"
                        "s16le 2ch 48000Hz\tIDLE\n");
    }
    return ExecResult(99, "unexpected command: " + command);
  });

  const auto resolved = studiocast::audio::ResolveSafeInputSourceName("auto");
  if (resolved.ok ||
      resolved.error.find("No safe Pulse microphone source") ==
          std::string::npos ||
      resolved.error.find("Select a physical microphone") ==
          std::string::npos) {
    std::cerr << "auto source without safe fallback did not fail clearly; ok="
              << resolved.ok << " source='" << resolved.source_name
              << "' error='" << resolved.error << "'\n";
    return false;
  }

  return true;
}

bool TestVirtualAudioServiceReportsResolvedAutoSourceAndWarnings() {
  std::vector<std::string> commands;
  ScopedPactlExecHook hook([&](const std::string &command) {
    commands.push_back(command);
    if (command == "pactl get-default-source 2>&1")
      return ExecResult(0, "studiocast_speakers.monitor\n");
    if (command == "pactl list short sources 2>&1") {
      return ExecResult(0,
                        "0\tstudiocast_speakers.monitor\tmodule-null-sink.c\t"
                        "s16le 2ch 48000Hz\tIDLE\n"
                        "1\talsa_input.usb_service_mic\tmodule-alsa-card.c\t"
                        "s16le 1ch 48000Hz\tRUNNING\n");
    }
    return ExecResult(99, "unexpected command: " + command);
  });

  std::atomic<bool> mic_consumer_present{true};
  std::atomic<int> pipeline_creates{0};
  std::string pipeline_source;

  VirtualAudioServiceHooks hooks;
  HookMicrophoneConsumerFlag(&hooks, &mic_consumer_present);
  hooks.create_pipeline =
      [&](AudioProcessor *) -> std::unique_ptr<AudioPipelineRunner> {
    ++pipeline_creates;
    class CapturingPipeline final : public AudioPipelineRunner {
    public:
      explicit CapturingPipeline(std::string *source) : source_(source) {}

      bool Start(const AudioPipelineConfig &cfg, std::string *error) override {
        if (source_)
          *source_ = cfg.source_name;
        if (error)
          error->clear();
        return true;
      }

      void Stop() override {}

      AudioPipelineStats GetStats() const override {
        AudioPipelineStats stats;
        stats.running = true;
        return stats;
      }

    private:
      std::string *source_ = nullptr;
    };
    return std::make_unique<CapturingPipeline>(&pipeline_source);
  };
  hooks.sleep_for = [](std::chrono::milliseconds) {
    std::this_thread::sleep_for(1ms);
  };

  VirtualAudioService service(std::move(hooks));
  VirtualAudioServiceConfig cfg;
  cfg.enabled = true;
  cfg.create_virtual_mic = false;
  cfg.source_name.clear(); // auto
  cfg.poll_ms = 1;
  cfg.consumer_grace_ms = 0;

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  const bool resolved = WaitUntil(
      [&] {
        const auto status = service.Status();
        return pipeline_creates.load(std::memory_order_relaxed) == 1 &&
               pipeline_source == "alsa_input.usb_service_mic" &&
               status.selected_source == "alsa_input.usb_service_mic" &&
               status.source_error.empty() && !status.source_warnings.empty();
      },
      250ms);
  const auto status = service.Status();
  service.Stop();

  if (!resolved) {
    std::cerr << "service did not report/pass resolved auto source; "
              << "creates=" << pipeline_creates.load()
              << " pipeline_source='" << pipeline_source
              << "' selected_source='" << status.selected_source
              << "' source_error='" << status.source_error
              << "' warnings=" << status.source_warnings.size() << "\n";
    return false;
  }

  const std::vector<std::string> expected = {
      "pactl get-default-source 2>&1",
      "pactl list short sources 2>&1",
  };
  if (commands.size() < expected.size() ||
      !std::equal(expected.begin(), expected.end(), commands.begin())) {
    std::cerr << "unexpected source resolution command prefix\n";
    for (const auto &command : commands)
      std::cerr << "  " << command << "\n";
    return false;
  }

  return true;
}

bool TestVirtualAudioServicePreservesUnavailableConfiguredSource() {
  std::vector<std::string> commands;
  ScopedPactlExecHook hook([&](const std::string &command) {
    commands.push_back(command);
    if (command == "pactl list short sources 2>&1") {
      return ExecResult(0,
                        "0\talsa_input.other_mic\tmodule-alsa-card.c\t"
                        "s16le 1ch 48000Hz\tRUNNING\n");
    }
    return ExecResult(99, "unexpected command: " + command);
  });

  VirtualAudioServiceHooks hooks;
  hooks.sleep_for = [](std::chrono::milliseconds) {
    std::this_thread::sleep_for(1ms);
  };

  VirtualAudioService service(std::move(hooks));
  VirtualAudioServiceConfig cfg;
  cfg.enabled = false;
  cfg.create_virtual_mic = false;
  cfg.source_name = "alsa_input.disconnected_mic";
  cfg.poll_ms = 1;

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  const bool reported = WaitUntil(
      [&] {
        const auto status = service.Status();
        return status.selected_source == "alsa_input.disconnected_mic" &&
               status.source_availability == "unavailable" &&
               status.source_error.find("not currently available") !=
                   std::string::npos;
      },
      250ms);
  const auto status = service.Status();
  const auto preservedConfig = service.Config();
  service.Stop();

  if (!reported) {
    std::cerr << "unavailable configured source was not reported; selected='"
              << status.selected_source << "' availability='"
              << status.source_availability << "' error='"
              << status.source_error << "'\n";
    return false;
  }
  if (preservedConfig.source_name != "alsa_input.disconnected_mic") {
    std::cerr << "configured source was mutated; got '"
              << preservedConfig.source_name << "'\n";
    return false;
  }

  const std::vector<std::string> expected = {
      "pactl list short sources 2>&1",
  };
  if (commands.size() < expected.size() ||
      !std::equal(expected.begin(), expected.end(), commands.begin())) {
    std::cerr << "unexpected unavailable-source command prefix\n";
    for (const auto &command : commands)
      std::cerr << "  " << command << "\n";
    return false;
  }

  return true;
}

bool TestSpeakerTargetSafetyRejectsVirtualAndMonitorEndpoints() {
  std::string reason;
  if (!studiocast::audio::IsUnsafeSpeakerTargetSinkName("studiocast_sink",
                                                        &reason) ||
      reason.find("feedback loop") == std::string::npos) {
    std::cerr << "virtual speaker target was not rejected; reason='" << reason
              << "'\n";
    return false;
  }

  reason.clear();
  if (!studiocast::audio::IsUnsafeSpeakerTargetSinkName(
          "alsa_output.pci_test.monitor", &reason) ||
      reason.find("monitor source") == std::string::npos) {
    std::cerr << "monitor endpoint target was not rejected; reason='" << reason
              << "'\n";
    return false;
  }

  std::string err;
  const auto chosen = studiocast::audio::ChooseSafeSpeakerTargetSinkName(
      "studiocast_sink", &err);
  if (chosen || err.find("feedback loop") == std::string::npos) {
    std::cerr << "virtual target chooser did not reject sink; chosen='"
              << (chosen ? *chosen : std::string("<none>")) << "' error='"
              << err << "'\n";
    return false;
  }

  return true;
}

bool TestSpeakerTargetAutoFallsBackFromUnsafeDefaultSink() {
  std::vector<std::string> commands;
  ScopedPactlExecHook hook([&](const std::string &command) {
    commands.push_back(command);
    if (command == "pactl get-default-sink 2>&1")
      return ExecResult(0, "studiocast_speakers\n");
    if (command == "pactl list short sinks 2>&1") {
      return ExecResult(0, "0\tstudiocast_speakers\tmodule-null-sink.c\t"
                           "s16le 2ch 48000Hz\tIDLE\n"
                           "1\tstudiocast_sink\tmodule-null-sink.c\t"
                           "s16le 2ch 48000Hz\tIDLE\n"
                           "2\tphysical_test_sink\tmodule-alsa-card.c\t"
                           "s16le 2ch 48000Hz\tRUNNING\n");
    }
    return ExecResult(99, "unexpected command: " + command);
  });

  std::string err;
  const auto chosen = studiocast::audio::ChooseSafeSpeakerTargetSinkName(
      /*configured_target=*/"", &err);
  if (!chosen || *chosen != "physical_test_sink") {
    std::cerr << "speaker auto target did not fall back to physical sink; "
              << "chosen='" << (chosen ? *chosen : std::string("<none>"))
              << "' error='" << err << "'\n";
    return false;
  }

  const std::vector<std::string> expected = {
      "pactl get-default-sink 2>&1",
      "pactl list short sinks 2>&1",
  };
  if (commands != expected) {
    std::cerr << "unexpected speaker target command sequence\n";
    for (const auto &command : commands)
      std::cerr << "  " << command << "\n";
    return false;
  }

  return true;
}

bool TestStatusTextSurfacesModuleListFailure() {
  ScopedXdgStateHome state_home;
  std::vector<std::string> commands;
  ScopedPactlExecHook hook([&](const std::string &command) {
    commands.push_back(command);
    if (command == "pactl --version 2>&1")
      return ExecResult(0, "pactl 16.1\n");
    if (command == "pactl list short modules 2>&1")
      return ExecResult(1, "Failure: synthetic module list failure\n");
    if (command == "pactl list short sources 2>&1")
      return ExecResult(0);
    if (command == "pactl get-default-sink 2>&1")
      return ExecResult(0, "physical_test_sink\n");
    if (command == "pactl list short sinks 2>&1")
      return ExecResult(0, "2\tphysical_test_sink\tmodule-alsa-card.c\t"
                           "s16le 2ch 48000Hz\tRUNNING\n");
    return ExecResult(99, "unexpected command: " + command);
  });

  const std::string text = studiocast::audio::StatusText();

  const std::string unavailable = "loaded ids: unavailable";
  std::size_t unavailable_count = 0;
  std::size_t pos = 0;
  while ((pos = text.find(unavailable, pos)) != std::string::npos) {
    ++unavailable_count;
    pos += unavailable.size();
  }

  if (unavailable_count < 2 ||
      text.find("synthetic module list failure") == std::string::npos) {
    std::cerr << "status text did not surface module list failures:\n"
              << text << "\n";
    return false;
  }

  if (text.find("loaded ids: sink=none, remap=none, loopback=none") !=
          std::string::npos ||
      text.find("loaded ids: sink=none, loopback=none") != std::string::npos) {
    std::cerr << "status text rendered unknown loaded ids as none:\n"
              << text << "\n";
    return false;
  }

  return true;
}

bool TestCreateVirtualMicPropagatesListFailureWithoutLoading() {
  ScopedXdgStateHome state_home;
  VirtualMicState initial;
  initial.null_sink_module_id = 10;
  initial.remap_source_module_id = 11;
  std::string err;
  if (!studiocast::audio::SaveVirtualMicState(initial, &err)) {
    std::cerr << "failed to seed virtual mic state: " << err << "\n";
    return false;
  }

  std::vector<std::string> commands;
  ScopedPactlExecHook hook([&](const std::string &command) {
    commands.push_back(command);
    if (command == "pactl --version 2>&1")
      return ExecResult(0, "pactl 16.1\n");
    if (command == "pactl list short modules 2>&1")
      return ExecResult(1, "Failure: synthetic mic list failure\n");
    return ExecResult(99, "unexpected command: " + command);
  });

  err.clear();
  const bool ok = studiocast::audio::CreateVirtualMic(&err);
  if (ok || err.find("synthetic mic list failure") == std::string::npos) {
    std::cerr << "expected virtual mic create to fail on list failure; ok="
              << ok << " error='" << err << "'\n";
    return false;
  }

  if (CommandWasRun(commands, "pactl load-module")) {
    std::cerr << "virtual mic create loaded modules after list failure\n";
    for (const auto &command : commands)
      std::cerr << "  " << command << "\n";
    return false;
  }

  const auto state = studiocast::audio::LoadVirtualMicState();
  if (!state.null_sink_module_id || *state.null_sink_module_id != 10 ||
      !state.remap_source_module_id || *state.remap_source_module_id != 11) {
    std::cerr << "virtual mic create changed state after list failure; sink="
              << (state.null_sink_module_id
                      ? std::to_string(*state.null_sink_module_id)
                      : std::string("<none>"))
              << " remap="
              << (state.remap_source_module_id
                      ? std::to_string(*state.remap_source_module_id)
                      : std::string("<none>"))
              << "\n";
    return false;
  }

  return true;
}

bool TestCreateVirtualSpeakerPropagatesListFailureWithoutLoading() {
  ScopedXdgStateHome state_home;
  VirtualSpeakerState initial;
  initial.null_sink_module_id = 10;
  initial.loopback_module_id = 42;
  initial.loopback_target_sink_name = "physical_test_sink";
  std::string err;
  if (!studiocast::audio::SaveVirtualSpeakerState(initial, &err)) {
    std::cerr << "failed to seed virtual speaker state: " << err << "\n";
    return false;
  }

  std::vector<std::string> commands;
  ScopedPactlExecHook hook([&](const std::string &command) {
    commands.push_back(command);
    if (command == "pactl --version 2>&1")
      return ExecResult(0, "pactl 16.1\n");
    if (command == "pactl list short modules 2>&1")
      return ExecResult(1, "Failure: synthetic speaker list failure\n");
    return ExecResult(99, "unexpected command: " + command);
  });

  err.clear();
  const bool ok = studiocast::audio::CreateVirtualSpeaker(&err);
  if (ok || err.find("synthetic speaker list failure") == std::string::npos) {
    std::cerr << "expected virtual speaker create to fail on list failure; ok="
              << ok << " error='" << err << "'\n";
    return false;
  }

  if (CommandWasRun(commands, "pactl load-module")) {
    std::cerr << "virtual speaker create loaded modules after list failure\n";
    for (const auto &command : commands)
      std::cerr << "  " << command << "\n";
    return false;
  }

  const auto state = studiocast::audio::LoadVirtualSpeakerState();
  if (!state.null_sink_module_id || *state.null_sink_module_id != 10 ||
      !state.loopback_module_id || *state.loopback_module_id != 42 ||
      !state.loopback_target_sink_name ||
      *state.loopback_target_sink_name != "physical_test_sink") {
    std::cerr
        << "virtual speaker create changed state after list failure; sink="
        << (state.null_sink_module_id
                ? std::to_string(*state.null_sink_module_id)
                : std::string("<none>"))
        << " loopback="
        << (state.loopback_module_id ? std::to_string(*state.loopback_module_id)
                                     : std::string("<none>"))
        << " target='"
        << (state.loopback_target_sink_name ? *state.loopback_target_sink_name
                                            : std::string("<none>"))
        << "'\n";
    return false;
  }

  return true;
}

bool TestVirtualSpeakerLoopbackFallsBackFromVirtualDefaultSink() {
  ScopedXdgStateHome state_home;
  std::vector<std::string> commands;
  const std::string expected_loopback =
      "pactl load-module 'module-loopback' "
      "'source=studiocast_speakers.monitor' 'sink=physical_test_sink' "
      "'latency_msec=12' 2>&1";

  ScopedPactlExecHook hook([&](const std::string &command) {
    commands.push_back(command);
    if (command == "pactl --version 2>&1")
      return ExecResult(0, "pactl 16.1\n");
    if (command == "pactl list short modules 2>&1")
      return ExecResult(0);
    if (command.find("pactl load-module 'module-null-sink'") == 0)
      return ExecResult(0, "10\n");
    if (command.find("pactl update-sink-proplist 'studiocast_speakers'") == 0)
      return ExecResult(0);
    if (command == "pactl get-default-sink 2>&1")
      return ExecResult(0, "studiocast_speakers\n");
    if (command == "pactl list short sinks 2>&1") {
      return ExecResult(0, "0\tstudiocast_speakers\tmodule-null-sink.c\t"
                           "s16le 2ch 48000Hz\tRUNNING\n"
                           "1\tstudiocast_sink\tmodule-null-sink.c\t"
                           "s16le 2ch 48000Hz\tRUNNING\n"
                           "2\tphysical_test_sink\tmodule-alsa-card.c\t"
                           "s16le 2ch 48000Hz\tRUNNING\n");
    }
    if (command == expected_loopback)
      return ExecResult(0, "20\n");
    return ExecResult(99, "unexpected command: " + command);
  });

  std::string err;
  const bool ok = studiocast::audio::StartSpeakerLoopback("", 12, &err);
  if (!ok) {
    std::cerr << "expected speaker loopback fallback to succeed; error='" << err
              << "'\n";
    return false;
  }

  const auto state = studiocast::audio::LoadVirtualSpeakerState();
  if (!state.loopback_module_id || *state.loopback_module_id != 20 ||
      !state.loopback_target_sink_name ||
      *state.loopback_target_sink_name != "physical_test_sink") {
    std::cerr << "speaker loopback state did not record fallback sink; id="
              << (state.loopback_module_id
                      ? std::to_string(*state.loopback_module_id)
                      : std::string("<none>"))
              << " target='"
              << (state.loopback_target_sink_name
                      ? *state.loopback_target_sink_name
                      : std::string("<none>"))
              << "'\n";
    return false;
  }

  if (!CommandWasRun(commands, expected_loopback) ||
      CommandWasRun(commands, "'sink=studiocast_speakers'")) {
    std::cerr << "speaker loopback did not use the physical fallback sink\n";
    for (const auto &command : commands)
      std::cerr << "  " << command << "\n";
    return false;
  }

  return true;
}

bool TestVirtualSpeakerLoopbackRejectsVirtualTarget() {
  ScopedXdgStateHome state_home;
  std::vector<std::string> commands;
  ScopedPactlExecHook hook([&](const std::string &command) {
    commands.push_back(command);
    if (command == "pactl --version 2>&1")
      return ExecResult(0, "pactl 16.1\n");
    if (command == "pactl list short modules 2>&1")
      return ExecResult(0);
    if (command.find("pactl load-module 'module-null-sink'") == 0)
      return ExecResult(0, "10\n");
    if (command.find("pactl update-sink-proplist 'studiocast_speakers'") == 0)
      return ExecResult(0);
    return ExecResult(99, "unexpected command: " + command);
  });

  std::string err;
  const bool ok =
      studiocast::audio::StartSpeakerLoopback("studiocast_sink", 10, &err);
  if (ok || err.find("feedback loop") == std::string::npos) {
    std::cerr << "expected virtual speaker target to be rejected; ok=" << ok
              << " error='" << err << "'\n";
    return false;
  }

  if (CommandWasRun(commands, "pactl load-module 'module-loopback'")) {
    std::cerr << "module-loopback was loaded for an invalid virtual target\n";
    for (const auto &command : commands)
      std::cerr << "  " << command << "\n";
    return false;
  }

  return true;
}

bool TestVirtualSpeakerLoopbackRejectsVirtualTargetBeforeStop() {
  ScopedXdgStateHome state_home;
  VirtualSpeakerState initial;
  initial.null_sink_module_id = 10;
  initial.loopback_module_id = 42;
  initial.loopback_target_sink_name = "physical_test_sink";
  std::string err;
  if (!studiocast::audio::SaveVirtualSpeakerState(initial, &err)) {
    std::cerr << "failed to seed virtual speaker state: " << err << "\n";
    return false;
  }

  std::vector<std::string> commands;
  ScopedPactlExecHook hook([&](const std::string &command) {
    commands.push_back(command);
    if (command == "pactl --version 2>&1")
      return ExecResult(0, "pactl 16.1\n");
    return ExecResult(99, "unexpected command: " + command);
  });

  err.clear();
  const bool ok =
      studiocast::audio::StartSpeakerLoopback("studiocast_sink", 10, &err);
  if (ok || err.find("feedback loop") == std::string::npos) {
    std::cerr
        << "expected virtual speaker target to be rejected before stop; ok="
        << ok << " error='" << err << "'\n";
    return false;
  }

  const auto state = studiocast::audio::LoadVirtualSpeakerState();
  if (!state.loopback_module_id || *state.loopback_module_id != 42 ||
      !state.loopback_target_sink_name ||
      *state.loopback_target_sink_name != "physical_test_sink") {
    std::cerr << "invalid target changed active speaker route state; id="
              << (state.loopback_module_id
                      ? std::to_string(*state.loopback_module_id)
                      : std::string("<none>"))
              << " target='"
              << (state.loopback_target_sink_name
                      ? *state.loopback_target_sink_name
                      : std::string("<none>"))
              << "'\n";
    return false;
  }

  if (CommandWasRun(commands, "pactl unload-module") ||
      CommandWasRun(commands, "pactl load-module")) {
    std::cerr << "invalid target performed destructive pactl operation\n";
    for (const auto &command : commands)
      std::cerr << "  " << command << "\n";
    return false;
  }

  return true;
}

bool TestVirtualSpeakerLoopbackRestartPropagatesStopFailure() {
  ScopedXdgStateHome state_home;
  VirtualSpeakerState initial;
  initial.null_sink_module_id = 10;
  initial.loopback_module_id = 42;
  initial.loopback_target_sink_name = "old_physical_sink";
  std::string err;
  if (!studiocast::audio::SaveVirtualSpeakerState(initial, &err)) {
    std::cerr << "failed to seed virtual speaker state: " << err << "\n";
    return false;
  }

  std::vector<std::string> commands;
  ScopedPactlExecHook hook([&](const std::string &command) {
    commands.push_back(command);
    if (command == "pactl --version 2>&1")
      return ExecResult(0, "pactl 16.1\n");
    if (command == "pactl list short modules 2>&1") {
      return ExecResult(
          0, "10\tmodule-null-sink\tsink_name=studiocast_speakers\n"
             "42\tmodule-loopback\tsource=studiocast_speakers.monitor "
             "sink=old_physical_sink\n");
    }
    if (command.find("pactl update-sink-proplist 'studiocast_speakers'") == 0)
      return ExecResult(0);
    if (command == "pactl unload-module 42 2>&1")
      return ExecResult(1, "Failure: synthetic unload failure\n");
    return ExecResult(99, "unexpected command: " + command);
  });

  err.clear();
  const bool ok =
      studiocast::audio::StartSpeakerLoopback("new_physical_sink", 10, &err);
  if (ok || err.find("synthetic unload failure") == std::string::npos) {
    std::cerr << "expected loopback restart to fail on unload failure; ok="
              << ok << " error='" << err << "'\n";
    return false;
  }

  const auto state = studiocast::audio::LoadVirtualSpeakerState();
  if (!state.loopback_module_id || *state.loopback_module_id != 42 ||
      !state.loopback_target_sink_name ||
      *state.loopback_target_sink_name != "old_physical_sink") {
    std::cerr << "failed loopback stop did not preserve active state; id="
              << (state.loopback_module_id
                      ? std::to_string(*state.loopback_module_id)
                      : std::string("<none>"))
              << " target='"
              << (state.loopback_target_sink_name
                      ? *state.loopback_target_sink_name
                      : std::string("<none>"))
              << "'\n";
    return false;
  }

  if (CommandWasRun(commands, "pactl load-module 'module-loopback'")) {
    std::cerr << "new module-loopback was loaded after stop failure\n";
    for (const auto &command : commands)
      std::cerr << "  " << command << "\n";
    return false;
  }

  return true;
}

bool TestDestroyVirtualSpeakerPropagatesNullSinkUnloadFailure() {
  ScopedXdgStateHome state_home;
  VirtualSpeakerState initial;
  initial.null_sink_module_id = 10;
  initial.loopback_module_id = 42;
  initial.loopback_target_sink_name = "old_physical_sink";
  std::string err;
  if (!studiocast::audio::SaveVirtualSpeakerState(initial, &err)) {
    std::cerr << "failed to seed virtual speaker state: " << err << "\n";
    return false;
  }

  std::vector<std::string> commands;
  ScopedPactlExecHook hook([&](const std::string &command) {
    commands.push_back(command);
    if (command == "pactl --version 2>&1")
      return ExecResult(0, "pactl 16.1\n");
    if (command == "pactl list short modules 2>&1") {
      return ExecResult(
          0, "10\tmodule-null-sink\tsink_name=studiocast_speakers\n");
    }
    if (command == "pactl unload-module 10 2>&1")
      return ExecResult(1, "Failure: synthetic null unload failure\n");
    return ExecResult(99, "unexpected command: " + command);
  });

  err.clear();
  const bool ok = studiocast::audio::DestroyVirtualSpeaker(&err);
  if (ok || err.find("synthetic null unload failure") == std::string::npos) {
    std::cerr << "expected virtual speaker destroy to fail on unload failure; "
              << "ok=" << ok << " error='" << err << "'\n";
    return false;
  }

  const auto state = studiocast::audio::LoadVirtualSpeakerState();
  if (!state.null_sink_module_id || *state.null_sink_module_id != 10) {
    std::cerr << "failed virtual speaker destroy cleared null sink state\n";
    return false;
  }

  return true;
}

bool TestVirtualMicStopLoopbackPropagatesUnloadFailure() {
  ScopedXdgStateHome state_home;
  VirtualMicState initial;
  initial.null_sink_module_id = 10;
  initial.remap_source_module_id = 11;
  initial.loopback_module_id = 42;
  std::string err;
  if (!studiocast::audio::SaveVirtualMicState(initial, &err)) {
    std::cerr << "failed to seed virtual mic state: " << err << "\n";
    return false;
  }

  std::vector<std::string> commands;
  ScopedPactlExecHook hook([&](const std::string &command) {
    commands.push_back(command);
    if (command == "pactl --version 2>&1")
      return ExecResult(0, "pactl 16.1\n");
    if (command == "pactl list short modules 2>&1") {
      return ExecResult(0,
                        "10\tmodule-null-sink\tsink_name=studiocast_sink\n"
                        "11\tmodule-remap-source\tsource_name=studiocast_mic\n"
                        "42\tmodule-loopback\tsource=physical_test_mic "
                        "sink=studiocast_sink\n");
    }
    if (command == "pactl unload-module 42 2>&1")
      return ExecResult(1, "Failure: synthetic mic loopback unload failure\n");
    return ExecResult(99, "unexpected command: " + command);
  });

  err.clear();
  const bool ok = studiocast::audio::StopLoopback(&err);
  if (ok ||
      err.find("synthetic mic loopback unload failure") == std::string::npos) {
    std::cerr << "expected mic loopback stop to fail on unload failure; ok="
              << ok << " error='" << err << "'\n";
    return false;
  }

  const auto state = studiocast::audio::LoadVirtualMicState();
  if (!state.loopback_module_id || *state.loopback_module_id != 42) {
    std::cerr << "failed mic loopback stop cleared active loopback state\n";
    return false;
  }

  if (CommandWasRun(commands, "pactl load-module")) {
    std::cerr << "mic loopback stop unexpectedly loaded a module\n";
    for (const auto &command : commands)
      std::cerr << "  " << command << "\n";
    return false;
  }

  return true;
}

bool TestDestroyVirtualMicPreservesRemainingStateOnNullUnloadFailure() {
  ScopedXdgStateHome state_home;
  VirtualMicState initial;
  initial.null_sink_module_id = 10;
  initial.remap_source_module_id = 11;
  std::string err;
  if (!studiocast::audio::SaveVirtualMicState(initial, &err)) {
    std::cerr << "failed to seed virtual mic state: " << err << "\n";
    return false;
  }

  std::vector<std::string> commands;
  ScopedPactlExecHook hook([&](const std::string &command) {
    commands.push_back(command);
    if (command == "pactl --version 2>&1")
      return ExecResult(0, "pactl 16.1\n");
    if (command == "pactl list short modules 2>&1") {
      return ExecResult(
          0, "10\tmodule-null-sink\tsink_name=studiocast_sink\n"
             "11\tmodule-remap-source\tsource_name=studiocast_mic\n");
    }
    if (command == "pactl unload-module 11 2>&1")
      return ExecResult(0);
    if (command == "pactl unload-module 10 2>&1")
      return ExecResult(1, "Failure: synthetic mic null unload failure\n");
    return ExecResult(99, "unexpected command: " + command);
  });

  err.clear();
  const bool ok = studiocast::audio::DestroyVirtualMic(&err);
  if (ok ||
      err.find("synthetic mic null unload failure") == std::string::npos) {
    std::cerr << "expected virtual mic destroy to fail on null sink unload; "
              << "ok=" << ok << " error='" << err << "'\n";
    return false;
  }

  const auto state = studiocast::audio::LoadVirtualMicState();
  if (!state.null_sink_module_id || *state.null_sink_module_id != 10 ||
      state.remap_source_module_id) {
    std::cerr << "failed virtual mic destroy did not preserve remaining state; "
              << "sink="
              << (state.null_sink_module_id
                      ? std::to_string(*state.null_sink_module_id)
                      : std::string("<none>"))
              << " remap="
              << (state.remap_source_module_id
                      ? std::to_string(*state.remap_source_module_id)
                      : std::string("<none>"))
              << "\n";
    return false;
  }

  return true;
}

bool TestMicrophonePipelineDoesNotStartWithoutConsumer() {
  std::atomic<int> pipeline_creates{0};
  std::atomic<bool> mic_consumer_present{false};

  VirtualAudioServiceHooks hooks;
  HookMicrophoneConsumerFlag(&hooks, &mic_consumer_present);
  hooks.create_pipeline =
      [&](AudioProcessor *) -> std::unique_ptr<AudioPipelineRunner> {
    pipeline_creates.fetch_add(1, std::memory_order_relaxed);
    return std::make_unique<FixedStatsPipeline>(true, "");
  };
  hooks.sleep_for = [](std::chrono::milliseconds) {
    std::this_thread::sleep_for(1ms);
  };

  VirtualAudioService service(std::move(hooks));
  VirtualAudioServiceConfig cfg;
  cfg.enabled = true;
  cfg.create_virtual_mic = false;
  cfg.create_virtual_speakers = false;
  cfg.poll_ms = 1;
  cfg.consumer_grace_ms = 0;

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  const bool saw_idle = WaitUntil(
      [&] {
        const auto status = service.Status();
        return status.pipeline_state == "idle_no_consumer" &&
               !status.pipeline_active_needed && !status.pipeline_running &&
               !status.mic_consumer_present;
      },
      250ms);
  std::this_thread::sleep_for(30ms);
  const int creates = pipeline_creates.load(std::memory_order_relaxed);
  const auto status = service.Status();
  service.Stop();

  if (!saw_idle || creates != 0) {
    std::cerr << "microphone pipeline did not stay idle without consumer; "
              << "creates=" << creates << " state='" << status.pipeline_state
              << "' idle='" << status.pipeline_idle_reason
              << "' running=" << status.pipeline_running << "\n";
    return false;
  }

  return true;
}

bool TestMicrophonePipelineStartsWhenConsumerAppears() {
  std::atomic<int> pipeline_creates{0};
  std::atomic<bool> mic_consumer_present{false};

  VirtualAudioServiceHooks hooks;
  HookMicrophoneConsumerFlag(&hooks, &mic_consumer_present);
  hooks.create_pipeline =
      [&](AudioProcessor *) -> std::unique_ptr<AudioPipelineRunner> {
    pipeline_creates.fetch_add(1, std::memory_order_relaxed);
    return std::make_unique<FixedStatsPipeline>(true, "");
  };
  hooks.sleep_for = [](std::chrono::milliseconds) {
    std::this_thread::sleep_for(1ms);
  };

  VirtualAudioService service(std::move(hooks));
  VirtualAudioServiceConfig cfg;
  cfg.enabled = true;
  cfg.create_virtual_mic = false;
  cfg.create_virtual_speakers = false;
  cfg.poll_ms = 1;
  cfg.consumer_grace_ms = 0;

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  if (!WaitUntil(
          [&] { return service.Status().pipeline_state == "idle_no_consumer"; },
          250ms)) {
    std::cerr << "microphone pipeline did not reach no-consumer idle state\n";
    service.Stop();
    return false;
  }

  mic_consumer_present.store(true, std::memory_order_relaxed);
  const bool started = WaitUntil(
      [&] {
        const auto status = service.Status();
        return pipeline_creates.load(std::memory_order_relaxed) >= 1 &&
               status.pipeline_running && status.pipeline_active_needed &&
               status.pipeline_state == "running" &&
               status.mic_consumer_present;
      },
      250ms);
  const auto status = service.Status();
  service.Stop();

  if (!started) {
    std::cerr << "microphone pipeline did not start after consumer appeared; "
              << "creates=" << pipeline_creates.load() << " state='"
              << status.pipeline_state
              << "' running=" << status.pipeline_running << "\n";
    return false;
  }

  return true;
}

bool TestMicrophonePipelineStopsWhenConsumerDisappears() {
  std::atomic<int> pipeline_creates{0};
  std::atomic<int> pipeline_stops{0};
  std::atomic<bool> mic_consumer_present{true};

  VirtualAudioServiceHooks hooks;
  HookMicrophoneConsumerFlag(&hooks, &mic_consumer_present);
  hooks.create_pipeline =
      [&](AudioProcessor *) -> std::unique_ptr<AudioPipelineRunner> {
    pipeline_creates.fetch_add(1, std::memory_order_relaxed);
    return std::make_unique<FixedStatsPipeline>(true, "", &pipeline_stops);
  };
  hooks.sleep_for = [](std::chrono::milliseconds) {
    std::this_thread::sleep_for(1ms);
  };

  VirtualAudioService service(std::move(hooks));
  VirtualAudioServiceConfig cfg;
  cfg.enabled = true;
  cfg.create_virtual_mic = false;
  cfg.create_virtual_speakers = false;
  cfg.poll_ms = 1;
  cfg.consumer_grace_ms = 0;

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  if (!WaitUntil(
          [&] {
            return pipeline_creates.load(std::memory_order_relaxed) >= 1 &&
                   service.Status().pipeline_running;
          },
          250ms)) {
    std::cerr << "microphone pipeline did not start before consumer vanished\n";
    service.Stop();
    return false;
  }

  mic_consumer_present.store(false, std::memory_order_relaxed);
  const bool stopped = WaitUntil(
      [&] {
        const auto status = service.Status();
        return pipeline_stops.load(std::memory_order_relaxed) >= 1 &&
               !status.pipeline_running &&
               status.pipeline_state == "idle_no_consumer" &&
               !status.pipeline_active_needed;
      },
      250ms);
  const auto status = service.Status();
  service.Stop();

  if (!stopped) {
    std::cerr << "microphone pipeline did not stop after consumer disappeared; "
              << "stops=" << pipeline_stops.load() << " state='"
              << status.pipeline_state
              << "' running=" << status.pipeline_running << "\n";
    return false;
  }

  return true;
}

bool TestMicrophoneGraceWindowAbsorbsConsumerFlapping() {
  std::atomic<int> pipeline_creates{0};
  std::atomic<int> pipeline_stops{0};
  std::atomic<bool> mic_consumer_present{true};

  VirtualAudioServiceHooks hooks;
  HookMicrophoneConsumerFlag(&hooks, &mic_consumer_present);
  hooks.create_pipeline =
      [&](AudioProcessor *) -> std::unique_ptr<AudioPipelineRunner> {
    pipeline_creates.fetch_add(1, std::memory_order_relaxed);
    return std::make_unique<FixedStatsPipeline>(true, "", &pipeline_stops);
  };
  hooks.sleep_for = [](std::chrono::milliseconds) {
    std::this_thread::sleep_for(1ms);
  };

  VirtualAudioService service(std::move(hooks));
  VirtualAudioServiceConfig cfg;
  cfg.enabled = true;
  cfg.create_virtual_mic = false;
  cfg.create_virtual_speakers = false;
  cfg.poll_ms = 1;
  cfg.consumer_grace_ms = 200;

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  if (!WaitUntil(
          [&] {
            const auto status = service.Status();
            return pipeline_creates.load(std::memory_order_relaxed) == 1 &&
                   status.pipeline_running &&
                   status.pipeline_state == "running";
          },
          250ms)) {
    std::cerr << "microphone pipeline did not start before flapping test\n";
    service.Stop();
    return false;
  }

  for (int i = 0; i < 5; ++i) {
    mic_consumer_present.store(false, std::memory_order_relaxed);
    std::this_thread::sleep_for(10ms);
    const auto absent_status = service.Status();
    if (pipeline_stops.load(std::memory_order_relaxed) != 0 ||
        !absent_status.pipeline_running ||
        !absent_status.pipeline_active_needed) {
      std::cerr << "microphone pipeline churned during grace window; i=" << i
                << " creates=" << pipeline_creates.load()
                << " stops=" << pipeline_stops.load()
                << " running=" << absent_status.pipeline_running
                << " needed=" << absent_status.pipeline_active_needed
                << " state='" << absent_status.pipeline_state << "'\n";
      service.Stop();
      return false;
    }

    mic_consumer_present.store(true, std::memory_order_relaxed);
    if (!WaitUntil(
            [&] {
              const auto status = service.Status();
              return status.pipeline_running && status.mic_consumer_present;
            },
            100ms)) {
      std::cerr << "microphone consumer did not recover during flap cycle\n";
      service.Stop();
      return false;
    }
  }

  if (pipeline_creates.load(std::memory_order_relaxed) != 1 ||
      pipeline_stops.load(std::memory_order_relaxed) != 0) {
    std::cerr << "microphone pipeline restarted during flapping; creates="
              << pipeline_creates.load() << " stops=" << pipeline_stops.load()
              << "\n";
    service.Stop();
    return false;
  }

  mic_consumer_present.store(false, std::memory_order_relaxed);
  const bool stopped = WaitUntil(
      [&] {
        const auto status = service.Status();
        return pipeline_stops.load(std::memory_order_relaxed) == 1 &&
               !status.pipeline_running && !status.pipeline_active_needed &&
               status.pipeline_state == "idle_no_consumer";
      },
      700ms);
  const auto status = service.Status();
  service.Stop();

  if (!stopped) {
    std::cerr << "microphone pipeline did not stop after sustained absence; "
              << "creates=" << pipeline_creates.load()
              << " stops=" << pipeline_stops.load()
              << " running=" << status.pipeline_running
              << " needed=" << status.pipeline_active_needed << " state='"
              << status.pipeline_state << "'\n";
    return false;
  }

  return true;
}

bool TestMicrophoneConsumerDetectionRecoversAfterErrors() {
  std::atomic<int> pipeline_creates{0};
  std::atomic<int> detection_stage{0};

  VirtualAudioServiceHooks hooks;
  hooks.detect_microphone_consumers = [&] {
    AudioConsumerSnapshot out;
    const int stage = detection_stage.load(std::memory_order_relaxed);
    if (stage == 0) {
      out.error = "synthetic Pulse server restart";
    } else if (stage == 1) {
      out.error = "Pulse source 'studiocast_mic' is not present.";
    } else {
      out = ConsumerSnapshot(true, 1);
    }
    return out;
  };
  hooks.create_pipeline =
      [&](AudioProcessor *) -> std::unique_ptr<AudioPipelineRunner> {
    pipeline_creates.fetch_add(1, std::memory_order_relaxed);
    return std::make_unique<FixedStatsPipeline>(true, "");
  };
  hooks.sleep_for = [](std::chrono::milliseconds) {
    std::this_thread::sleep_for(1ms);
  };

  VirtualAudioService service(std::move(hooks));
  VirtualAudioServiceConfig cfg;
  cfg.enabled = true;
  cfg.create_virtual_mic = false;
  cfg.create_virtual_speakers = false;
  cfg.poll_ms = 1;
  cfg.consumer_grace_ms = 0;

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  if (!WaitUntil(
          [&] {
            const auto status = service.Status();
            return status.pipeline_state == "idle_no_consumer" &&
                   status.mic_consumer_error.find("server restart") !=
                       std::string::npos &&
                   pipeline_creates.load(std::memory_order_relaxed) == 0;
          },
          250ms)) {
    const auto status = service.Status();
    std::cerr << "microphone detection error was not surfaced; state='"
              << status.pipeline_state << "' error='"
              << status.mic_consumer_error
              << "' creates=" << pipeline_creates.load() << "\n";
    service.Stop();
    return false;
  }

  detection_stage.store(1, std::memory_order_relaxed);
  if (!WaitUntil(
          [&] {
            const auto status = service.Status();
            return status.pipeline_state == "idle_no_consumer" &&
                   status.mic_consumer_error.find("not present") !=
                       std::string::npos &&
                   pipeline_creates.load(std::memory_order_relaxed) == 0;
          },
          250ms)) {
    const auto status = service.Status();
    std::cerr << "missing virtual mic source was not surfaced; state='"
              << status.pipeline_state << "' error='"
              << status.mic_consumer_error
              << "' creates=" << pipeline_creates.load() << "\n";
    service.Stop();
    return false;
  }

  detection_stage.store(2, std::memory_order_relaxed);
  const bool recovered = WaitUntil(
      [&] {
        const auto status = service.Status();
        return pipeline_creates.load(std::memory_order_relaxed) == 1 &&
               status.pipeline_running && status.pipeline_state == "running" &&
               status.mic_consumer_present && status.mic_consumer_error.empty();
      },
      250ms);
  const auto status = service.Status();
  service.Stop();

  if (!recovered) {
    std::cerr << "microphone pipeline did not recover after detection errors; "
              << "creates=" << pipeline_creates.load() << " state='"
              << status.pipeline_state
              << "' running=" << status.pipeline_running
              << " consumer=" << status.mic_consumer_present << " error='"
              << status.mic_consumer_error << "'\n";
    return false;
  }

  return true;
}

bool TestSpeakerPipelineFollowsConsumerGate() {
  std::atomic<int> pipeline_creates{0};
  std::atomic<int> pipeline_stops{0};
  std::atomic<bool> speaker_consumer_present{false};

  VirtualAudioServiceHooks hooks;
  HookSpeakerConsumerFlag(&hooks, &speaker_consumer_present);
  hooks.create_virtual_speaker = [](std::string *error) {
    if (error)
      error->clear();
    return true;
  };
  hooks.probe_speaker_backend_availability =
      [](const VirtualAudioServiceConfig &) {
        AudioBackendAvailability avail;
        avail.maxine_reason = "synthetic maxine unavailable";
        avail.open_source_reason = "synthetic open audio unavailable";
        return avail;
      };
  hooks.create_pipeline =
      [&](AudioProcessor *) -> std::unique_ptr<AudioPipelineRunner> {
    pipeline_creates.fetch_add(1, std::memory_order_relaxed);
    return std::make_unique<FixedStatsPipeline>(true, "", &pipeline_stops);
  };
  hooks.sleep_for = [](std::chrono::milliseconds) {
    std::this_thread::sleep_for(1ms);
  };

  VirtualAudioService service(std::move(hooks));
  VirtualAudioServiceConfig cfg;
  cfg.enabled = false;
  cfg.create_virtual_mic = false;
  cfg.create_virtual_speakers = true;
  cfg.speakers_enabled = true;
  cfg.speaker_target_sink = "physical_test_sink";
  cfg.effects.speaker.noise_removal_enabled = true;
  cfg.poll_ms = 1;
  cfg.consumer_grace_ms = 0;

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  const bool saw_idle = WaitUntil(
      [&] {
        const auto status = service.Status();
        return status.speakers_pipeline_state == "idle_no_consumer" &&
               !status.speakers_pipeline_active_needed &&
               !status.speakers_pipeline_running &&
               !status.speakers_consumer_present;
      },
      250ms);
  std::this_thread::sleep_for(30ms);
  if (!saw_idle || pipeline_creates.load(std::memory_order_relaxed) != 0) {
    const auto status = service.Status();
    std::cerr << "speaker pipeline did not stay idle without consumer; creates="
              << pipeline_creates.load() << " state='"
              << status.speakers_pipeline_state
              << "' running=" << status.speakers_pipeline_running << "\n";
    service.Stop();
    return false;
  }

  speaker_consumer_present.store(true, std::memory_order_relaxed);
  const bool started = WaitUntil(
      [&] {
        const auto status = service.Status();
        return pipeline_creates.load(std::memory_order_relaxed) >= 1 &&
               status.speakers_pipeline_running &&
               status.speakers_pipeline_active_needed &&
               status.speakers_pipeline_state == "running";
      },
      250ms);
  if (!started) {
    const auto status = service.Status();
    std::cerr << "speaker pipeline did not start after consumer appeared; "
              << "creates=" << pipeline_creates.load() << " state='"
              << status.speakers_pipeline_state
              << "' running=" << status.speakers_pipeline_running << "\n";
    service.Stop();
    return false;
  }

  speaker_consumer_present.store(false, std::memory_order_relaxed);
  const bool stopped = WaitUntil(
      [&] {
        const auto status = service.Status();
        return pipeline_stops.load(std::memory_order_relaxed) >= 1 &&
               !status.speakers_pipeline_running &&
               status.speakers_pipeline_state == "idle_no_consumer" &&
               !status.speakers_pipeline_active_needed;
      },
      250ms);
  const auto status = service.Status();
  service.Stop();

  if (!stopped) {
    std::cerr << "speaker pipeline did not stop after consumer disappeared; "
              << "stops=" << pipeline_stops.load() << " state='"
              << status.speakers_pipeline_state
              << "' running=" << status.speakers_pipeline_running << "\n";
    return false;
  }

  return true;
}

bool TestSpeakerGraceWindowAbsorbsConsumerFlapping() {
  std::atomic<int> pipeline_creates{0};
  std::atomic<int> pipeline_stops{0};
  std::atomic<bool> speaker_consumer_present{true};

  VirtualAudioServiceHooks hooks;
  HookSpeakerConsumerFlag(&hooks, &speaker_consumer_present);
  hooks.create_virtual_speaker = [](std::string *error) {
    if (error)
      error->clear();
    return true;
  };
  hooks.probe_speaker_backend_availability =
      [](const VirtualAudioServiceConfig &) {
        AudioBackendAvailability avail;
        avail.maxine_reason = "synthetic maxine unavailable";
        avail.open_source_reason = "synthetic open audio unavailable";
        return avail;
      };
  hooks.create_pipeline =
      [&](AudioProcessor *) -> std::unique_ptr<AudioPipelineRunner> {
    pipeline_creates.fetch_add(1, std::memory_order_relaxed);
    return std::make_unique<FixedStatsPipeline>(true, "", &pipeline_stops);
  };
  hooks.sleep_for = [](std::chrono::milliseconds) {
    std::this_thread::sleep_for(1ms);
  };

  VirtualAudioService service(std::move(hooks));
  VirtualAudioServiceConfig cfg;
  cfg.enabled = false;
  cfg.create_virtual_mic = false;
  cfg.create_virtual_speakers = true;
  cfg.speakers_enabled = true;
  cfg.speaker_target_sink = "physical_test_sink";
  cfg.effects.speaker.noise_removal_enabled = true;
  cfg.poll_ms = 1;
  cfg.consumer_grace_ms = 200;

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  if (!WaitUntil(
          [&] {
            const auto status = service.Status();
            return pipeline_creates.load(std::memory_order_relaxed) == 1 &&
                   status.speakers_pipeline_running &&
                   status.speakers_pipeline_state == "running";
          },
          250ms)) {
    std::cerr << "speaker pipeline did not start before flapping test\n";
    service.Stop();
    return false;
  }

  for (int i = 0; i < 5; ++i) {
    speaker_consumer_present.store(false, std::memory_order_relaxed);
    std::this_thread::sleep_for(10ms);
    const auto absent_status = service.Status();
    if (pipeline_stops.load(std::memory_order_relaxed) != 0 ||
        !absent_status.speakers_pipeline_running ||
        !absent_status.speakers_pipeline_active_needed) {
      std::cerr << "speaker pipeline churned during grace window; i=" << i
                << " creates=" << pipeline_creates.load()
                << " stops=" << pipeline_stops.load()
                << " running=" << absent_status.speakers_pipeline_running
                << " needed=" << absent_status.speakers_pipeline_active_needed
                << " state='" << absent_status.speakers_pipeline_state << "'\n";
      service.Stop();
      return false;
    }

    speaker_consumer_present.store(true, std::memory_order_relaxed);
    if (!WaitUntil(
            [&] {
              const auto status = service.Status();
              return status.speakers_pipeline_running &&
                     status.speakers_consumer_present;
            },
            100ms)) {
      std::cerr << "speaker consumer did not recover during flap cycle\n";
      service.Stop();
      return false;
    }
  }

  if (pipeline_creates.load(std::memory_order_relaxed) != 1 ||
      pipeline_stops.load(std::memory_order_relaxed) != 0) {
    std::cerr << "speaker pipeline restarted during flapping; creates="
              << pipeline_creates.load() << " stops=" << pipeline_stops.load()
              << "\n";
    service.Stop();
    return false;
  }

  speaker_consumer_present.store(false, std::memory_order_relaxed);
  const bool stopped = WaitUntil(
      [&] {
        const auto status = service.Status();
        return pipeline_stops.load(std::memory_order_relaxed) == 1 &&
               !status.speakers_pipeline_running &&
               !status.speakers_pipeline_active_needed &&
               status.speakers_pipeline_state == "idle_no_consumer";
      },
      700ms);
  const auto status = service.Status();
  service.Stop();

  if (!stopped) {
    std::cerr << "speaker pipeline did not stop after sustained absence; "
              << "creates=" << pipeline_creates.load()
              << " stops=" << pipeline_stops.load()
              << " running=" << status.speakers_pipeline_running
              << " needed=" << status.speakers_pipeline_active_needed
              << " state='" << status.speakers_pipeline_state << "'\n";
    return false;
  }

  return true;
}

bool TestSpeakerLoopbackPassThroughStatusIsNotConsumerGated() {
  std::atomic<int> loopback_start_calls{0};
  std::atomic<int> speaker_detection_calls{0};

  VirtualAudioServiceHooks hooks;
  hooks.create_virtual_speaker = [](std::string *error) {
    if (error)
      error->clear();
    return true;
  };
  hooks.start_speaker_loopback = [&](const std::string &, int,
                                     std::string *error) {
    loopback_start_calls.fetch_add(1, std::memory_order_relaxed);
    if (error)
      error->clear();
    return true;
  };
  hooks.detect_speaker_consumers = [&] {
    speaker_detection_calls.fetch_add(1, std::memory_order_relaxed);
    return ConsumerSnapshot(false, 0);
  };
  hooks.sleep_for = [](std::chrono::milliseconds) {
    std::this_thread::sleep_for(1ms);
  };

  VirtualAudioService service(std::move(hooks));
  VirtualAudioServiceConfig cfg;
  cfg.enabled = false;
  cfg.create_virtual_mic = false;
  cfg.create_virtual_speakers = true;
  cfg.speakers_enabled = true;
  cfg.speaker_target_sink = "physical_test_sink";
  cfg.poll_ms = 1;
  cfg.consumer_grace_ms = 0;

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  const bool active = WaitUntil(
      [&] {
        const auto status = service.Status();
        return loopback_start_calls.load(std::memory_order_relaxed) >= 1 &&
               status.speakers_route_mode == "loopback" &&
               status.speakers_routing_active &&
               status.speakers_pipeline_state == "disabled" &&
               status.speakers_pipeline_idle_reason ==
                   "Speaker processing is not requested.";
      },
      250ms);
  std::this_thread::sleep_for(30ms);
  const auto status = service.Status();
  const int detections =
      speaker_detection_calls.load(std::memory_order_relaxed);
  service.Stop();

  if (!active) {
    std::cerr << "speaker pass-through loopback status was not explicit; "
              << "starts=" << loopback_start_calls.load() << " route='"
              << status.speakers_route_mode
              << "' active=" << status.speakers_routing_active
              << " pipeline_state='" << status.speakers_pipeline_state
              << "' idle='" << status.speakers_pipeline_idle_reason << "'\n";
    return false;
  }

  if (detections != 0) {
    std::cerr << "speaker pass-through loopback was consumer-gated; "
              << "consumer detections=" << detections << "\n";
    return false;
  }

  return true;
}

bool TestMicrophonePipelineRestartsWhenWorkerDies() {
  std::atomic<int> pipeline_creates{0};
  std::atomic<int> pipeline_stops{0};
  std::atomic<int> sleep_calls{0};
  std::atomic<bool> mic_consumer_present{true};

  VirtualAudioServiceHooks hooks;
  HookMicrophoneConsumerFlag(&hooks, &mic_consumer_present);
  hooks.create_pipeline =
      [&](AudioProcessor *) -> std::unique_ptr<AudioPipelineRunner> {
    pipeline_creates.fetch_add(1, std::memory_order_relaxed);
    return std::make_unique<DeadPipeline>(&pipeline_stops);
  };
  hooks.sleep_for = [&](std::chrono::milliseconds) {
    sleep_calls.fetch_add(1, std::memory_order_relaxed);
    std::this_thread::sleep_for(1ms);
  };

  VirtualAudioService service(std::move(hooks));
  VirtualAudioServiceConfig cfg;
  cfg.enabled = true;
  cfg.create_virtual_mic = false;
  cfg.create_virtual_speakers = false;
  cfg.poll_ms = 1;
  cfg.start_retry_ms = 1;

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  const bool restarted =
      WaitUntil([&] { return pipeline_creates.load() >= 2; }, 250ms);
  service.Stop();

  if (!restarted) {
    std::cerr << "expected microphone pipeline to restart after worker death;"
              << " creates=" << pipeline_creates.load()
              << " sleeps=" << sleep_calls.load() << "\n";
    return false;
  }

  if (pipeline_stops.load(std::memory_order_relaxed) == 0) {
    std::cerr
        << "expected dead microphone pipeline to be stopped before restart\n";
    return false;
  }

  return true;
}

bool TestMicrophonePipelinePreservesWorkerDeathError() {
  std::atomic<int> pipeline_creates{0};
  std::atomic<int> pipeline_stops{0};
  std::atomic<bool> mic_consumer_present{true};

  VirtualAudioServiceHooks hooks;
  HookMicrophoneConsumerFlag(&hooks, &mic_consumer_present);
  hooks.create_pipeline =
      [&](AudioProcessor *) -> std::unique_ptr<AudioPipelineRunner> {
    const int create_index =
        pipeline_creates.fetch_add(1, std::memory_order_relaxed);
    if (create_index == 0) {
      return std::make_unique<FixedStatsPipeline>(
          false, "synthetic terminal pipeline failure", &pipeline_stops);
    }
    return std::make_unique<FixedStatsPipeline>(true, "", &pipeline_stops);
  };
  hooks.sleep_for = [&](std::chrono::milliseconds) {
    std::this_thread::sleep_for(1ms);
  };

  VirtualAudioService service(std::move(hooks));
  VirtualAudioServiceConfig cfg;
  cfg.enabled = true;
  cfg.create_virtual_mic = false;
  cfg.create_virtual_speakers = false;
  cfg.poll_ms = 1;
  cfg.start_retry_ms = 1;

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  const bool restarted =
      WaitUntil([&] { return pipeline_creates.load() >= 2; }, 250ms);
  const auto status = service.Status();
  service.Stop();

  if (!restarted) {
    std::cerr << "expected microphone pipeline to restart after terminal error;"
              << " creates=" << pipeline_creates.load() << "\n";
    return false;
  }

  if (status.last_error.find("synthetic terminal pipeline failure") ==
      std::string::npos) {
    std::cerr << "expected terminal pipeline error to remain visible; got '"
              << status.last_error << "'\n";
    return false;
  }

  return true;
}

bool TestStatusDoesNotBlockDuringRetrySleep() {
  std::mutex mu;
  std::condition_variable cv;
  bool sleep_entered = false;
  bool release_sleep = false;
  std::atomic<int> sleep_calls{0};
  std::atomic<bool> mic_consumer_present{true};

  VirtualAudioServiceHooks hooks;
  HookMicrophoneConsumerFlag(&hooks, &mic_consumer_present);
  hooks.create_pipeline =
      [&](AudioProcessor *) -> std::unique_ptr<AudioPipelineRunner> {
    return std::make_unique<StartFailPipeline>();
  };
  hooks.sleep_for = [&](std::chrono::milliseconds) {
    const int call_index = sleep_calls.fetch_add(1, std::memory_order_relaxed);
    if (call_index != 0) {
      std::this_thread::sleep_for(1ms);
      return;
    }

    std::unique_lock<std::mutex> lock(mu);
    sleep_entered = true;
    cv.notify_all();
    cv.wait(lock, [&] { return release_sleep; });
  };

  VirtualAudioService service(std::move(hooks));
  VirtualAudioServiceConfig cfg;
  cfg.enabled = true;
  cfg.create_virtual_mic = false;
  cfg.create_virtual_speakers = false;
  cfg.poll_ms = 1;
  cfg.start_retry_ms = 1;

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  {
    std::unique_lock<std::mutex> lock(mu);
    if (!cv.wait_for(lock, 250ms, [&] { return sleep_entered; })) {
      std::cerr << "service did not enter retry sleep\n";
      service.Stop();
      return false;
    }
  }

  auto status_future =
      std::async(std::launch::async, [&] { return service.Status(); });
  const bool ready_before_release =
      (status_future.wait_for(50ms) == std::future_status::ready);

  {
    std::lock_guard<std::mutex> lock(mu);
    release_sleep = true;
  }
  cv.notify_all();

  service.Stop();
  (void)status_future.get();

  if (!ready_before_release) {
    std::cerr << "Status() blocked while the service was backing off\n";
    return false;
  }

  return true;
}

bool TestMicrophoneNullPipelineFactoryFailsWithoutCrash() {
  std::atomic<bool> mic_consumer_present{true};
  VirtualAudioServiceHooks hooks;
  HookMicrophoneConsumerFlag(&hooks, &mic_consumer_present);
  hooks.create_pipeline =
      [](AudioProcessor *) -> std::unique_ptr<AudioPipelineRunner> {
    return nullptr;
  };
  hooks.sleep_for = [&](std::chrono::milliseconds) {
    std::this_thread::sleep_for(1ms);
  };

  VirtualAudioService service(std::move(hooks));
  VirtualAudioServiceConfig cfg;
  cfg.enabled = true;
  cfg.create_virtual_mic = false;
  cfg.create_virtual_speakers = false;
  cfg.poll_ms = 1;
  cfg.start_retry_ms = 250;

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  const bool saw_error = WaitUntil(
      [&] {
        const auto status = service.Status();
        return !status.pipeline_running &&
               status.last_error.find("pipeline factory returned null") !=
                   std::string::npos;
      },
      250ms);
  const auto status = service.Status();
  service.Stop();

  if (!saw_error) {
    std::cerr << "expected null microphone pipeline factory to surface an "
                 "error; got '"
              << status.last_error << "' running=" << status.pipeline_running
              << "\n";
    return false;
  }

  return true;
}

bool TestSpeakerPipelineStartFailureClearsRouteState() {
  std::atomic<int> pipeline_creates{0};
  std::atomic<bool> speaker_consumer_present{true};

  VirtualAudioServiceHooks hooks;
  HookSpeakerConsumerFlag(&hooks, &speaker_consumer_present);
  hooks.create_virtual_speaker = [](std::string *error) {
    if (error)
      error->clear();
    return true;
  };
  hooks.probe_speaker_backend_availability =
      [](const VirtualAudioServiceConfig &) {
        AudioBackendAvailability avail;
        avail.maxine_reason = "synthetic maxine unavailable";
        avail.open_source_reason = "synthetic open audio unavailable";
        return avail;
      };
  hooks.create_pipeline =
      [&](AudioProcessor *) -> std::unique_ptr<AudioPipelineRunner> {
    pipeline_creates.fetch_add(1, std::memory_order_relaxed);
    return std::make_unique<StartFailPipeline>();
  };
  hooks.sleep_for = [&](std::chrono::milliseconds) {
    std::this_thread::sleep_for(1ms);
  };

  VirtualAudioService service(std::move(hooks));
  VirtualAudioServiceConfig cfg;
  cfg.enabled = false;
  cfg.create_virtual_mic = false;
  cfg.create_virtual_speakers = false;
  cfg.speakers_enabled = true;
  cfg.speaker_target_sink = "physical_test_sink";
  cfg.effects.speaker.noise_removal_enabled = true;
  cfg.poll_ms = 1;
  cfg.start_retry_ms = 250;

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  const bool saw_failure = WaitUntil(
      [&] {
        const auto status = service.Status();
        return status.speakers_pipeline_last_error.find(
                   "synthetic start failure") != std::string::npos;
      },
      250ms);
  const auto status = service.Status();
  service.Stop();

  if (!saw_failure) {
    std::cerr << "expected speaker pipeline start failure; creates="
              << pipeline_creates.load() << " error='"
              << status.speakers_pipeline_last_error << "'\n";
    return false;
  }

  if (status.speakers_routing_active ||
      !status.speaker_target_sink_active.empty()) {
    std::cerr << "speaker route looked active after pipeline start failure; "
              << "routing=" << status.speakers_routing_active << " target='"
              << status.speaker_target_sink_active << "'\n";
    return false;
  }

  return true;
}

bool TestOpenAudioFailureCooldownAvoidsRestartChurn() {
  std::atomic<int> pipeline_creates{0};
  std::atomic<bool> mic_consumer_present{true};

  VirtualAudioServiceHooks hooks;
  HookMicrophoneConsumerFlag(&hooks, &mic_consumer_present);
  hooks.probe_microphone_backend_availability =
      [](const VirtualAudioServiceConfig &) {
        AudioBackendAvailability avail;
        avail.open_source_ok = true;
        return avail;
      };
  hooks.create_pipeline =
      [&](AudioProcessor *) -> std::unique_ptr<AudioPipelineRunner> {
    pipeline_creates.fetch_add(1, std::memory_order_relaxed);
    return std::make_unique<FixedStatsPipeline>(true, "");
  };
  hooks.sleep_for = [&](std::chrono::milliseconds) {
    std::this_thread::sleep_for(1ms);
  };

  VirtualAudioService service(std::move(hooks));
  VirtualAudioServiceConfig cfg;
  cfg.enabled = true;
  cfg.create_virtual_mic = false;
  cfg.create_virtual_speakers = false;
  cfg.effects.engine =
      studiocast::audio::effects::AudioEffectsEnginePreference::kOpenSource;
  cfg.effects.microphone.noise_removal_enabled = true;
  cfg.effects.microphone.model_path =
      "/tmp/studiocast-definitely-missing-open-audio-model.onnx";
  cfg.poll_ms = 1;
  cfg.start_retry_ms = 1000;

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  const bool started_fallback =
      WaitUntil([&] { return pipeline_creates.load() >= 1; }, 250ms);
  std::this_thread::sleep_for(75ms);
  const int creates_after_cooldown_window = pipeline_creates.load();
  const auto status = service.Status();
  service.Stop();

  if (!started_fallback) {
    std::cerr
        << "expected fallback pipeline to start after Open Audio failure\n";
    return false;
  }

  if (creates_after_cooldown_window != 1) {
    std::cerr << "Open Audio cooldown did not prevent restart churn; creates="
              << creates_after_cooldown_window << "\n";
    return false;
  }

  if (status.effects_backend_active != "passthrough") {
    std::cerr
        << "expected passthrough backend during Open Audio cooldown; got '"
        << status.effects_backend_active << "'\n";
    return false;
  }

  return true;
}

bool TestForcedMaxineMicrophoneFailureFallsBackToPassthrough() {
  EnvGuard afx_env("STUDIOCAST_AFX_SDK_ROOT",
                   "/tmp/studiocast-definitely-missing-afx-sdk");

  std::atomic<int> pipeline_creates{0};
  std::atomic<bool> mic_consumer_present{true};

  VirtualAudioServiceHooks hooks;
  HookMicrophoneConsumerFlag(&hooks, &mic_consumer_present);
  hooks.probe_microphone_backend_availability =
      [](const VirtualAudioServiceConfig &) {
        AudioBackendAvailability avail;
        avail.maxine_ok = true;
        avail.open_source_ok = false;
        avail.open_source_reason = "synthetic open audio unavailable";
        return avail;
      };
  hooks.create_pipeline =
      [&](AudioProcessor *) -> std::unique_ptr<AudioPipelineRunner> {
    pipeline_creates.fetch_add(1, std::memory_order_relaxed);
    return std::make_unique<FixedStatsPipeline>(true, "");
  };
  hooks.sleep_for = [](std::chrono::milliseconds) {
    std::this_thread::sleep_for(1ms);
  };

  VirtualAudioService service(std::move(hooks));
  VirtualAudioServiceConfig cfg;
  cfg.enabled = true;
  cfg.create_virtual_mic = false;
  cfg.create_virtual_speakers = false;
  cfg.effects.engine =
      studiocast::audio::effects::AudioEffectsEnginePreference::kMaxine;
  cfg.effects.microphone.noise_removal_enabled = true;
  cfg.poll_ms = 1;
  cfg.start_retry_ms = 500;

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  const bool started_fallback = WaitUntil(
      [&] {
        const auto status = service.Status();
        return pipeline_creates.load(std::memory_order_relaxed) >= 1 &&
               status.pipeline_running && status.pipeline_state == "running" &&
               status.effects_backend_active == "passthrough" &&
               status.effects_note.find("using pass-through") !=
                   std::string::npos;
      },
      300ms);
  std::this_thread::sleep_for(75ms);
  const int creates_after_settle =
      pipeline_creates.load(std::memory_order_relaxed);
  const auto status = service.Status();
  service.Stop();

  if (!started_fallback) {
    std::cerr << "forced Maxine microphone failure did not fall back to "
                 "pass-through; creates="
              << pipeline_creates.load() << " state='" << status.pipeline_state
              << "' backend='" << status.effects_backend_active << "' note='"
              << status.effects_note << "' error='" << status.last_error
              << "'\n";
    return false;
  }

  if (creates_after_settle != 1) {
    std::cerr << "forced Maxine microphone fallback churned restarts; creates="
              << creates_after_settle << "\n";
    return false;
  }

  return true;
}

bool TestMicrophoneAvailabilityCacheIgnoresSpeakerOnlyChanges() {
  std::atomic<int> mic_probes{0};
  std::atomic<bool> mic_consumer_present{true};

  VirtualAudioServiceHooks hooks;
  HookMicrophoneConsumerFlag(&hooks, &mic_consumer_present);
  hooks.probe_microphone_backend_availability =
      [&](const VirtualAudioServiceConfig &) {
        mic_probes.fetch_add(1, std::memory_order_relaxed);
        AudioBackendAvailability avail;
        avail.maxine_reason = "synthetic maxine unavailable";
        avail.open_source_reason = "synthetic open audio unavailable";
        return avail;
      };
  hooks.create_pipeline =
      [](AudioProcessor *) -> std::unique_ptr<AudioPipelineRunner> {
    return std::make_unique<FixedStatsPipeline>(true, "");
  };
  hooks.sleep_for = [](std::chrono::milliseconds) {
    std::this_thread::sleep_for(1ms);
  };

  VirtualAudioService service(std::move(hooks));
  VirtualAudioServiceConfig cfg;
  cfg.enabled = true;
  cfg.create_virtual_mic = false;
  cfg.create_virtual_speakers = false;
  cfg.effects.microphone.noise_removal_enabled = true;
  cfg.poll_ms = 1;

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  if (!WaitUntil([&] { return mic_probes.load() >= 1; }, 250ms)) {
    std::cerr << "microphone availability was not probed\n";
    service.Stop();
    return false;
  }

  const int probes_before = mic_probes.load();
  cfg.effects.speaker.noise_removal_enabled = true;
  cfg.effects.speaker.strength = 73;
  service.UpdateConfig(cfg);
  std::this_thread::sleep_for(90ms);
  const int probes_after = mic_probes.load();
  service.Stop();

  if (probes_after != probes_before) {
    std::cerr << "speaker-only effects change invalidated microphone "
                 "availability cache; before="
              << probes_before << " after=" << probes_after << "\n";
    return false;
  }

  return true;
}

bool TestSpeakerAvailabilityCacheIgnoresMicrophoneOnlyChanges() {
  std::atomic<int> speaker_probes{0};
  std::atomic<bool> speaker_consumer_present{true};

  VirtualAudioServiceHooks hooks;
  HookSpeakerConsumerFlag(&hooks, &speaker_consumer_present);
  hooks.create_virtual_speaker = [](std::string *error) {
    if (error)
      error->clear();
    return true;
  };
  hooks.probe_speaker_backend_availability =
      [&](const VirtualAudioServiceConfig &) {
        speaker_probes.fetch_add(1, std::memory_order_relaxed);
        AudioBackendAvailability avail;
        avail.maxine_reason = "synthetic maxine unavailable";
        avail.open_source_reason = "synthetic open audio unavailable";
        return avail;
      };
  hooks.create_pipeline =
      [](AudioProcessor *) -> std::unique_ptr<AudioPipelineRunner> {
    return std::make_unique<FixedStatsPipeline>(true, "");
  };
  hooks.sleep_for = [](std::chrono::milliseconds) {
    std::this_thread::sleep_for(1ms);
  };

  VirtualAudioService service(std::move(hooks));
  VirtualAudioServiceConfig cfg;
  cfg.enabled = false;
  cfg.create_virtual_mic = false;
  cfg.create_virtual_speakers = true;
  cfg.speakers_enabled = true;
  cfg.speaker_target_sink = "physical_test_sink";
  cfg.effects.speaker.noise_removal_enabled = true;
  cfg.poll_ms = 1;

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  if (!WaitUntil([&] { return speaker_probes.load() >= 1; }, 250ms)) {
    std::cerr << "speaker availability was not probed\n";
    service.Stop();
    return false;
  }

  const int probes_before = speaker_probes.load();
  cfg.effects.microphone.noise_removal_enabled = true;
  cfg.effects.microphone.strength = 82;
  service.UpdateConfig(cfg);
  std::this_thread::sleep_for(90ms);
  const int probes_after = speaker_probes.load();
  service.Stop();

  if (probes_after != probes_before) {
    std::cerr << "microphone-only effects change invalidated speaker "
                 "availability cache; before="
              << probes_before << " after=" << probes_after << "\n";
    return false;
  }

  return true;
}

bool TestMicrophoneDeadWorkerBacksOffBeforeRestart() {
  std::atomic<int> pipeline_creates{0};
  std::atomic<int> pipeline_stops{0};
  std::atomic<bool> mic_consumer_present{true};

  VirtualAudioServiceHooks hooks;
  HookMicrophoneConsumerFlag(&hooks, &mic_consumer_present);
  hooks.create_pipeline =
      [&](AudioProcessor *) -> std::unique_ptr<AudioPipelineRunner> {
    pipeline_creates.fetch_add(1, std::memory_order_relaxed);
    return std::make_unique<DeadPipeline>(&pipeline_stops);
  };
  hooks.sleep_for = [](std::chrono::milliseconds) {
    std::this_thread::sleep_for(1ms);
  };

  VirtualAudioService service(std::move(hooks));
  VirtualAudioServiceConfig cfg;
  cfg.enabled = true;
  cfg.create_virtual_mic = false;
  cfg.create_virtual_speakers = false;
  cfg.poll_ms = 1;
  cfg.start_retry_ms = 80;

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  if (!WaitUntil([&] { return pipeline_creates.load() >= 1; }, 250ms)) {
    std::cerr << "microphone pipeline was not started\n";
    service.Stop();
    return false;
  }

  std::this_thread::sleep_for(40ms);
  const int creates_during_backoff = pipeline_creates.load();
  const bool restarted =
      WaitUntil([&] { return pipeline_creates.load() >= 2; }, 300ms);
  service.Stop();

  if (creates_during_backoff != 1) {
    std::cerr << "dead microphone worker restarted before retry backoff; "
              << "creates=" << creates_during_backoff << "\n";
    return false;
  }

  if (!restarted) {
    std::cerr << "dead microphone worker did not restart after retry backoff; "
              << "creates=" << pipeline_creates.load() << "\n";
    return false;
  }

  return true;
}

bool TestSpeakerDeadWorkerBacksOffAndClearsRoute() {
  std::atomic<int> pipeline_creates{0};
  std::atomic<int> pipeline_stops{0};
  std::atomic<bool> speaker_consumer_present{true};

  VirtualAudioServiceHooks hooks;
  HookSpeakerConsumerFlag(&hooks, &speaker_consumer_present);
  hooks.create_virtual_speaker = [](std::string *error) {
    if (error)
      error->clear();
    return true;
  };
  hooks.probe_speaker_backend_availability =
      [](const VirtualAudioServiceConfig &) {
        AudioBackendAvailability avail;
        avail.maxine_reason = "synthetic maxine unavailable";
        avail.open_source_reason = "synthetic open audio unavailable";
        return avail;
      };
  hooks.create_pipeline =
      [&](AudioProcessor *) -> std::unique_ptr<AudioPipelineRunner> {
    pipeline_creates.fetch_add(1, std::memory_order_relaxed);
    return std::make_unique<DeadPipeline>(&pipeline_stops);
  };
  hooks.sleep_for = [](std::chrono::milliseconds) {
    std::this_thread::sleep_for(1ms);
  };

  VirtualAudioService service(std::move(hooks));
  VirtualAudioServiceConfig cfg;
  cfg.enabled = false;
  cfg.create_virtual_mic = false;
  cfg.create_virtual_speakers = true;
  cfg.speakers_enabled = true;
  cfg.speaker_target_sink = "physical_test_sink";
  cfg.effects.speaker.noise_removal_enabled = true;
  cfg.poll_ms = 1;
  cfg.start_retry_ms = 80;

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  if (!WaitUntil([&] { return pipeline_creates.load() >= 1; }, 250ms)) {
    std::cerr << "speaker pipeline was not started\n";
    service.Stop();
    return false;
  }

  const bool saw_inactive = WaitUntil(
      [&] {
        const auto status = service.Status();
        return !status.speakers_routing_active &&
               status.speaker_target_sink_active.empty() &&
               status.speakers_pipeline_last_error.find(
                   "Speaker audio pipeline stopped") != std::string::npos;
      },
      250ms);
  std::this_thread::sleep_for(40ms);
  const int creates_during_backoff = pipeline_creates.load();
  const bool restarted =
      WaitUntil([&] { return pipeline_creates.load() >= 2; }, 300ms);
  service.Stop();

  if (!saw_inactive) {
    std::cerr << "speaker worker death did not clear active route status\n";
    return false;
  }

  if (creates_during_backoff != 1) {
    std::cerr << "dead speaker worker restarted before retry backoff; creates="
              << creates_during_backoff << "\n";
    return false;
  }

  if (!restarted) {
    std::cerr << "dead speaker worker did not restart after retry backoff; "
              << "creates=" << pipeline_creates.load() << "\n";
    return false;
  }

  return true;
}

bool TestSpeakerPipelineStatsClearWhenProcessingDisabled() {
  std::atomic<bool> speaker_consumer_present{true};
  AudioPipelineStats synthetic_stats;
  synthetic_stats.running = true;
  synthetic_stats.frames_processed = 123;
  synthetic_stats.process_time_us_sum = 456;
  synthetic_stats.process_time_us_max = 78;
  synthetic_stats.process_time_us_last = 9;
  synthetic_stats.process_overruns = 2;
  synthetic_stats.pulse_capture_latency_us_last = 1000;
  synthetic_stats.pulse_playback_latency_us_last = 2000;
  synthetic_stats.pulse_latency_us_max = 3000;
  synthetic_stats.resync_events = 4;

  VirtualAudioServiceHooks hooks;
  HookSpeakerConsumerFlag(&hooks, &speaker_consumer_present);
  hooks.create_virtual_speaker = [](std::string *error) {
    if (error)
      error->clear();
    return true;
  };
  hooks.probe_speaker_backend_availability =
      [](const VirtualAudioServiceConfig &) {
        AudioBackendAvailability avail;
        avail.maxine_reason = "synthetic maxine unavailable";
        avail.open_source_reason = "synthetic open audio unavailable";
        return avail;
      };
  hooks.create_pipeline =
      [synthetic_stats](
          AudioProcessor *) -> std::unique_ptr<AudioPipelineRunner> {
    return std::make_unique<FixedStatsPipeline>(synthetic_stats);
  };
  hooks.sleep_for = [](std::chrono::milliseconds) {
    std::this_thread::sleep_for(1ms);
  };

  VirtualAudioService service(std::move(hooks));
  VirtualAudioServiceConfig cfg;
  cfg.enabled = false;
  cfg.create_virtual_mic = false;
  cfg.create_virtual_speakers = true;
  cfg.speakers_enabled = true;
  cfg.speaker_target_sink = "physical_test_sink";
  cfg.effects.speaker.noise_removal_enabled = true;
  cfg.poll_ms = 1;

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  const bool saw_stats = WaitUntil(
      [&] {
        const auto status = service.Status();
        return status.speakers_pipeline_frames_processed == 123 &&
               status.speakers_pipeline_pulse_latency_us_max == 3000;
      },
      250ms);
  if (!saw_stats) {
    const auto status = service.Status();
    std::cerr << "speaker pipeline stats were not published; frames="
              << status.speakers_pipeline_frames_processed << " max_latency="
              << status.speakers_pipeline_pulse_latency_us_max << "\n";
    service.Stop();
    return false;
  }

  cfg.speakers_enabled = false;
  service.UpdateConfig(cfg);
  const bool cleared = WaitUntil(
      [&] {
        const auto status = service.Status();
        return status.speakers_route_mode == "off" &&
               !status.speakers_pipeline_running &&
               status.speakers_pipeline_frames_processed == 0 &&
               status.speakers_pipeline_process_time_us_sum == 0 &&
               status.speakers_pipeline_pulse_capture_latency_us_last == 0 &&
               status.speakers_pipeline_pulse_playback_latency_us_last == 0 &&
               status.speakers_pipeline_pulse_latency_us_max == 0 &&
               status.speakers_pipeline_resync_events == 0;
      },
      250ms);
  const auto status = service.Status();
  service.Stop();

  if (!cleared) {
    std::cerr << "speaker pipeline stats stayed stale after disabling "
                 "processing; route='"
              << status.speakers_route_mode
              << "' frames=" << status.speakers_pipeline_frames_processed
              << " max_latency="
              << status.speakers_pipeline_pulse_latency_us_max << "\n";
    return false;
  }

  return true;
}

bool TestSpeakerLoopbackRestartFailureClearsRouteState() {
  std::atomic<int> loopback_start_calls{0};

  VirtualAudioServiceHooks hooks;
  hooks.create_virtual_speaker = [](std::string *error) {
    if (error)
      error->clear();
    return true;
  };
  hooks.start_speaker_loopback = [&](const std::string &, int,
                                     std::string *error) {
    const int call =
        loopback_start_calls.fetch_add(1, std::memory_order_relaxed);
    if (call == 0) {
      if (error)
        error->clear();
      return true;
    }
    if (error)
      *error = "synthetic loopback load failure";
    return false;
  };
  hooks.sleep_for = [](std::chrono::milliseconds) {
    std::this_thread::sleep_for(1ms);
  };

  VirtualAudioService service(std::move(hooks));
  VirtualAudioServiceConfig cfg;
  cfg.enabled = false;
  cfg.create_virtual_mic = false;
  cfg.create_virtual_speakers = true;
  cfg.speakers_enabled = true;
  cfg.speaker_target_sink = "physical_test_sink";
  cfg.poll_ms = 1;
  cfg.start_retry_ms = 200;

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  const bool first_route_active = WaitUntil(
      [&] {
        const auto status = service.Status();
        return loopback_start_calls.load(std::memory_order_relaxed) >= 1 &&
               status.speakers_routing_active &&
               status.speakers_route_mode == "loopback";
      },
      250ms);
  if (!first_route_active) {
    const auto status = service.Status();
    std::cerr << "speaker loopback route did not become active; starts="
              << loopback_start_calls.load()
              << " active=" << status.speakers_routing_active << " route='"
              << status.speakers_route_mode << "' error='"
              << status.speakers_last_error << "'\n";
    service.Stop();
    return false;
  }

  cfg.speaker_target_sink = "other_physical_test_sink";
  service.UpdateConfig(cfg);

  const bool cleared_after_failure = WaitUntil(
      [&] {
        const auto status = service.Status();
        return loopback_start_calls.load(std::memory_order_relaxed) >= 2 &&
               !status.speakers_routing_active &&
               status.speaker_target_sink_active.empty() &&
               status.speakers_last_error.find(
                   "synthetic loopback load failure") != std::string::npos;
      },
      250ms);
  const int starts_after_failure =
      loopback_start_calls.load(std::memory_order_relaxed);
  std::this_thread::sleep_for(50ms);
  const int starts_during_backoff =
      loopback_start_calls.load(std::memory_order_relaxed);
  const auto status = service.Status();
  service.Stop();

  if (!cleared_after_failure) {
    std::cerr << "speaker loopback failure left route active; starts="
              << loopback_start_calls.load()
              << " active=" << status.speakers_routing_active << " target='"
              << status.speaker_target_sink_active << "' error='"
              << status.speakers_last_error << "'\n";
    return false;
  }

  if (starts_during_backoff != starts_after_failure) {
    std::cerr << "speaker loopback failure retried before backoff elapsed; "
              << "after_failure=" << starts_after_failure
              << " during_backoff=" << starts_during_backoff << "\n";
    return false;
  }

  return true;
}

bool TestSpeakerLoopbackRealHelperStopFailureKeepsOldRouteActive() {
  ScopedXdgStateHome state_home;
  VirtualSpeakerState initial;
  initial.null_sink_module_id = 10;
  initial.loopback_module_id = 42;
  initial.loopback_target_sink_name = "old_physical_sink";
  std::string err;
  if (!studiocast::audio::SaveVirtualSpeakerState(initial, &err)) {
    std::cerr << "failed to seed virtual speaker state: " << err << "\n";
    return false;
  }

  std::atomic<int> unload_calls{0};
  std::vector<std::string> commands;
  ScopedPactlExecHook hook([&](const std::string &command) {
    commands.push_back(command);
    if (command == "pactl --version 2>&1")
      return ExecResult(0, "pactl 16.1\n");
    if (command == "pactl list short modules 2>&1") {
      return ExecResult(
          0, "10\tmodule-null-sink\tsink_name=studiocast_speakers\n"
             "42\tmodule-loopback\tsource=studiocast_speakers.monitor "
             "sink=old_physical_sink\n");
    }
    if (command.find("pactl update-sink-proplist 'studiocast_speakers'") == 0)
      return ExecResult(0);
    if (command == "pactl unload-module 42 2>&1") {
      unload_calls.fetch_add(1, std::memory_order_relaxed);
      return ExecResult(1, "Failure: synthetic old loopback unload failure\n");
    }
    return ExecResult(99, "unexpected command: " + command);
  });

  VirtualAudioServiceHooks hooks;
  hooks.sleep_for = [](std::chrono::milliseconds) {
    std::this_thread::sleep_for(1ms);
  };

  VirtualAudioService service(std::move(hooks));
  VirtualAudioServiceConfig cfg;
  cfg.enabled = false;
  cfg.create_virtual_mic = false;
  cfg.create_virtual_speakers = true;
  cfg.speakers_enabled = true;
  cfg.speaker_target_sink = "new_physical_sink";
  cfg.poll_ms = 1;
  cfg.start_retry_ms = 200;

  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  const bool old_route_preserved = WaitUntil(
      [&] {
        const auto status = service.Status();
        return unload_calls.load(std::memory_order_relaxed) >= 1 &&
               status.speakers_route_mode == "loopback" &&
               status.speakers_routing_active &&
               status.speaker_target_sink_active == "old_physical_sink" &&
               status.speakers_last_error.find(
                   "synthetic old loopback unload failure") !=
                   std::string::npos;
      },
      250ms);
  const int unloads_after_failure =
      unload_calls.load(std::memory_order_relaxed);
  std::this_thread::sleep_for(50ms);
  const int unloads_during_backoff =
      unload_calls.load(std::memory_order_relaxed);
  const auto status = service.Status();
  service.Stop();

  if (!old_route_preserved) {
    std::cerr << "real helper stop failure did not preserve old loopback; "
              << "unloads=" << unload_calls.load() << " route='"
              << status.speakers_route_mode
              << "' active=" << status.speakers_routing_active << " target='"
              << status.speaker_target_sink_active << "' error='"
              << status.speakers_last_error << "'\n";
    for (const auto &command : commands)
      std::cerr << "  " << command << "\n";
    return false;
  }

  if (unloads_during_backoff != unloads_after_failure) {
    std::cerr << "real helper stop failure retried before backoff elapsed; "
              << "after_failure=" << unloads_after_failure
              << " during_backoff=" << unloads_during_backoff << "\n";
    return false;
  }

  if (CommandWasRun(commands, "pactl load-module 'module-loopback'")) {
    std::cerr << "new speaker loopback loaded after old route stop failed\n";
    for (const auto &command : commands)
      std::cerr << "  " << command << "\n";
    return false;
  }

  return true;
}

bool TestVirtualSpeakerDestroyFailureBacksOffAndKeepsPresent() {
  std::atomic<int> create_calls{0};
  std::atomic<int> destroy_calls{0};

  VirtualAudioServiceHooks hooks;
  hooks.create_virtual_speaker = [&](std::string *error) {
    create_calls.fetch_add(1, std::memory_order_relaxed);
    if (error)
      error->clear();
    return true;
  };
  hooks.destroy_virtual_speaker = [&](std::string *error) {
    destroy_calls.fetch_add(1, std::memory_order_relaxed);
    if (error)
      *error = "synthetic virtual speaker destroy failure";
    return false;
  };
  hooks.sleep_for = [](std::chrono::milliseconds) {
    std::this_thread::sleep_for(1ms);
  };

  VirtualAudioService service(std::move(hooks));
  VirtualAudioServiceConfig cfg;
  cfg.enabled = false;
  cfg.create_virtual_mic = false;
  cfg.create_virtual_speakers = true;
  cfg.speakers_enabled = false;
  cfg.poll_ms = 1;
  cfg.start_retry_ms = 200;

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  if (!WaitUntil(
          [&] {
            const auto status = service.Status();
            return create_calls.load(std::memory_order_relaxed) >= 1 &&
                   status.speakers_present;
          },
          250ms)) {
    std::cerr << "virtual speakers did not become present before destroy\n";
    service.Stop();
    return false;
  }

  cfg.create_virtual_speakers = false;
  service.UpdateConfig(cfg);

  const bool destroy_failed = WaitUntil(
      [&] {
        const auto status = service.Status();
        return destroy_calls.load(std::memory_order_relaxed) >= 1 &&
               status.speakers_present &&
               status.speakers_last_error.find(
                   "synthetic virtual speaker destroy failure") !=
                   std::string::npos;
      },
      250ms);
  const int destroys_after_failure =
      destroy_calls.load(std::memory_order_relaxed);
  std::this_thread::sleep_for(50ms);
  const int destroys_during_backoff =
      destroy_calls.load(std::memory_order_relaxed);
  const auto status = service.Status();
  service.Stop();

  if (!destroy_failed) {
    std::cerr << "virtual speaker destroy failure did not keep status present; "
              << "destroys=" << destroy_calls.load()
              << " present=" << status.speakers_present << " error='"
              << status.speakers_last_error << "'\n";
    return false;
  }

  if (destroys_during_backoff != destroys_after_failure) {
    std::cerr << "virtual speaker destroy retried before backoff elapsed; "
              << "after_failure=" << destroys_after_failure
              << " during_backoff=" << destroys_during_backoff << "\n";
    return false;
  }

  return true;
}

bool TestSpeakerLoopbackStopFailureBlocksPipelineStart() {
  std::atomic<int> loopback_start_calls{0};
  std::atomic<int> loopback_stop_calls{0};
  std::atomic<int> pipeline_creates{0};
  std::atomic<bool> speaker_consumer_present{true};

  VirtualAudioServiceHooks hooks;
  HookSpeakerConsumerFlag(&hooks, &speaker_consumer_present);
  hooks.create_virtual_speaker = [](std::string *error) {
    if (error)
      error->clear();
    return true;
  };
  hooks.start_speaker_loopback = [&](const std::string &, int,
                                     std::string *error) {
    loopback_start_calls.fetch_add(1, std::memory_order_relaxed);
    if (error)
      error->clear();
    return true;
  };
  hooks.stop_speaker_loopback = [&](std::string *error) {
    loopback_stop_calls.fetch_add(1, std::memory_order_relaxed);
    if (error)
      *error = "synthetic loopback stop failure";
    return false;
  };
  hooks.probe_speaker_backend_availability =
      [](const VirtualAudioServiceConfig &) {
        AudioBackendAvailability avail;
        avail.maxine_reason = "synthetic maxine unavailable";
        avail.open_source_reason = "synthetic open audio unavailable";
        return avail;
      };
  hooks.create_pipeline =
      [&](AudioProcessor *) -> std::unique_ptr<AudioPipelineRunner> {
    pipeline_creates.fetch_add(1, std::memory_order_relaxed);
    return std::make_unique<FixedStatsPipeline>(true, "");
  };
  hooks.sleep_for = [](std::chrono::milliseconds) {
    std::this_thread::sleep_for(1ms);
  };

  VirtualAudioService service(std::move(hooks));
  VirtualAudioServiceConfig cfg;
  cfg.enabled = false;
  cfg.create_virtual_mic = false;
  cfg.create_virtual_speakers = true;
  cfg.speakers_enabled = true;
  cfg.speaker_target_sink = "physical_test_sink";
  cfg.poll_ms = 1;

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  if (!WaitUntil(
          [&] {
            const auto status = service.Status();
            return loopback_start_calls.load(std::memory_order_relaxed) >= 1 &&
                   status.speakers_route_mode == "loopback" &&
                   status.speakers_routing_active;
          },
          250ms)) {
    const auto status = service.Status();
    std::cerr << "speaker loopback did not become active before processing "
                 "transition; starts="
              << loopback_start_calls.load() << " route='"
              << status.speakers_route_mode
              << "' active=" << status.speakers_routing_active << "\n";
    service.Stop();
    return false;
  }

  cfg.effects.speaker.noise_removal_enabled = true;
  service.UpdateConfig(cfg);

  const bool stop_failed = WaitUntil(
      [&] {
        const auto status = service.Status();
        return loopback_stop_calls.load(std::memory_order_relaxed) >= 1 &&
               status.speakers_route_mode == "loopback" &&
               status.speakers_routing_active &&
               !status.speakers_pipeline_running &&
               !status.speakers_pipeline_starting &&
               status.speakers_last_error.find("synthetic loopback stop "
                                               "failure") != std::string::npos;
      },
      250ms);
  const int stops_after_failure =
      loopback_stop_calls.load(std::memory_order_relaxed);
  std::this_thread::sleep_for(30ms);
  const int stops_during_backoff =
      loopback_stop_calls.load(std::memory_order_relaxed);
  const int creates_after_stop_failure =
      pipeline_creates.load(std::memory_order_relaxed);
  const auto status = service.Status();
  service.Stop();

  if (!stop_failed) {
    std::cerr << "speaker stop failure did not keep loopback route active; "
              << "stops=" << loopback_stop_calls.load() << " route='"
              << status.speakers_route_mode
              << "' active=" << status.speakers_routing_active
              << " pipeline_running=" << status.speakers_pipeline_running
              << " pipeline_starting=" << status.speakers_pipeline_starting
              << " error='" << status.speakers_last_error << "'\n";
    return false;
  }

  if (stops_during_backoff != stops_after_failure) {
    std::cerr << "speaker loopback stop retried before backoff elapsed; "
              << "after_failure=" << stops_after_failure
              << " during_backoff=" << stops_during_backoff << "\n";
    return false;
  }

  if (creates_after_stop_failure != 0) {
    std::cerr << "speaker pipeline started while loopback stop was failing; "
              << "creates=" << creates_after_stop_failure << "\n";
    return false;
  }

  return true;
}

bool TestSpeakerLoopbackStopFailurePreventsDestroyAndKeepsRoute() {
  std::atomic<int> loopback_start_calls{0};
  std::atomic<int> loopback_stop_calls{0};
  std::atomic<int> destroy_calls{0};

  VirtualAudioServiceHooks hooks;
  hooks.create_virtual_speaker = [](std::string *error) {
    if (error)
      error->clear();
    return true;
  };
  hooks.start_speaker_loopback = [&](const std::string &, int,
                                     std::string *error) {
    loopback_start_calls.fetch_add(1, std::memory_order_relaxed);
    if (error)
      error->clear();
    return true;
  };
  hooks.stop_speaker_loopback = [&](std::string *error) {
    loopback_stop_calls.fetch_add(1, std::memory_order_relaxed);
    if (error)
      *error = "synthetic loopback stop failure";
    return false;
  };
  hooks.destroy_virtual_speaker = [&](std::string *error) {
    destroy_calls.fetch_add(1, std::memory_order_relaxed);
    if (error)
      error->clear();
    return true;
  };
  hooks.sleep_for = [](std::chrono::milliseconds) {
    std::this_thread::sleep_for(1ms);
  };

  VirtualAudioService service(std::move(hooks));
  VirtualAudioServiceConfig cfg;
  cfg.enabled = false;
  cfg.create_virtual_mic = false;
  cfg.create_virtual_speakers = true;
  cfg.speakers_enabled = true;
  cfg.speaker_target_sink = "physical_test_sink";
  cfg.poll_ms = 1;

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  if (!WaitUntil(
          [&] {
            const auto status = service.Status();
            return loopback_start_calls.load(std::memory_order_relaxed) >= 1 &&
                   status.speakers_route_mode == "loopback" &&
                   status.speakers_routing_active;
          },
          250ms)) {
    std::cerr << "speaker loopback did not become active before disable\n";
    service.Stop();
    return false;
  }

  cfg.speakers_enabled = false;
  cfg.create_virtual_speakers = false;
  service.UpdateConfig(cfg);

  const bool stop_failed = WaitUntil(
      [&] {
        const auto status = service.Status();
        return loopback_stop_calls.load(std::memory_order_relaxed) >= 1 &&
               status.speakers_route_mode == "loopback" &&
               status.speakers_routing_active && status.speakers_present &&
               status.speakers_last_error.find("synthetic loopback stop "
                                               "failure") != std::string::npos;
      },
      250ms);
  const int stops_after_failure =
      loopback_stop_calls.load(std::memory_order_relaxed);
  std::this_thread::sleep_for(30ms);
  const int stops_during_backoff =
      loopback_stop_calls.load(std::memory_order_relaxed);
  const int destroys_after_stop_failure =
      destroy_calls.load(std::memory_order_relaxed);
  const auto status = service.Status();
  service.Stop();

  if (!stop_failed) {
    std::cerr
        << "speaker disable did not preserve failed loopback route; stops="
        << loopback_stop_calls.load() << " route='"
        << status.speakers_route_mode
        << "' active=" << status.speakers_routing_active
        << " present=" << status.speakers_present << " error='"
        << status.speakers_last_error << "'\n";
    return false;
  }

  if (stops_during_backoff != stops_after_failure) {
    std::cerr << "speaker loopback stop during disable retried before backoff "
                 "elapsed; after_failure="
              << stops_after_failure
              << " during_backoff=" << stops_during_backoff << "\n";
    return false;
  }

  if (destroys_after_stop_failure != 0) {
    std::cerr << "virtual speakers were destroyed while loopback stop failed; "
              << "destroys=" << destroys_after_stop_failure << "\n";
    return false;
  }

  return true;
}

bool TestStopInterruptsOpenAfterEarlyStopReset() {
  auto state = std::make_shared<ResettingOpenIoState>();
  CopyProcessor processor;

  AudioPipelineHooks hooks;
  hooks.create_io = [state] {
    return std::make_unique<ResettingOpenIo>(state, 25ms, 200ms);
  };

  AudioPipeline pipeline(&processor, std::move(hooks));
  AudioPipelineConfig cfg;

  std::string err;
  auto start_future = std::async(std::launch::async, [&] {
    return pipeline.Start(cfg, &err);
  });

  if (!WaitUntil(
          [&] {
            std::lock_guard<std::mutex> lock(state->mu);
            return state->open_started;
          },
          250ms)) {
    std::cerr << "pipeline.Start did not enter Open()\n";
    pipeline.Stop();
    (void)start_future.get();
    return false;
  }

  auto stop_future = std::async(std::launch::async, [&] {
    pipeline.Stop();
    return true;
  });
  const bool stop_ready =
      (stop_future.wait_for(100ms) == std::future_status::ready);
  (void)stop_future.get();
  const bool start_ready =
      (start_future.wait_for(100ms) == std::future_status::ready);
  const bool start_ok = start_ready ? start_future.get() : true;

  int request_stop_calls = 0;
  {
    std::lock_guard<std::mutex> lock(state->mu);
    request_stop_calls = state->request_stop_calls;
  }

  if (!stop_ready) {
    std::cerr << "Stop() stayed blocked while Open() reset an early stop\n";
    return false;
  }

  if (!start_ready || start_ok) {
    std::cerr << "Start() did not return false after Stop() interrupted Open(); "
              << "ready=" << start_ready << " ok=" << start_ok
              << " err='" << err << "'\n";
    return false;
  }

  if (request_stop_calls == 0) {
    std::cerr << "Stop() never signaled Open() to stop\n";
    return false;
  }

  return true;
}

bool TestStopInterruptsBlockedFlush() {
  auto state = std::make_shared<BlockingFlushIoState>();
  CopyProcessor processor;

  AudioPipelineHooks hooks;
  hooks.create_io = [state] {
    return std::make_unique<BlockingFlushIo>(state, 200ms);
  };

  AudioPipeline pipeline(&processor, std::move(hooks));
  AudioPipelineConfig cfg;

  std::string err;
  if (!pipeline.Start(cfg, &err)) {
    std::cerr << "pipeline.Start failed: " << err << "\n";
    return false;
  }

  if (!WaitUntil(
          [&] {
            std::lock_guard<std::mutex> lock(state->mu);
            return state->flush_entered;
          },
          250ms)) {
    std::cerr << "pipeline did not enter blocked flush\n";
    pipeline.Stop();
    return false;
  }

  auto stop_future = std::async(std::launch::async, [&] {
    pipeline.Stop();
    return true;
  });
  const bool stop_ready =
      (stop_future.wait_for(50ms) == std::future_status::ready);
  (void)stop_future.get();

  int request_stop_calls = 0;
  {
    std::lock_guard<std::mutex> lock(state->mu);
    request_stop_calls = state->request_stop_calls;
  }

  if (!stop_ready) {
    std::cerr << "Stop() stayed blocked while Flush() was stuck\n";
    return false;
  }

  if (request_stop_calls == 0) {
    std::cerr << "Stop() never signaled the flush transport to stop\n";
    return false;
  }

  return true;
}

bool TestStartReturnsOpenFailureAndCanRetry() {
  std::atomic<int> open_calls{0};
  CopyProcessor processor;

  AudioPipelineHooks hooks;
  hooks.create_io = [&] { return std::make_unique<OpenFailIo>(&open_calls); };

  AudioPipeline pipeline(&processor, std::move(hooks));
  AudioPipelineConfig cfg;

  std::string err;
  if (pipeline.Start(cfg, &err)) {
    std::cerr << "first pipeline.Start succeeded despite open failure\n";
    return false;
  }
  if (err.find("synthetic open failure") == std::string::npos) {
    std::cerr << "first Start() did not return open error; err='" << err
              << "'\n";
    return false;
  }

  if (open_calls.load(std::memory_order_relaxed) != 1 ||
      pipeline.GetStats().running) {
    std::cerr << "first failed start left pipeline running; opens="
              << open_calls.load()
              << " running=" << pipeline.GetStats().running << "\n";
    return false;
  }

  err.clear();
  if (pipeline.Start(cfg, &err)) {
    std::cerr << "second pipeline.Start succeeded despite open failure\n";
    return false;
  }
  if (err.find("synthetic open failure") == std::string::npos) {
    std::cerr << "second Start() did not return open error; err='" << err
              << "'\n";
    return false;
  }

  if (open_calls.load(std::memory_order_relaxed) != 2 ||
      pipeline.GetStats().running) {
    std::cerr << "second failed start left pipeline running; opens="
              << open_calls.load()
              << " running=" << pipeline.GetStats().running << "\n";
    return false;
  }

  pipeline.Stop();
  return true;
}

bool TestPipelineSurfacesCaptureDisconnectError() {
  std::atomic<int> read_calls{0};
  CopyProcessor processor;

  AudioPipelineHooks hooks;
  hooks.create_io = [&] { return std::make_unique<ReadFailIo>(&read_calls); };

  AudioPipeline pipeline(&processor, std::move(hooks));
  AudioPipelineConfig cfg;

  std::string err;
  if (!pipeline.Start(cfg, &err)) {
    std::cerr << "pipeline.Start failed: " << err << "\n";
    return false;
  }

  const bool saw_disconnect = WaitUntil(
      [&] {
        const auto stats = pipeline.GetStats();
        return read_calls.load(std::memory_order_relaxed) > 0 &&
               !stats.running &&
               stats.last_error.find("capture stream disconnected") !=
                   std::string::npos;
      },
      250ms);
  const auto stats = pipeline.GetStats();
  pipeline.Stop();

  if (!saw_disconnect) {
    std::cerr << "expected capture disconnect error; reads="
              << read_calls.load() << " running=" << stats.running << " error='"
              << stats.last_error << "'\n";
    return false;
  }

  return true;
}

bool TestLatencyGuardSumsCaptureAndPlaybackBeforeResync() {
  auto state = std::make_shared<LatencyIoState>();
  std::atomic<int> reset_calls{0};
  CountingCopyProcessor processor(&reset_calls);

  AudioPipelineHooks hooks;
  hooks.create_io = [state] { return std::make_unique<HighLatencyIo>(state); };

  AudioPipeline pipeline(&processor, std::move(hooks));
  AudioPipelineConfig cfg;

  std::string err;
  if (!pipeline.Start(cfg, &err)) {
    std::cerr << "pipeline.Start failed: " << err << "\n";
    return false;
  }

  const bool resynced =
      WaitUntil([&] { return pipeline.GetStats().resync_events >= 1; }, 1700ms);
  const auto stats = pipeline.GetStats();
  pipeline.Stop();

  if (!resynced) {
    std::cerr << "expected latency guard to resync when capture+playback "
                 "latency exceeded threshold; max_us="
              << stats.pulse_latency_us_max
              << " capture_queries=" << state->capture_latency_queries.load()
              << " playback_queries=" << state->playback_latency_queries.load()
              << "\n";
    return false;
  }

  if (stats.pulse_capture_latency_us_last != 80000 ||
      stats.pulse_playback_latency_us_last != 80000 ||
      stats.pulse_latency_us_max < 160000) {
    std::cerr << "latency stats did not account for capture+playback sum; "
              << "capture=" << stats.pulse_capture_latency_us_last
              << " playback=" << stats.pulse_playback_latency_us_last
              << " max=" << stats.pulse_latency_us_max << "\n";
    return false;
  }

  if (reset_calls.load(std::memory_order_relaxed) < 2 ||
      state->flush_calls.load(std::memory_order_relaxed) < 2) {
    std::cerr << "resync did not flush/reset pipeline state; resets="
              << reset_calls.load() << " flushes=" << state->flush_calls.load()
              << "\n";
    return false;
  }

  return true;
}

bool TestLatencyQueryFailureClearsStaleLastValue() {
  auto state = std::make_shared<FlakyLatencyIoState>();
  CopyProcessor processor;

  AudioPipelineHooks hooks;
  hooks.create_io = [state] { return std::make_unique<FlakyLatencyIo>(state); };

  AudioPipeline pipeline(&processor, std::move(hooks));
  AudioPipelineConfig cfg;

  std::string err;
  if (!pipeline.Start(cfg, &err)) {
    std::cerr << "pipeline.Start failed: " << err << "\n";
    return false;
  }

  const bool saw_initial_latency = WaitUntil(
      [&] {
        const auto stats = pipeline.GetStats();
        return stats.pulse_capture_latency_us_last == 12000 &&
               stats.pulse_playback_latency_us_last == 34000 &&
               stats.pulse_latency_us_max >= 46000;
      },
      1300ms);

  const bool cleared_failed_side = WaitUntil(
      [&] {
        const auto stats = pipeline.GetStats();
        return state->capture_latency_queries.load(std::memory_order_relaxed) >=
                   2 &&
               state->playback_latency_queries.load(
                   std::memory_order_relaxed) >= 2 &&
               stats.pulse_capture_latency_us_last == 0 &&
               stats.pulse_playback_latency_us_last == 5000 &&
               stats.pulse_latency_us_max >= 46000;
      },
      1300ms);
  const auto stats = pipeline.GetStats();
  pipeline.Stop();

  if (!saw_initial_latency || !cleared_failed_side) {
    std::cerr << "latency query failure left stale status; initial="
              << saw_initial_latency << " cleared=" << cleared_failed_side
              << " capture_last=" << stats.pulse_capture_latency_us_last
              << " playback_last=" << stats.pulse_playback_latency_us_last
              << " max=" << stats.pulse_latency_us_max
              << " capture_queries=" << state->capture_latency_queries.load()
              << " playback_queries=" << state->playback_latency_queries.load()
              << "\n";
    return false;
  }

  return true;
}

bool TestOfflinePassthroughPipelineAudioQuality() {
  constexpr std::uint32_t kFrameSamples = 480;
  constexpr int kFrameCount = 8;
  constexpr std::uint32_t kChannels = 2;

  auto state = std::make_shared<ScriptedQualityIoState>();
  const std::vector<float> input =
      MakeSyntheticStereoAudio(kFrameSamples, kFrameCount);
  {
    std::lock_guard<std::mutex> lock(state->mu);
    state->input = input;
  }

  studiocast::audio::PassthroughAudioProcessor processor;
  AudioPipelineHooks hooks;
  hooks.create_io = [state] {
    return std::make_unique<ScriptedQualityIo>(state);
  };

  AudioPipeline pipeline(&processor, std::move(hooks));
  AudioPipelineConfig cfg;
  cfg.sample_rate = 48000;
  cfg.frame_samples = kFrameSamples;
  cfg.channels = kChannels;

  std::string err;
  if (!pipeline.Start(cfg, &err)) {
    std::cerr << "pipeline.Start failed: " << err << "\n";
    return false;
  }

  const bool finished = WaitUntil(
      [&] {
        const auto stats = pipeline.GetStats();
        return stats.frames_processed ==
                   static_cast<std::uint64_t>(kFrameCount) &&
               !stats.running;
      },
      500ms);
  const auto stats = pipeline.GetStats();
  pipeline.Stop();

  std::vector<float> output;
  std::uint32_t open_sample_rate = 0;
  std::uint32_t open_channels = 0;
  int read_calls = 0;
  int write_calls = 0;
  int flush_calls = 0;
  {
    std::lock_guard<std::mutex> lock(state->mu);
    output = state->output;
    open_sample_rate = state->open_sample_rate;
    open_channels = state->open_channels;
    read_calls = state->read_calls;
    write_calls = state->write_calls;
    flush_calls = state->flush_calls;
  }

  if (!finished) {
    std::cerr << "offline passthrough pipeline did not finish expected "
                 "buffers; frames="
              << stats.frames_processed << " running=" << stats.running
              << " error='" << stats.last_error << "'\n";
    return false;
  }

  if (open_sample_rate != 48000 || open_channels != kChannels ||
      read_calls != kFrameCount || write_calls != kFrameCount ||
      flush_calls < 2) {
    std::cerr << "offline pipeline format/buffer behavior changed; rate="
              << open_sample_rate << " channels=" << open_channels
              << " reads=" << read_calls << " writes=" << write_calls
              << " flushes=" << flush_calls << "\n";
    return false;
  }

  if (output.size() != input.size()) {
    std::cerr << "offline pipeline changed sample count; input=" << input.size()
              << " output=" << output.size() << "\n";
    return false;
  }

  for (std::size_t i = 0; i < output.size(); ++i) {
    if (!std::isfinite(output[i]) || std::fabs(output[i]) > 1.000001f) {
      std::cerr << "offline pipeline produced invalid sample at " << i << ": "
                << output[i] << "\n";
      return false;
    }
  }

  const std::size_t silence_samples =
      static_cast<std::size_t>(kFrameSamples) * kChannels;
  for (std::size_t i = 0; i < silence_samples; ++i) {
    if (output[i] != 0.0f) {
      std::cerr << "offline pipeline did not preserve leading silence at " << i
                << ": " << output[i] << "\n";
      return false;
    }
  }

  if (output != input) {
    double max_abs_diff = 0.0;
    for (std::size_t i = 0; i < output.size(); ++i) {
      max_abs_diff =
          std::max(max_abs_diff, std::fabs(static_cast<double>(output[i]) -
                                           static_cast<double>(input[i])));
    }
    std::cerr << "passthrough pipeline changed samples; max_abs_diff="
              << max_abs_diff << "\n";
    return false;
  }

  const double input_rms = Rms(input);
  const double output_rms = Rms(output);
  if (std::fabs(input_rms - output_rms) > 1e-9) {
    std::cerr << "passthrough RMS changed; input=" << input_rms
              << " output=" << output_rms << "\n";
    return false;
  }

  return true;
}

bool TestStopInterruptsBlockedCaptureRead() {
  auto state = std::make_shared<BlockingIoState>();
  CopyProcessor processor;

  AudioPipelineHooks hooks;
  hooks.create_io = [state] {
    return std::make_unique<BlockingIo>(state, BlockMode::kRead, 200ms);
  };

  AudioPipeline pipeline(&processor, std::move(hooks));
  AudioPipelineConfig cfg;

  std::string err;
  if (!pipeline.Start(cfg, &err)) {
    std::cerr << "pipeline.Start failed: " << err << "\n";
    return false;
  }

  if (!WaitUntil(
          [&] {
            std::lock_guard<std::mutex> lock(state->mu);
            return state->block_entered;
          },
          250ms)) {
    std::cerr << "pipeline did not enter blocked capture read\n";
    pipeline.Stop();
    return false;
  }

  auto stop_future = std::async(std::launch::async, [&] {
    pipeline.Stop();
    return true;
  });
  const bool stop_ready =
      (stop_future.wait_for(50ms) == std::future_status::ready);
  (void)stop_future.get();

  int request_stop_calls = 0;
  {
    std::lock_guard<std::mutex> lock(state->mu);
    request_stop_calls = state->request_stop_calls;
  }

  if (!stop_ready) {
    std::cerr << "Stop() stayed blocked while capture read was stuck\n";
    return false;
  }

  if (request_stop_calls == 0) {
    std::cerr << "Stop() never signaled the capture transport to stop\n";
    return false;
  }

  return true;
}

bool TestStopInterruptsBlockedPlaybackWrite() {
  auto state = std::make_shared<BlockingIoState>();
  CopyProcessor processor;

  AudioPipelineHooks hooks;
  hooks.create_io = [state] {
    return std::make_unique<BlockingIo>(state, BlockMode::kWrite, 200ms);
  };

  AudioPipeline pipeline(&processor, std::move(hooks));
  AudioPipelineConfig cfg;

  std::string err;
  if (!pipeline.Start(cfg, &err)) {
    std::cerr << "pipeline.Start failed: " << err << "\n";
    return false;
  }

  if (!WaitUntil(
          [&] {
            std::lock_guard<std::mutex> lock(state->mu);
            return state->block_entered;
          },
          250ms)) {
    std::cerr << "pipeline did not enter blocked playback write\n";
    pipeline.Stop();
    return false;
  }

  auto stop_future = std::async(std::launch::async, [&] {
    pipeline.Stop();
    return true;
  });
  const bool stop_ready =
      (stop_future.wait_for(50ms) == std::future_status::ready);
  (void)stop_future.get();

  int request_stop_calls = 0;
  {
    std::lock_guard<std::mutex> lock(state->mu);
    request_stop_calls = state->request_stop_calls;
  }

  if (!stop_ready) {
    std::cerr << "Stop() stayed blocked while playback write was stuck\n";
    return false;
  }

  if (request_stop_calls == 0) {
    std::cerr << "Stop() never signaled the playback transport to stop\n";
    return false;
  }

  return true;
}

struct ConcurrentStopFixture {
  std::atomic<int> stops_entered{0};
  std::atomic<int> stops_done{0};
  std::atomic<int> join_errors{0};
  std::mutex text_mu;
  std::string join_error_text;

  void RecordJoinError(const std::system_error &e) {
    join_errors.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(text_mu);
    if (join_error_text.empty())
      join_error_text = e.what();
  }

  std::string JoinErrorText() {
    std::lock_guard<std::mutex> lock(text_mu);
    return join_error_text;
  }
};

struct ConcurrentPipelineStopFixture : ConcurrentStopFixture {
  std::shared_ptr<ParkedReadIoState> io_state =
      std::make_shared<ParkedReadIoState>();
  CopyProcessor processor;
  std::unique_ptr<AudioPipeline> pipeline;

  void ReleaseWorker() {
    {
      std::lock_guard<std::mutex> lock(io_state->mu);
      io_state->released = true;
    }
    io_state->cv.notify_all();
  }
};

bool TestConcurrentPipelineStopJoinsWorkerOnce() {
  auto fx = std::make_shared<ConcurrentPipelineStopFixture>();

  AudioPipelineHooks hooks;
  auto io_state = fx->io_state;
  hooks.create_io = [io_state] {
    return std::make_unique<ParkedReadIo>(io_state);
  };

  fx->pipeline =
      std::make_unique<AudioPipeline>(&fx->processor, std::move(hooks));

  AudioPipelineConfig cfg;
  std::string err;
  if (!fx->pipeline->Start(cfg, &err)) {
    std::cerr << "pipeline.Start failed: " << err << "\n";
    return false;
  }

  // Wait longer than the 250 ms that most tests in this file use. This wait
  // is only for progress, and a run with more copies than CPUs can need more
  // than 250 ms to give the worker its first slice.
  if (!WaitUntil(
          [&] {
            std::lock_guard<std::mutex> lock(fx->io_state->mu);
            return fx->io_state->read_entered;
          },
          5000ms)) {
    std::cerr << "pipeline worker did not enter the parked capture read\n";
    fx->ReleaseWorker();
    fx->pipeline->Stop();
    return false;
  }

  // Two callers stop the same pipeline at once. The worker must be joined
  // exactly once: a second join on the same worker either throws
  // "Invalid argument" or waits for a thread that no longer exists.
  auto stopper = [fx] {
    fx->stops_entered.fetch_add(1, std::memory_order_release);
    try {
      fx->pipeline->Stop();
    } catch (const std::system_error &e) {
      fx->RecordJoinError(e);
    }
    fx->stops_done.fetch_add(1, std::memory_order_release);
  };

  std::thread first_stop(stopper);
  std::thread second_stop(stopper);

  // Hold the worker until both callers are inside Stop(). A fixed sleep can
  // pass on a loaded runner without ever making the race, thus wait for the
  // two callers and then give them a short moment to reach the join.
  //
  // The 20 ms settle below is the last step that depends on timing: the count
  // proves that both callers entered Stop(), not that either reached the
  // join. A count taken immediately before the handle lock would close it,
  // but that needs a seam in the production code.
  if (!WaitUntil(
          [&] {
            return fx->stops_entered.load(std::memory_order_acquire) == 2;
          },
          2000ms)) {
    std::cerr << "both Stop() callers never entered Stop()\n";
    fx->ReleaseWorker();
    first_stop.join();
    second_stop.join();
    return false;
  }
  std::this_thread::sleep_for(20ms);
  fx->ReleaseWorker();

  const bool both_returned = WaitUntil(
      [&] { return fx->stops_done.load(std::memory_order_acquire) == 2; },
      5000ms);

  if (!both_returned) {
    // A Stop() is wedged inside join() on a worker that the other Stop()
    // already joined. The wedged thread cannot be recovered and its stale
    // thread id can be reused by a later test, so end the run here with a
    // failure instead of corrupting the tests that follow.
    first_stop.detach();
    second_stop.detach();
    std::cout.flush();
    std::cerr << "[FAIL] concurrent pipeline Stop() never returned; a join() "
                 "is stuck on a worker that another Stop() already joined"
              << std::endl;
    std::_Exit(1);
  }

  first_stop.join();
  second_stop.join();

  if (fx->join_errors.load(std::memory_order_relaxed) != 0) {
    std::cerr << "concurrent pipeline Stop() threw from join(): "
              << fx->JoinErrorText() << "\n";
    return false;
  }

  if (fx->pipeline->GetStats().running) {
    std::cerr << "pipeline still reports running after concurrent Stop()\n";
    return false;
  }

  return true;
}

struct ConcurrentServiceStopFixture : ConcurrentStopFixture {
  std::mutex park_mu;
  std::condition_variable park_cv;
  bool parked = false;
  bool released = false;
  std::atomic<bool> mic_consumer_present{true};
  std::unique_ptr<VirtualAudioService> service;

  // Parks the supervisor thread in the sleep hook until the test releases it.
  void ParkSupervisor() {
    std::unique_lock<std::mutex> lock(park_mu);
    parked = true;
    park_cv.notify_all();
    park_cv.wait(lock, [this] { return released; });
  }

  void ReleaseSupervisor() {
    {
      std::lock_guard<std::mutex> lock(park_mu);
      released = true;
    }
    park_cv.notify_all();
  }

  // See the wait in TestConcurrentPipelineStopJoinsWorkerOnce: a wait for
  // progress must not fail only because the runner is loaded.
  bool WaitForParkedSupervisor() {
    std::unique_lock<std::mutex> lock(park_mu);
    return park_cv.wait_for(lock, 5000ms, [this] { return parked; });
  }
};

bool TestConcurrentServiceStopJoinsSupervisorOnce() {
  auto fx = std::make_shared<ConcurrentServiceStopFixture>();

  VirtualAudioServiceHooks hooks;
  HookMicrophoneConsumerFlag(&hooks, &fx->mic_consumer_present);
  hooks.probe_microphone_backend_availability =
      [](const VirtualAudioServiceConfig &) {
        AudioBackendAvailability avail;
        avail.open_source_ok = true;
        return avail;
      };
  hooks.create_pipeline =
      [](AudioProcessor *) -> std::unique_ptr<AudioPipelineRunner> {
    return std::make_unique<StartFailPipeline>();
  };
  // Capture a raw pointer: the hooks go into the service, the fixture owns
  // the service, and a shared_ptr here would make a cycle that never frees
  // either of them. The fixture outlives the service by construction.
  auto *raw = fx.get();
  hooks.sleep_for = [raw](std::chrono::milliseconds) { raw->ParkSupervisor(); };

  fx->service = std::make_unique<VirtualAudioService>(std::move(hooks));

  VirtualAudioServiceConfig cfg;
  cfg.enabled = true;
  cfg.create_virtual_mic = false;
  cfg.create_virtual_speakers = false;
  cfg.poll_ms = 1;
  cfg.start_retry_ms = 1;

  std::string err;
  if (!fx->service->Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  if (!fx->WaitForParkedSupervisor()) {
    std::cerr << "service supervisor did not enter the parked sleep\n";
    fx->ReleaseSupervisor();
    fx->service->Stop();
    return false;
  }

  // Two callers stop the same service at once. The supervisor must be joined
  // exactly once.
  auto stopper = [fx] {
    fx->stops_entered.fetch_add(1, std::memory_order_release);
    try {
      fx->service->Stop();
    } catch (const std::system_error &e) {
      fx->RecordJoinError(e);
    }
    fx->stops_done.fetch_add(1, std::memory_order_release);
  };

  std::thread first_stop(stopper);
  std::thread second_stop(stopper);

  // Hold the supervisor until both callers are inside Stop(). A fixed sleep
  // can pass on a loaded runner without ever making the race, thus wait for
  // the two callers and then give them a short moment to reach the join.
  //
  // The 20 ms settle below is the last step that depends on timing: the count
  // proves that both callers entered Stop(), not that either reached the
  // join. A count taken immediately before the handle lock would close it,
  // but that needs a seam in the production code.
  if (!WaitUntil(
          [&] {
            return fx->stops_entered.load(std::memory_order_acquire) == 2;
          },
          2000ms)) {
    std::cerr << "both Stop() callers never entered Stop()\n";
    fx->ReleaseSupervisor();
    first_stop.join();
    second_stop.join();
    return false;
  }
  std::this_thread::sleep_for(20ms);
  fx->ReleaseSupervisor();

  const bool both_returned = WaitUntil(
      [&] { return fx->stops_done.load(std::memory_order_acquire) == 2; },
      5000ms);

  if (!both_returned) {
    // See TestConcurrentPipelineStopJoinsWorkerOnce: a wedged join cannot be
    // recovered, thus the run ends here.
    first_stop.detach();
    second_stop.detach();
    std::cout.flush();
    std::cerr << "[FAIL] concurrent service Stop() never returned; a join() "
                 "is stuck on a supervisor that another Stop() already joined"
              << std::endl;
    std::_Exit(1);
  }

  first_stop.join();
  second_stop.join();

  if (fx->join_errors.load(std::memory_order_relaxed) != 0) {
    std::cerr << "concurrent service Stop() threw from join(): "
              << fx->JoinErrorText() << "\n";
    return false;
  }

  if (fx->service->Status().service_running) {
    std::cerr << "service still reports running after concurrent Stop()\n";
    return false;
  }

  return true;
}

constexpr const char *kOverlapOpenError = "overlap io refuses to open";
constexpr const char *kOverlapNoIoError =
    "Audio pipeline I/O backend is not available.";
constexpr const char *kPipelineAlreadyRunningError =
    "Audio pipeline is already running.";

// I/O for the Start()/Stop() overlap test. Open() fails at once, thus the
// worker exits by itself and stays in the handle: the next Start() must join
// it, and so must a Stop() that runs at the same time. A Stop() that reaches
// the handle first now finds no backend of this worker to release, because
// Start() sets io_ and publishes the worker in one hold of thread_mu_. The
// window that is left is a worker that Start() took out of the handle and
// joins outside every lock: a Stop() can release io_ while that worker is
// still inside Open(), and the worker keeps the backend through the shared
// reference that GetActiveIo() gives it, thus the call stays defined.
class OverlapIo final : public AudioPipelineIo {
public:
  bool Open(const AudioPipelineConfig &, std::string *error) override {
    if (error)
      *error = kOverlapOpenError;
    return false;
  }

  bool Read(void *, std::size_t, std::string *error) override {
    if (error)
      error->clear();
    return false;
  }

  bool Write(const void *, std::size_t, std::string *error) override {
    if (error)
      error->clear();
    return false;
  }

  bool GetCaptureLatencyUs(std::uint64_t *) override { return false; }
  bool GetPlaybackLatencyUs(std::uint64_t *) override { return false; }
  void Flush() override {}
  void RequestStop() override {}
};

struct StartStopOverlapFixture : ConcurrentStopFixture {
  std::atomic<int> ios_created{0};
  std::atomic<int> overlaps{0};
  std::atomic<int> starts_done{0};
  std::atomic<int> start_failures{0};
  std::atomic<bool> start_returned{false};
  CopyProcessor processor;
  std::unique_ptr<AudioPipeline> pipeline;
};

// Start() and Stop() on one pipeline at the same time. This is the overlap
// that TestStopInterruptsOpenAfterEarlyStopReset already makes by accident.
// Every attempt begins from a worker that exited by itself and is still in
// the handle, thus the join that Start() makes first and the join that Stop()
// makes both reach the same worker: they must not both join it. Start() has
// no seam after the guard, thus the stopper sweeps its delay across the
// attempts to cover the window up to the publish of the new handle.
bool TestConcurrentStartStopKeepsWorkerHandleUsable() {
  auto fx = std::make_shared<StartStopOverlapFixture>();
  auto *raw = fx.get();

  AudioPipelineHooks hooks;
  hooks.create_io = [raw]() -> std::unique_ptr<AudioPipelineIo> {
    raw->ios_created.fetch_add(1, std::memory_order_relaxed);
    return std::make_unique<OverlapIo>();
  };

  fx->pipeline =
      std::make_unique<AudioPipeline>(&fx->processor, std::move(hooks));

  const AudioPipelineConfig cfg;
  std::string prime_error;
  if (fx->pipeline->Start(cfg, &prime_error)) {
    std::cerr << "pipeline.Start reported success though the I/O refuses to "
                 "open\n";
    return false;
  }

  constexpr int kAttempts = 64;
  for (int attempt = 0; attempt < kAttempts; ++attempt) {
    if (!WaitUntil([&] { return !raw->pipeline->GetStats().running; },
                   2000ms)) {
      std::cerr << "the worker of attempt " << attempt
                << " did not exit by itself\n";
      return false;
    }

    std::atomic<bool> gate{false};
    std::atomic<int> burn{0};
    fx->start_returned.store(false, std::memory_order_release);

    std::thread starter([&] {
      while (!gate.load(std::memory_order_acquire))
        std::this_thread::yield();
      std::string start_error;
      try {
        raw->pipeline->Start(cfg, &start_error);
      } catch (const std::system_error &e) {
        raw->RecordJoinError(e);
      }
      if (start_error != kOverlapOpenError &&
          start_error != kOverlapNoIoError) {
        std::cerr << "pipeline.Start gave an unexpected error on attempt "
                  << attempt << ": " << start_error << "\n";
        raw->start_failures.fetch_add(1, std::memory_order_relaxed);
      }
      raw->start_returned.store(true, std::memory_order_release);
      raw->starts_done.fetch_add(1, std::memory_order_release);
    });

    std::thread stopper([&] {
      while (!gate.load(std::memory_order_acquire))
        std::this_thread::yield();
      // Sweep the delay so the stopper lands at a different point of Start()
      // on every attempt.
      for (int spin = 0; spin < attempt * 32; ++spin)
        burn.fetch_add(1, std::memory_order_relaxed);
      // Record that Start() had not yet returned, thus a green run cannot be
      // one where the two calls never met.
      if (!raw->start_returned.load(std::memory_order_acquire))
        raw->overlaps.fetch_add(1, std::memory_order_relaxed);
      try {
        raw->pipeline->Stop();
      } catch (const std::system_error &e) {
        raw->RecordJoinError(e);
      }
      raw->stops_done.fetch_add(1, std::memory_order_release);
    });

    gate.store(true, std::memory_order_release);

    const bool both_returned = WaitUntil(
        [&] {
          return raw->starts_done.load(std::memory_order_acquire) ==
                     attempt + 1 &&
                 raw->stops_done.load(std::memory_order_acquire) == attempt + 1;
        },
        5000ms);

    if (!both_returned) {
      // A caller is wedged inside join() on a worker that the other caller
      // already joined. A wedged thread cannot be recovered and its stale
      // thread id can go to a later test, thus the run ends here.
      starter.detach();
      stopper.detach();
      std::cout.flush();
      std::cerr << "[FAIL] pipeline Start()/Stop() overlap never returned on "
                   "attempt "
                << attempt
                << "; a join() is stuck on a worker that another caller "
                   "already joined"
                << std::endl;
      std::_Exit(1);
    }

    starter.join();
    stopper.join();

    if (fx->start_failures.load(std::memory_order_relaxed) != 0)
      return false;

    if (fx->join_errors.load(std::memory_order_relaxed) != 0) {
      std::cerr << "Start()/Stop() overlap threw from join(): "
                << fx->JoinErrorText() << "\n";
      return false;
    }
  }

  if (fx->overlaps.load(std::memory_order_relaxed) == 0) {
    std::cerr << "no attempt ever put Stop() inside Start(), thus the sweep "
                 "never made an overlap\n";
    return false;
  }

  // Every Start() must have reached a new worker: a lost handle stops the
  // pipeline from making the next one.
  const int expected_ios = kAttempts + 1;
  if (fx->ios_created.load(std::memory_order_relaxed) != expected_ios) {
    std::cerr << "pipeline made "
              << fx->ios_created.load(std::memory_order_relaxed)
              << " I/O backends, expected " << expected_ios << "\n";
    return false;
  }

  fx->pipeline->Stop();
  if (fx->pipeline->GetStats().running) {
    std::cerr << "pipeline still reports running after the last Stop()\n";
    return false;
  }

  return true;
}

// State that the stop-flag ordering test shares with its I/O backends.
struct StopOrderIoState {
  std::mutex mu;
  std::condition_variable cv;
  // The pipeline stop flag, which SetStopRequestedFlag() gives to every
  // backend. The test reads the flag through this pointer.
  const std::atomic<bool> *stop_flag = nullptr;
  // Open() parks while this is set, thus a worker that nobody tells to stop
  // stays in the backend and a join of that worker never returns. This is
  // what a real capture read does and what the shipped OverlapIo cannot do,
  // because its Open() fails at once.
  bool park_open = false;
  // The gate that holds the first backend inside RequestStop(). It gives the
  // test a seam inside Stop(), after the raise of the stop flag and before
  // the join.
  bool request_stop_entered = false;
  bool request_stop_released = false;
  // The value of the stop flag at the moment the gate opened.
  bool stop_flag_recorded = false;
  bool stop_flag_on_release = false;
};

// I/O for the stop-flag ordering test. Open() fails, as OverlapIo does, but
// it first parks until the pipeline asks it to stop, either with the stop
// flag or with RequestStop().
class StopOrderIo final : public AudioPipelineIo {
public:
  StopOrderIo(std::shared_ptr<StopOrderIoState> state, bool gate_request_stop)
      : state_(std::move(state)), gate_request_stop_(gate_request_stop) {}

  void SetStopRequestedFlag(const std::atomic<bool> *stop_requested) override {
    std::lock_guard<std::mutex> lock(state_->mu);
    stop_requested_ = stop_requested;
    state_->stop_flag = stop_requested;
  }

  bool Open(const AudioPipelineConfig &, std::string *error) override {
    {
      std::unique_lock<std::mutex> lock(state_->mu);
      state_->cv.wait(lock,
                      [this] { return !state_->park_open || StopAsked(); });
    }
    if (error)
      *error = kOverlapOpenError;
    return false;
  }

  bool Read(void *, std::size_t, std::string *error) override {
    if (error)
      error->clear();
    return false;
  }

  bool Write(const void *, std::size_t, std::string *error) override {
    if (error)
      error->clear();
    return false;
  }

  bool GetCaptureLatencyUs(std::uint64_t *) override { return false; }
  bool GetPlaybackLatencyUs(std::uint64_t *) override { return false; }
  void Flush() override {}

  void RequestStop() override {
    std::unique_lock<std::mutex> lock(state_->mu);
    stop_asked_ = true;
    state_->cv.notify_all();
    if (!gate_request_stop_)
      return;
    // Hold Stop() here. It has raised the flag and it has not yet joined.
    state_->request_stop_entered = true;
    state_->cv.notify_all();
    state_->cv.wait(lock, [this] { return state_->request_stop_released; });
    state_->stop_flag_on_release =
        stop_requested_ != nullptr &&
        stop_requested_->load(std::memory_order_acquire);
    state_->stop_flag_recorded = true;
  }

private:
  // Call with state_->mu held.
  bool StopAsked() const {
    return stop_asked_ || (stop_requested_ != nullptr &&
                           stop_requested_->load(std::memory_order_acquire));
  }

  std::shared_ptr<StopOrderIoState> state_;
  bool gate_request_stop_ = false;
  bool stop_asked_ = false;
  const std::atomic<bool> *stop_requested_ = nullptr;
};

struct StopOrderFixture {
  std::shared_ptr<StopOrderIoState> io_state =
      std::make_shared<StopOrderIoState>();
  std::atomic<int> ios_created{0};
  // Set as the first statement of each thread. Both verdicts below are a
  // wait that must run out, thus a thread that the scheduler never ran would
  // give the same result as the call that behaves. These marks say that the
  // thread ran; the test waits for the mark before it starts the wait.
  std::atomic<bool> stopper_entered{false};
  std::atomic<bool> starter_entered{false};
  std::atomic<bool> stopper_done{false};
  std::atomic<bool> starter_done{false};
  std::atomic<bool> cleaner_done{false};
  CopyProcessor processor;
  std::unique_ptr<AudioPipeline> pipeline;
};

// Stop() must keep the stop flag up for the worker that it joins.
//
// Stop() raises the flag, asks the backend to stop and then joins. A Start()
// that runs between the raise and the join clears the flag, makes a new
// backend and publishes a new worker; Stop() then joins a worker that nobody
// told to stop and whose backend never got RequestStop(). A worker parked in
// a real capture read leaves only on one of those two, thus that join never
// returns. VirtualAudioService::Stop() had the same fault.
//
// The test holds Stop() inside RequestStop() and then lets a Start() run. The
// flag must still be up when Stop() leaves RequestStop(), which is true only
// while Stop() holds the worker lock across the raise. A sweep under load
// cannot show this, because the window between the raise and the join is a
// few instructions wide; the gate makes the order instead of racing for it.
bool TestStopKeepsTheStopFlagUpForTheWorkerItJoins() {
  auto fx = std::make_shared<StopOrderFixture>();
  auto *raw = fx.get();
  auto io_state = fx->io_state;

  AudioPipelineHooks hooks;
  hooks.create_io = [raw, io_state]() -> std::unique_ptr<AudioPipelineIo> {
    // Only the first backend holds Stop() inside RequestStop().
    const int index = raw->ios_created.fetch_add(1, std::memory_order_relaxed);
    return std::make_unique<StopOrderIo>(io_state, index == 0);
  };

  fx->pipeline =
      std::make_unique<AudioPipeline>(&fx->processor, std::move(hooks));

  // Prime the pipeline: the first backend stays installed and its worker
  // exits by itself, thus Stop() finds a backend to ask and a worker to join.
  const AudioPipelineConfig cfg;
  std::string prime_error;
  if (fx->pipeline->Start(cfg, &prime_error)) {
    std::cerr << "pipeline.Start reported success though the I/O refuses to "
                 "open\n";
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(io_state->mu);
    io_state->park_open = true;
  }

  std::thread stopper([raw] {
    raw->pipeline->Stop();
    raw->stopper_done.store(true, std::memory_order_release);
  });

  if (!WaitUntil(
          [&] {
            std::lock_guard<std::mutex> lock(io_state->mu);
            return io_state->request_stop_entered;
          },
          5000ms)) {
    std::cerr << "Stop() never reached RequestStop()\n";
    {
      std::lock_guard<std::mutex> lock(io_state->mu);
      io_state->park_open = false;
      io_state->request_stop_released = true;
    }
    io_state->cv.notify_all();
    stopper.join();
    return false;
  }

  std::thread starter([raw, cfg] {
    raw->starter_entered.store(true, std::memory_order_release);
    std::string start_error;
    raw->pipeline->Start(cfg, &start_error);
    raw->starter_done.store(true, std::memory_order_release);
  });

  // The verdict below is a wait that must run out, thus wait for the starter
  // to run first: a starter that the scheduler never gave a turn would pass
  // the test for the wrong reason.
  if (!WaitUntil(
          [&] { return raw->starter_entered.load(std::memory_order_acquire); },
          5000ms)) {
    std::cerr << "the starter thread never ran\n";
    {
      std::lock_guard<std::mutex> lock(io_state->mu);
      io_state->park_open = false;
      io_state->request_stop_released = true;
    }
    io_state->cv.notify_all();
    starter.join();
    stopper.join();
    return false;
  }

  // Wait for the flag to fall. Start() clears it as soon as it is past the
  // handle lock at the top of its body, thus this ends in milliseconds when
  // Stop() raised the flag outside that lock. When Stop() holds the lock,
  // Start() waits for it and this wait runs out, which is the pass. Half a
  // second is a hundred times the margin the failing shape needs, and the
  // passing shape cannot end this wait at any budget.
  const bool flag_cleared = WaitUntil(
      [&] {
        std::lock_guard<std::mutex> lock(io_state->mu);
        return io_state->stop_flag != nullptr &&
               !io_state->stop_flag->load(std::memory_order_acquire);
      },
      500ms);

  {
    std::lock_guard<std::mutex> lock(io_state->mu);
    io_state->request_stop_released = true;
  }
  io_state->cv.notify_all();

  bool flag_up_on_release = false;
  WaitUntil(
      [&] {
        std::lock_guard<std::mutex> lock(io_state->mu);
        return io_state->stop_flag_recorded;
      },
      5000ms);
  {
    std::lock_guard<std::mutex> lock(io_state->mu);
    flag_up_on_release = io_state->stop_flag_on_release;
  }

  // Start() publishes a worker that parks in the backend. One more Stop()
  // raises the flag for it and joins it. That worker leaves on the flag
  // alone, thus this works also when Stop() already released the backend.
  // Wait for the new backend first: Start() clears the flag before it makes
  // one, thus a Stop() before that point loses its raise to Start().
  //
  // A third thread makes this last Stop(), because a Stop() that is wedged in
  // a join holds the worker lock and would take the test thread with it.
  WaitUntil(
      [&] { return raw->ios_created.load(std::memory_order_relaxed) > 1; },
      5000ms);
  std::thread cleaner([raw] {
    raw->pipeline->Stop();
    raw->cleaner_done.store(true, std::memory_order_release);
  });

  const bool all_returned = WaitUntil(
      [&] {
        return raw->stopper_done.load(std::memory_order_acquire) &&
               raw->starter_done.load(std::memory_order_acquire) &&
               raw->cleaner_done.load(std::memory_order_acquire);
      },
      5000ms);

  if (!all_returned) {
    // A caller is wedged inside a join on a worker that nobody told to stop.
    // The three threads are already detached here, thus the run can go on:
    // the other tests do not get a stale thread id from them. Say which
    // caller is still in flight, because that names the path that wedged.
    stopper.detach();
    starter.detach();
    cleaner.detach();
    std::cout.flush();
    std::cerr << "[FAIL] Start()/Stop() with a parked worker never returned; "
                 "a join() is stuck on a worker that was never told to stop"
              << " (stopper_done="
              << raw->stopper_done.load(std::memory_order_acquire)
              << " starter_done="
              << raw->starter_done.load(std::memory_order_acquire)
              << " cleaner_done="
              << raw->cleaner_done.load(std::memory_order_acquire)
              << " ios_created="
              << raw->ios_created.load(std::memory_order_relaxed) << ")"
              << std::endl;
    // The three detached threads still read the fixture, and the destructor
    // of the pipeline in it calls Stop(), which is the call that is wedged.
    // Leak one more reference on purpose, thus the fixture stays alive and
    // the run can go on with the other tests.
    (void)new std::shared_ptr<StopOrderFixture>(fx);
    return false;
  }

  stopper.join();
  starter.join();
  cleaner.join();

  if (flag_cleared || !flag_up_on_release) {
    std::cerr << "Start() cleared the stop flag while Stop() was between the "
                 "raise and the join, thus Stop() can join a worker that "
                 "nobody told to stop\n";
    return false;
  }

  if (fx->pipeline->GetStats().running) {
    std::cerr << "pipeline still reports running after the last Stop()\n";
    return false;
  }

  return true;
}

struct StopRaiseFixture : StopOrderFixture {
  std::mutex gate_mu;
  std::condition_variable gate_cv;
  // Holds the second backend inside the factory, thus Start() parks inside
  // the worker lock, after it cleared the stop flag.
  bool create_io_entered = false;
  bool create_io_released = false;

  void WaitInFactory() {
    std::unique_lock<std::mutex> lock(gate_mu);
    create_io_entered = true;
    gate_cv.notify_all();
    gate_cv.wait(lock, [this] { return create_io_released; });
  }

  void ReleaseFactory() {
    {
      std::lock_guard<std::mutex> lock(gate_mu);
      create_io_released = true;
    }
    gate_cv.notify_all();
  }
};

// Stop() must raise the stop flag under the worker lock, not before it.
//
// TestStopKeepsTheStopFlagUpForTheWorkerItJoins pins where RequestStop()
// sits, not where the raise sits: its gate is inside RequestStop(), thus a
// Stop() with the raise outside the lock still parks with the flag up. This
// test pins the raise itself. Start() clears the flag under the same lock,
// thus while a Start() holds that lock the flag must not move. A Stop() that
// raises the flag before it takes the lock breaks that: it raises the flag
// between the clear of a Start() and the publish of its worker, and it then
// joins a worker whose flag is down.
//
// The seam is the backend factory, which Start() calls under the worker
// lock: it holds Start() there, past the clear, while a Stop() runs.
bool TestStopRaisesTheStopFlagUnderTheWorkerLock() {
  auto fx = std::make_shared<StopRaiseFixture>();
  auto *raw = fx.get();
  auto io_state = fx->io_state;

  AudioPipelineHooks hooks;
  hooks.create_io = [raw, io_state]() -> std::unique_ptr<AudioPipelineIo> {
    const int index = raw->ios_created.fetch_add(1, std::memory_order_relaxed);
    // Only the second backend holds Start() inside the worker lock.
    if (index == 1)
      raw->WaitInFactory();
    return std::make_unique<StopOrderIo>(io_state, false);
  };

  fx->pipeline =
      std::make_unique<AudioPipeline>(&fx->processor, std::move(hooks));

  // Prime the pipeline: the worker of the first backend exits by itself and
  // the backend keeps the pointer to the stop flag, thus the test can read
  // the flag for the whole run.
  const AudioPipelineConfig cfg;
  std::string prime_error;
  if (fx->pipeline->Start(cfg, &prime_error)) {
    std::cerr << "pipeline.Start reported success though the I/O refuses to "
                 "open\n";
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(io_state->mu);
    io_state->park_open = true;
  }

  // The flag is down after a Start(). A raise from here on can only come
  // from the Stop() below.
  {
    std::lock_guard<std::mutex> lock(io_state->mu);
    if (io_state->stop_flag == nullptr ||
        io_state->stop_flag->load(std::memory_order_acquire)) {
      std::cerr << "the stop flag is not down after the priming Start()\n";
      return false;
    }
  }

  std::thread starter([raw, cfg] {
    std::string start_error;
    raw->pipeline->Start(cfg, &start_error);
    raw->starter_done.store(true, std::memory_order_release);
  });

  if (!WaitUntil(
          [&] {
            std::lock_guard<std::mutex> lock(raw->gate_mu);
            return raw->create_io_entered;
          },
          5000ms)) {
    std::cerr << "Start() never reached the backend factory\n";
    fx->ReleaseFactory();
    starter.join();
    fx->pipeline->Stop();
    return false;
  }

  std::thread stopper([raw] {
    raw->stopper_entered.store(true, std::memory_order_release);
    raw->pipeline->Stop();
    raw->stopper_done.store(true, std::memory_order_release);
  });

  // The verdict below is a wait that must run out, thus wait for the stopper
  // to run first: a stopper that the scheduler never gave a turn would pass
  // the test for the wrong reason.
  if (!WaitUntil(
          [&] { return raw->stopper_entered.load(std::memory_order_acquire); },
          5000ms)) {
    std::cerr << "the stopper thread never ran\n";
    fx->ReleaseFactory();
    starter.join();
    stopper.join();
    return false;
  }

  // Wait for the flag to go up. Start() holds the worker lock in the factory,
  // thus a Stop() that raises the flag under that lock waits and this wait
  // runs out, which is the pass. A Stop() that raises the flag before the
  // lock ends this wait in milliseconds.
  const bool flag_raised = WaitUntil(
      [&] {
        std::lock_guard<std::mutex> lock(io_state->mu);
        return io_state->stop_flag != nullptr &&
               io_state->stop_flag->load(std::memory_order_acquire);
      },
      500ms);

  fx->ReleaseFactory();

  const bool both_returned = WaitUntil(
      [&] {
        return raw->starter_done.load(std::memory_order_acquire) &&
               raw->stopper_done.load(std::memory_order_acquire);
      },
      5000ms);

  if (!both_returned) {
    // Both threads are detached below, thus the run can go on. See the wedge
    // handler of TestStopKeepsTheStopFlagUpForTheWorkerItJoins.
    starter.detach();
    stopper.detach();
    std::cout.flush();
    std::cerr << "[FAIL] Start()/Stop() around the backend factory never "
                 "returned (starter_done="
              << raw->starter_done.load(std::memory_order_acquire)
              << " stopper_done="
              << raw->stopper_done.load(std::memory_order_acquire) << ")"
              << std::endl;
    (void)new std::shared_ptr<StopRaiseFixture>(fx);
    return false;
  }

  starter.join();
  stopper.join();

  if (flag_raised) {
    std::cerr << "Stop() raised the stop flag while a Start() held the worker "
                 "lock, thus the raise and the clear of the flag can cross\n";
    return false;
  }

  fx->pipeline->Stop();
  if (fx->pipeline->GetStats().running) {
    std::cerr << "pipeline still reports running after the last Stop()\n";
    return false;
  }

  return true;
}

struct ServiceStartStopOverlapFixture : ConcurrentStopFixture {
  std::atomic<bool> mic_consumer_present{true};
  std::atomic<int> supervisor_loops{0};
  std::atomic<int> overlaps{0};
  std::atomic<int> starts_done{0};
  std::atomic<int> start_failures{0};
  std::atomic<bool> start_returned{false};
  std::unique_ptr<VirtualAudioService> service;
};

// Start() and Stop() on one service at the same time. Every attempt begins
// from a live supervisor, thus the stopper and the Stop() that Start() makes
// first both reach the same supervisor handle: they must not both join it.
// VirtualAudioService has no seam inside Start(), thus the stopper sweeps its
// delay across the attempts to cover the window from the join of the old
// supervisor to the publish of the new one.
bool TestConcurrentServiceStartStopKeepsSupervisorHandleUsable() {
  auto fx = std::make_shared<ServiceStartStopOverlapFixture>();
  auto *raw = fx.get();

  VirtualAudioServiceHooks hooks;
  HookMicrophoneConsumerFlag(&hooks, &fx->mic_consumer_present);
  hooks.probe_microphone_backend_availability =
      [](const VirtualAudioServiceConfig &) {
        AudioBackendAvailability avail;
        avail.open_source_ok = true;
        return avail;
      };
  hooks.create_pipeline =
      [](AudioProcessor *) -> std::unique_ptr<AudioPipelineRunner> {
    return std::make_unique<StartFailPipeline>();
  };
  // Keep the supervisor short: an attempt must cost a thread, not a sleep.
  hooks.sleep_for = [raw](std::chrono::milliseconds) {
    raw->supervisor_loops.fetch_add(1, std::memory_order_relaxed);
    std::this_thread::yield();
  };

  fx->service = std::make_unique<VirtualAudioService>(std::move(hooks));

  VirtualAudioServiceConfig cfg;
  cfg.enabled = true;
  cfg.create_virtual_mic = false;
  cfg.create_virtual_speakers = false;
  cfg.poll_ms = 1;
  cfg.start_retry_ms = 1;

  std::string err;
  if (!fx->service->Start(cfg, &err)) {
    std::cerr << "service.Start failed before the overlap sweep: " << err
              << "\n";
    return false;
  }

  constexpr int kAttempts = 64;
  for (int attempt = 0; attempt < kAttempts; ++attempt) {
    std::atomic<bool> gate{false};
    std::atomic<int> burn{0};
    fx->start_returned.store(false, std::memory_order_release);

    std::thread starter([&] {
      while (!gate.load(std::memory_order_acquire))
        std::this_thread::yield();
      std::string start_error;
      if (!raw->service->Start(cfg, &start_error)) {
        std::cerr << "service.Start failed on attempt " << attempt << ": "
                  << start_error << "\n";
        raw->start_failures.fetch_add(1, std::memory_order_relaxed);
      }
      raw->start_returned.store(true, std::memory_order_release);
      raw->starts_done.fetch_add(1, std::memory_order_release);
    });

    std::thread stopper([&] {
      while (!gate.load(std::memory_order_acquire))
        std::this_thread::yield();
      // Sweep the delay so the stopper lands at a different point of Start()
      // on every attempt.
      for (int spin = 0; spin < attempt * 32; ++spin)
        burn.fetch_add(1, std::memory_order_relaxed);
      // Record that Start() had not yet returned, thus a green run cannot be
      // one where the two calls never met.
      if (!raw->start_returned.load(std::memory_order_acquire))
        raw->overlaps.fetch_add(1, std::memory_order_relaxed);
      try {
        raw->service->Stop();
      } catch (const std::system_error &e) {
        raw->RecordJoinError(e);
      }
      raw->stops_done.fetch_add(1, std::memory_order_release);
    });

    gate.store(true, std::memory_order_release);

    const bool both_returned = WaitUntil(
        [&] {
          return raw->starts_done.load(std::memory_order_acquire) ==
                     attempt + 1 &&
                 raw->stops_done.load(std::memory_order_acquire) == attempt + 1;
        },
        5000ms);

    if (!both_returned) {
      // A caller is wedged inside join() on a supervisor that the other
      // caller already joined. A wedged thread cannot be recovered and its
      // stale thread id can go to a later test, thus the run ends here.
      starter.detach();
      stopper.detach();
      std::cout.flush();
      std::cerr << "[FAIL] service Start()/Stop() overlap never returned on "
                   "attempt "
                << attempt
                << "; a join() is stuck on a supervisor that "
                   "another caller already joined"
                << std::endl;
      std::_Exit(1);
    }

    starter.join();
    stopper.join();

    if (fx->start_failures.load(std::memory_order_relaxed) != 0)
      return false;

    if (fx->join_errors.load(std::memory_order_relaxed) != 0) {
      std::cerr << "Start()/Stop() overlap threw from join(): "
                << fx->JoinErrorText() << "\n";
      return false;
    }

    // Give the next attempt a live supervisor again. A stopper that landed
    // after Start() leaves the service stopped, which is a correct result.
    if (!fx->service->Status().service_running &&
        !fx->service->Start(cfg, &err)) {
      std::cerr << "service.Start failed after attempt " << attempt << ": "
                << err << "\n";
      return false;
    }
  }

  if (fx->overlaps.load(std::memory_order_relaxed) == 0) {
    std::cerr << "no attempt ever put Stop() inside Start(), thus the sweep "
                 "never made an overlap\n";
    return false;
  }

  // The supervisor handle must still work after all that overlap.
  fx->service->Stop();
  if (!fx->service->Start(cfg, &err)) {
    std::cerr << "service.Start failed after the overlap sweep: " << err
              << "\n";
    return false;
  }

  if (!fx->service->Status().service_running) {
    std::cerr << "service does not report running after the overlap sweep\n";
    fx->service->Stop();
    return false;
  }

  const bool supervisor_ran = WaitUntil(
      [&] { return fx->supervisor_loops.load(std::memory_order_relaxed) > 0; },
      2000ms);

  fx->service->Stop();

  if (!supervisor_ran) {
    std::cerr << "no supervisor ever ran during the overlap sweep\n";
    return false;
  }

  if (fx->service->Status().service_running) {
    std::cerr << "service still reports running after the last Stop()\n";
    return false;
  }

  return true;
}

constexpr const char *kPipelineAlreadyStartingError =
    "Audio pipeline is already starting.";
constexpr const char *kServiceAlreadyStartingError =
    "Virtual audio service is already starting.";

// State that the backend release test shares with its backends.
struct ReleaseGateState {
  std::mutex mu;
  std::condition_variable cv;
  // The gate inside the destructor of the first backend. It holds Stop()
  // where that call releases the backend, which is the step this test is
  // about.
  bool release_entered = false;
  bool release_released = false;
};

// I/O for the backend release test. Open() fails at once, as OverlapIo does,
// but the destructor of the first backend parks. Stop() destroys the backend
// that it takes out of the handle, thus the gate holds Stop() at the
// release.
class ReleaseGateIo final : public AudioPipelineIo {
public:
  ReleaseGateIo(std::shared_ptr<ReleaseGateState> state, bool gate_destructor)
      : state_(std::move(state)), gate_destructor_(gate_destructor) {}

  ~ReleaseGateIo() override {
    if (!gate_destructor_)
      return;
    std::unique_lock<std::mutex> lock(state_->mu);
    state_->release_entered = true;
    state_->cv.notify_all();
    state_->cv.wait(lock, [this] { return state_->release_released; });
  }

  bool Open(const AudioPipelineConfig &, std::string *error) override {
    if (error)
      *error = kOverlapOpenError;
    return false;
  }

  bool Read(void *, std::size_t, std::string *error) override {
    if (error)
      error->clear();
    return false;
  }

  bool Write(const void *, std::size_t, std::string *error) override {
    if (error)
      error->clear();
    return false;
  }

  bool GetCaptureLatencyUs(std::uint64_t *) override { return false; }
  bool GetPlaybackLatencyUs(std::uint64_t *) override { return false; }
  void Flush() override {}
  void RequestStop() override {}

private:
  std::shared_ptr<ReleaseGateState> state_;
  bool gate_destructor_ = false;
};

struct ReleaseGateFixture {
  std::shared_ptr<ReleaseGateState> gate = std::make_shared<ReleaseGateState>();
  std::atomic<int> ios_created{0};
  std::atomic<bool> stopper_entered{false};
  std::atomic<bool> stopper_done{false};
  std::atomic<bool> starter_entered{false};
  std::atomic<bool> starter_done{false};
  CopyProcessor processor;
  std::unique_ptr<AudioPipeline> pipeline;

  void ReleaseBackend() {
    {
      std::lock_guard<std::mutex> lock(gate->mu);
      gate->release_released = true;
    }
    gate->cv.notify_all();
  }
};

// Stop() must release the backend under the worker lock.
//
// With the release outside that lock, a Stop() that already left the lock
// can drop the backend of a worker that a Start() published a moment later.
// That worker then parks in a backend that no later Stop() can ask to stop,
// and the join of it never returns. The window is a few instructions wide,
// thus a sweep under load does not find it; the gate makes the order
// instead of racing for it.
//
// The seam is the destructor of the backend, which Stop() runs where it
// releases it. While the gate holds Stop() there, a Start() must not get as
// far as a backend of its own: the worker lock is what keeps it out.
bool TestStopReleasesTheBackendUnderTheWorkerLock() {
  auto fx = std::make_shared<ReleaseGateFixture>();
  auto *raw = fx.get();
  auto gate = fx->gate;

  AudioPipelineHooks hooks;
  hooks.create_io = [raw, gate]() -> std::unique_ptr<AudioPipelineIo> {
    // Only the first backend holds Stop() inside its destructor.
    const int index = raw->ios_created.fetch_add(1, std::memory_order_relaxed);
    return std::make_unique<ReleaseGateIo>(gate, index == 0);
  };

  fx->pipeline =
      std::make_unique<AudioPipeline>(&fx->processor, std::move(hooks));

  // Prime the pipeline: the first backend stays installed and its worker
  // exits by itself, thus Stop() finds a worker to join and a backend to
  // release.
  const AudioPipelineConfig cfg;
  std::string prime_error;
  if (fx->pipeline->Start(cfg, &prime_error)) {
    std::cerr << "pipeline.Start reported success though the I/O refuses to "
                 "open\n";
    return false;
  }

  std::thread stopper([raw] {
    raw->stopper_entered.store(true, std::memory_order_release);
    raw->pipeline->Stop();
    raw->stopper_done.store(true, std::memory_order_release);
  });

  if (!WaitUntil(
          [&] {
            std::lock_guard<std::mutex> lock(gate->mu);
            return gate->release_entered;
          },
          5000ms)) {
    std::cerr << "Stop() never released the backend\n";
    fx->ReleaseBackend();
    stopper.join();
    return false;
  }

  std::thread starter([raw, cfg] {
    raw->starter_entered.store(true, std::memory_order_release);
    std::string start_error;
    raw->pipeline->Start(cfg, &start_error);
    raw->starter_done.store(true, std::memory_order_release);
  });

  // The verdict below is a wait that must run out, thus wait for the starter
  // to run first.
  if (!WaitUntil(
          [&] { return raw->starter_entered.load(std::memory_order_acquire); },
          5000ms)) {
    std::cerr << "the starter thread never ran\n";
    fx->ReleaseBackend();
    starter.join();
    stopper.join();
    return false;
  }

  // Wait for a second backend. Stop() holds the worker lock across the
  // release, thus Start() waits at its first statement and this wait runs
  // out, which is the pass. With the release outside that lock, Start() runs
  // on and makes a backend in milliseconds.
  const bool second_io = WaitUntil(
      [&] { return raw->ios_created.load(std::memory_order_relaxed) > 1; },
      500ms);

  fx->ReleaseBackend();

  const bool both_returned = WaitUntil(
      [&] {
        return raw->stopper_done.load(std::memory_order_acquire) &&
               raw->starter_done.load(std::memory_order_acquire);
      },
      5000ms);

  if (!both_returned) {
    // Both threads are detached below, thus the run can go on. See the wedge
    // handler of TestStopKeepsTheStopFlagUpForTheWorkerItJoins.
    stopper.detach();
    starter.detach();
    std::cout.flush();
    std::cerr << "[FAIL] Start()/Stop() around the backend release never "
                 "returned (stopper_done="
              << raw->stopper_done.load(std::memory_order_acquire)
              << " starter_done="
              << raw->starter_done.load(std::memory_order_acquire) << ")"
              << std::endl;
    (void)new std::shared_ptr<ReleaseGateFixture>(fx);
    return false;
  }

  stopper.join();
  starter.join();

  if (second_io) {
    std::cerr << "Start() made a backend while Stop() was releasing one, thus "
                 "the release is outside the worker lock\n";
    return false;
  }

  // The lock held the starter back; it was not absent. The backend that it
  // could not make while the gate was closed must be there now.
  if (fx->ios_created.load(std::memory_order_relaxed) != 2) {
    std::cerr << "pipeline made "
              << fx->ios_created.load(std::memory_order_relaxed)
              << " I/O backends, expected 2\n";
    return false;
  }

  fx->pipeline->Stop();
  if (fx->pipeline->GetStats().running) {
    std::cerr << "pipeline still reports running after the last Stop()\n";
    return false;
  }

  return true;
}

// State that the previous-worker join test shares with its backends.
struct JoinGateState {
  std::mutex mu;
  std::condition_variable cv;
  // The gate inside the thread-local mark of the first worker. It holds that
  // worker on its way out.
  bool worker_exiting = false;
  bool worker_released = false;
};

// Parks a worker thread after ThreadMain() returned. A thread-local object
// is destroyed at the end of the thread, thus after the guard in
// ThreadMain() cleared running_ and after Start() got its answer from the
// startup handshake: the worker is still joinable while the pipeline reports
// itself stopped. That is the state in which a Start() passes both of its
// guards and still finds a live worker in the handle to join, and that join
// is what this test is about.
struct WorkerExitGate {
  std::shared_ptr<JoinGateState> state;

  ~WorkerExitGate() {
    if (!state)
      return;
    std::unique_lock<std::mutex> lock(state->mu);
    state->worker_exiting = true;
    state->cv.notify_all();
    state->cv.wait(lock, [this] { return state->worker_released; });
  }
};

// I/O for the previous-worker join test. Open() fails at once, as OverlapIo
// does; the first backend also arms the mark that holds its worker on the
// way out.
class JoinGateIo final : public AudioPipelineIo {
public:
  JoinGateIo(std::shared_ptr<JoinGateState> state, bool arm_exit_gate)
      : state_(std::move(state)), arm_exit_gate_(arm_exit_gate) {}

  bool Open(const AudioPipelineConfig &, std::string *error) override {
    if (arm_exit_gate_) {
      // Open() runs on the worker thread, thus the mark belongs to that
      // thread and parks it, not the caller of Start().
      thread_local WorkerExitGate gate;
      gate.state = state_;
    }
    if (error)
      *error = kOverlapOpenError;
    return false;
  }

  bool Read(void *, std::size_t, std::string *error) override {
    if (error)
      error->clear();
    return false;
  }

  bool Write(const void *, std::size_t, std::string *error) override {
    if (error)
      error->clear();
    return false;
  }

  bool GetCaptureLatencyUs(std::uint64_t *) override { return false; }
  bool GetPlaybackLatencyUs(std::uint64_t *) override { return false; }
  void Flush() override {}
  void RequestStop() override {}

private:
  std::shared_ptr<JoinGateState> state_;
  bool arm_exit_gate_ = false;
};

struct JoinGateFixture {
  std::shared_ptr<JoinGateState> gate = std::make_shared<JoinGateState>();
  std::atomic<int> ios_created{0};
  std::atomic<int> starts_entered{0};
  std::atomic<int> starts_done{0};
  std::atomic<int> refusals{0};
  std::atomic<int> unexpected{0};
  CopyProcessor processor;
  std::unique_ptr<AudioPipeline> pipeline;

  void ReleaseWorker() {
    {
      std::lock_guard<std::mutex> lock(gate->mu);
      gate->worker_released = true;
    }
    gate->cv.notify_all();
  }
};

// Start() must join the worker of the last run outside the worker lock.
//
// A worker parked in the backend leaves only on RequestStop(), and Stop()
// needs the worker lock to make that call. A join under that lock would thus
// hold the lock for as long as the park, and the pair would never unwind.
//
// The seam is a thread-local mark that the first backend arms: it parks its
// worker at the end of the thread, after ThreadMain() cleared running_.
// Start() thus finds a live worker in the handle and stays in the join for
// as long as the test wants. A second Start() must be refused while the
// first one is there, and that refusal takes the worker lock: it can only
// happen while the join is outside it.
bool TestStartJoinsThePreviousWorkerOutsideTheWorkerLock() {
  auto fx = std::make_shared<JoinGateFixture>();
  auto *raw = fx.get();
  auto gate = fx->gate;

  AudioPipelineHooks hooks;
  hooks.create_io = [raw, gate]() -> std::unique_ptr<AudioPipelineIo> {
    // Only the worker of the first backend parks on its way out.
    const int index = raw->ios_created.fetch_add(1, std::memory_order_relaxed);
    return std::make_unique<JoinGateIo>(gate, index == 0);
  };

  fx->pipeline =
      std::make_unique<AudioPipeline>(&fx->processor, std::move(hooks));

  // Prime the pipeline: the worker fails to open, reports the failure to
  // Start() and then parks on its way out. It stays in the handle, and the
  // pipeline reports itself stopped.
  const AudioPipelineConfig cfg;
  std::string prime_error;
  if (fx->pipeline->Start(cfg, &prime_error)) {
    std::cerr << "pipeline.Start reported success though the I/O refuses to "
                 "open\n";
    return false;
  }

  if (!WaitUntil(
          [&] {
            std::lock_guard<std::mutex> lock(gate->mu);
            return gate->worker_exiting;
          },
          5000ms)) {
    std::cerr << "the worker of the priming Start() never reached its mark\n";
    fx->ReleaseWorker();
    return false;
  }

  if (fx->pipeline->GetStats().running) {
    std::cerr << "the pipeline reports itself running while its worker is on "
                 "the way out\n";
    fx->ReleaseWorker();
    return false;
  }

  // Two callers of Start(). One takes the handle and joins the parked
  // worker; the other must be refused, and cannot be refused before it has
  // the worker lock.
  const auto start_once = [raw, cfg] {
    raw->starts_entered.fetch_add(1, std::memory_order_release);
    std::string start_error;
    if (!raw->pipeline->Start(cfg, &start_error)) {
      if (start_error == kPipelineAlreadyStartingError) {
        raw->refusals.fetch_add(1, std::memory_order_release);
      } else if (start_error != kOverlapOpenError &&
                 start_error != kOverlapNoIoError) {
        raw->unexpected.fetch_add(1, std::memory_order_relaxed);
      }
    }
    raw->starts_done.fetch_add(1, std::memory_order_release);
  };

  std::thread first(start_once);
  std::thread second(start_once);

  if (!WaitUntil(
          [&] {
            return raw->starts_entered.load(std::memory_order_acquire) == 2;
          },
          5000ms)) {
    std::cerr << "the two Start() threads never ran\n";
    fx->ReleaseWorker();
    first.join();
    second.join();
    return false;
  }

  // The caller that loses gets its answer from the worker lock. With the
  // join outside that lock this takes milliseconds; with the join under it
  // the loser waits for the parked worker, which only this test can release.
  const bool refused = WaitUntil(
      [&] { return raw->refusals.load(std::memory_order_acquire) == 1; },
      2000ms);

  fx->ReleaseWorker();

  const bool both_returned = WaitUntil(
      [&] { return raw->starts_done.load(std::memory_order_acquire) == 2; },
      5000ms);

  if (!both_returned) {
    // Both threads are detached below, thus the run can go on. See the wedge
    // handler of TestStopKeepsTheStopFlagUpForTheWorkerItJoins.
    first.detach();
    second.detach();
    std::cout.flush();
    std::cerr << "[FAIL] two Start() callers on a parked worker never "
                 "returned (starts_done="
              << raw->starts_done.load(std::memory_order_acquire) << ")"
              << std::endl;
    (void)new std::shared_ptr<JoinGateFixture>(fx);
    return false;
  }

  first.join();
  second.join();

  if (!refused) {
    std::cerr << "no Start() was refused while another one joined the worker "
                 "of the last run, thus that join holds the worker lock\n";
    return false;
  }

  if (fx->unexpected.load(std::memory_order_relaxed) != 0) {
    std::cerr << "a Start() gave an unexpected error\n";
    return false;
  }

  // The caller that won made a backend of its own, thus the handle came
  // through the join and the refusal.
  if (fx->ios_created.load(std::memory_order_relaxed) != 2) {
    std::cerr << "pipeline made "
              << fx->ios_created.load(std::memory_order_relaxed)
              << " I/O backends, expected 2\n";
    return false;
  }

  fx->pipeline->Stop();
  if (fx->pipeline->GetStats().running) {
    std::cerr << "pipeline still reports running after the last Stop()\n";
    return false;
  }

  return true;
}

struct DoubleStartFixture {
  // The gate that holds the first caller of an attempt, so that the second
  // caller reaches the publish of the handle in the same window.
  std::mutex gate_mu;
  std::condition_variable gate_cv;
  bool gate_released = false;

  std::atomic<int> starts_entered{0};
  std::atomic<int> already_starting{0};
  std::atomic<int> unexpected{0};
  std::mutex text_mu;
  std::string unexpected_text;

  void RecordUnexpected(const std::string &text) {
    unexpected.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(text_mu);
    if (unexpected_text.empty())
      unexpected_text = text;
  }

  std::string UnexpectedText() {
    std::lock_guard<std::mutex> lock(text_mu);
    return unexpected_text;
  }

  void ArmGate() {
    std::lock_guard<std::mutex> lock(gate_mu);
    gate_released = false;
  }

  void OpenGate() {
    {
      std::lock_guard<std::mutex> lock(gate_mu);
      gate_released = true;
    }
    gate_cv.notify_all();
  }
};

struct DoublePipelineStartFixture : DoubleStartFixture {
  int create_io_entered = 0; // guarded by gate_mu
  CopyProcessor processor;
  std::unique_ptr<AudioPipeline> pipeline;
};

// Two Start() callers on one pipeline at the same time. The running_ guard is
// a test and then a store, thus both callers pass it and, without the start
// mark, both reach the move-assign of the worker handle. A move-assign onto a
// joinable handle ends the process with std::terminate().
//
// The check that stops this must be live in a release build: every automated
// configure passes -DCMAKE_BUILD_TYPE=Release, thus an assert() is in no
// binary that CI or a package makes. The second caller must get an error.
//
// CreateIo() runs under the worker lock, thus a caller that is held inside it
// keeps the other caller at the start mark. That is the widest seam Start()
// gives; the attempts cover the rest of the window.
bool TestConcurrentPipelineStartFailsAndKeepsTheProcess() {
  auto fx = std::make_shared<DoublePipelineStartFixture>();
  auto *raw = fx.get();

  AudioPipelineHooks hooks;
  hooks.create_io = [raw]() -> std::unique_ptr<AudioPipelineIo> {
    {
      std::unique_lock<std::mutex> lock(raw->gate_mu);
      ++raw->create_io_entered;
      raw->gate_cv.notify_all();
      if (raw->create_io_entered == 1)
        raw->gate_cv.wait(lock, [raw] { return raw->gate_released; });
    }
    return std::make_unique<OverlapIo>();
  };

  fx->pipeline =
      std::make_unique<AudioPipeline>(&fx->processor, std::move(hooks));

  const AudioPipelineConfig cfg;
  constexpr int kAttempts = 64;
  for (int attempt = 0; attempt < kAttempts; ++attempt) {
    {
      std::lock_guard<std::mutex> lock(fx->gate_mu);
      fx->create_io_entered = 0;
      fx->gate_released = false;
    }
    fx->starts_entered.store(0, std::memory_order_release);

    std::atomic<bool> go{false};
    auto starter = [raw, cfg, &go] {
      raw->starts_entered.fetch_add(1, std::memory_order_release);
      while (!go.load(std::memory_order_acquire))
        std::this_thread::yield();
      std::string start_error;
      if (raw->pipeline->Start(cfg, &start_error)) {
        raw->RecordUnexpected("Start reported success though the I/O refuses "
                              "to open");
      } else if (start_error == kPipelineAlreadyStartingError) {
        raw->already_starting.fetch_add(1, std::memory_order_relaxed);
      } else if (start_error != kOverlapOpenError &&
                 start_error != kOverlapNoIoError &&
                 start_error != kPipelineAlreadyRunningError) {
        raw->RecordUnexpected(start_error);
      }
    };

    std::thread first(starter);
    std::thread second(starter);
    // Both callers are in the lambda before either calls Start(), thus the
    // one that does not hold the gate always reaches the start mark.
    WaitUntil(
        [&] {
          return raw->starts_entered.load(std::memory_order_acquire) == 2;
        },
        5000ms);
    go.store(true, std::memory_order_release);

    WaitUntil(
        [&] {
          std::lock_guard<std::mutex> lock(raw->gate_mu);
          return raw->create_io_entered >= 1;
        },
        5000ms);
    fx->OpenGate();

    first.join();
    second.join();

    if (fx->unexpected.load(std::memory_order_relaxed) != 0) {
      std::cerr << "pipeline.Start gave an unexpected result on attempt "
                << attempt << ": " << fx->UnexpectedText() << "\n";
      fx->pipeline->Stop();
      return false;
    }

    // Leave the next attempt a stopped pipeline with a free handle.
    fx->pipeline->Stop();
  }

  if (fx->already_starting.load(std::memory_order_relaxed) == 0) {
    std::cerr << "no attempt ever put two Start() callers in Start() at the "
                 "same time, thus the sweep proves nothing\n";
    return false;
  }

  if (fx->pipeline->GetStats().running) {
    std::cerr << "pipeline still reports running after the last Stop()\n";
    return false;
  }

  return true;
}

struct DoubleServiceStartFixture : DoubleStartFixture {
  std::atomic<bool> mic_consumer_present{true};
  int sleepers_parked = 0; // guarded by gate_mu
  std::unique_ptr<VirtualAudioService> service;
};

// Two Start() callers on one service at the same time. Without the start
// mark both callers leave the Stop() that Start() makes first before either
// publishes the supervisor, and the second move-assign lands on a joinable
// handle. As in the pipeline test, the check must be live in a release
// build.
//
// The seam is the sleep of the supervisor: a parked supervisor holds the
// Stop() that the first caller makes inside its join, and that Stop() holds
// the handle lock, thus every other caller waits at the start mark behind
// it. Each attempt also proves that a caller that fails leaves the service
// of the caller that won running.
bool TestConcurrentServiceStartFailsAndKeepsTheProcess() {
  auto fx = std::make_shared<DoubleServiceStartFixture>();
  auto *raw = fx.get();

  VirtualAudioServiceHooks hooks;
  HookMicrophoneConsumerFlag(&hooks, &fx->mic_consumer_present);
  hooks.probe_microphone_backend_availability =
      [](const VirtualAudioServiceConfig &) {
        AudioBackendAvailability avail;
        avail.open_source_ok = true;
        return avail;
      };
  hooks.create_pipeline =
      [](AudioProcessor *) -> std::unique_ptr<AudioPipelineRunner> {
    return std::make_unique<StartFailPipeline>();
  };
  hooks.sleep_for = [raw](std::chrono::milliseconds) {
    std::unique_lock<std::mutex> lock(raw->gate_mu);
    ++raw->sleepers_parked;
    raw->gate_cv.notify_all();
    raw->gate_cv.wait(lock, [raw] { return raw->gate_released; });
    --raw->sleepers_parked;
  };

  fx->service = std::make_unique<VirtualAudioService>(std::move(hooks));

  VirtualAudioServiceConfig cfg;
  cfg.enabled = true;
  cfg.create_virtual_mic = false;
  cfg.create_virtual_speakers = false;
  cfg.poll_ms = 1;
  cfg.start_retry_ms = 1;

  std::string err;
  if (!fx->service->Start(cfg, &err)) {
    std::cerr << "service.Start failed before the sweep: " << err << "\n";
    return false;
  }

  // The start mark makes every attempt meet, thus the sweep no longer needs
  // to cover a window: 32 attempts give about 40 refusals on this host.
  constexpr int kAttempts = 32;
  constexpr int kCallers = 4;
  for (int attempt = 0; attempt < kAttempts; ++attempt) {
    fx->ArmGate();
    fx->starts_entered.store(0, std::memory_order_release);

    std::atomic<bool> go{false};
    auto starter = [raw, cfg, &go] {
      raw->starts_entered.fetch_add(1, std::memory_order_release);
      while (!go.load(std::memory_order_acquire))
        std::this_thread::yield();
      std::string start_error;
      if (!raw->service->Start(cfg, &start_error)) {
        if (start_error == kServiceAlreadyStartingError)
          raw->already_starting.fetch_add(1, std::memory_order_relaxed);
        else
          raw->RecordUnexpected(start_error);
      }
    };

    // Park the supervisor before the callers run, thus the Stop() that
    // Start() makes first waits inside its join.
    WaitUntil(
        [&] {
          std::lock_guard<std::mutex> lock(raw->gate_mu);
          return raw->sleepers_parked >= 1;
        },
        5000ms);

    std::vector<std::thread> starters;
    for (int i = 0; i < kCallers; ++i)
      starters.emplace_back(starter);
    WaitUntil(
        [&] {
          return raw->starts_entered.load(std::memory_order_acquire) ==
                 kCallers;
        },
        5000ms);
    go.store(true, std::memory_order_release);
    std::this_thread::sleep_for(5ms);
    fx->OpenGate();

    for (auto &t : starters)
      t.join();

    if (fx->unexpected.load(std::memory_order_relaxed) != 0) {
      std::cerr << "service.Start gave an unexpected result on attempt "
                << attempt << ": " << fx->UnexpectedText() << "\n";
      fx->OpenGate();
      fx->service->Stop();
      return false;
    }

    // A caller that fails must leave the service of the caller that won
    // alone. Start() stops the supervisor of the last run and clears the
    // status before it publishes, thus a caller that fails after all of that
    // leaves service_running down while the supervisor of the winner is
    // alive, and it restores none of it.
    if (!fx->service->Status().service_running) {
      std::cerr << "the status reported the service down on attempt " << attempt
                << ", thus a Start() caller that failed left the service of "
                   "the caller that won stopped\n";
      fx->OpenGate();
      fx->service->Stop();
      return false;
    }
  }

  fx->OpenGate();
  fx->service->Stop();

  if (fx->already_starting.load(std::memory_order_relaxed) == 0) {
    std::cerr << "no attempt ever put two Start() callers in Start() at the "
                 "same time, thus the sweep proves nothing\n";
    return false;
  }

  if (fx->service->Status().service_running) {
    std::cerr << "service still reports running after the last Stop()\n";
    return false;
  }

  return true;
}

} // namespace

int main() {
  const struct {
    const char *name;
    bool (*fn)();
    bool mock_safe_mic_source = false;
  } tests[] = {
      {"pactl load-module quotes vector arguments",
       &TestPactlLoadModuleQuotesVectorArguments},
      {"pactl load-module string compatibility splitter",
       &TestPactlLoadModuleStringCompatibilitySplitter},
      {"pactl default source/sink fallback to info",
       &TestPactlDefaultSourceAndSinkFallbackToInfo},
      {"pactl proplist commands quote args and detect failures",
       &TestPactlProplistCommandsQuoteArgumentsAndDetectFailures},
      {"pactl consumer lists parse source outputs and sink inputs",
       &TestPactlConsumerListsParseSourceOutputsAndSinkInputs},
      {"audio source safety rejects virtual and monitor sources",
       &TestAudioSourceSafetyRejectsVirtualAndMonitorSources},
      {"audio source auto falls back from unsafe default source",
       &TestAudioSourceAutoFallsBackFromUnsafeDefaultSource},
      {"audio source auto fails when no safe source exists",
       &TestAudioSourceAutoFailsWhenNoSafeSourceExists},
      {"virtual audio service reports resolved auto source and warnings",
       &TestVirtualAudioServiceReportsResolvedAutoSourceAndWarnings},
      {"virtual audio service preserves unavailable configured source",
       &TestVirtualAudioServicePreservesUnavailableConfiguredSource},
      {"speaker target safety rejects virtual and monitor endpoints",
       &TestSpeakerTargetSafetyRejectsVirtualAndMonitorEndpoints},
      {"speaker target auto falls back from unsafe default sink",
       &TestSpeakerTargetAutoFallsBackFromUnsafeDefaultSink},
      {"status text surfaces module list failures",
       &TestStatusTextSurfacesModuleListFailure},
      {"virtual mic create propagates list failure without loading",
       &TestCreateVirtualMicPropagatesListFailureWithoutLoading},
      {"virtual speaker create propagates list failure without loading",
       &TestCreateVirtualSpeakerPropagatesListFailureWithoutLoading},
      {"virtual speaker loopback falls back from virtual default sink",
       &TestVirtualSpeakerLoopbackFallsBackFromVirtualDefaultSink},
      {"virtual speaker loopback rejects virtual target",
       &TestVirtualSpeakerLoopbackRejectsVirtualTarget},
      {"virtual speaker loopback rejects virtual target before stop",
       &TestVirtualSpeakerLoopbackRejectsVirtualTargetBeforeStop},
      {"virtual speaker loopback restart propagates stop failure",
       &TestVirtualSpeakerLoopbackRestartPropagatesStopFailure},
      {"destroy virtual speaker propagates null sink unload failure",
       &TestDestroyVirtualSpeakerPropagatesNullSinkUnloadFailure},
      {"virtual mic loopback stop propagates unload failure",
       &TestVirtualMicStopLoopbackPropagatesUnloadFailure},
      {"destroy virtual mic preserves remaining state on null unload failure",
       &TestDestroyVirtualMicPreservesRemainingStateOnNullUnloadFailure},
      {"mic pipeline does not start without consumer",
       &TestMicrophonePipelineDoesNotStartWithoutConsumer, true},
      {"mic pipeline starts when consumer appears",
       &TestMicrophonePipelineStartsWhenConsumerAppears, true},
      {"mic pipeline stops when consumer disappears",
       &TestMicrophonePipelineStopsWhenConsumerDisappears, true},
      {"mic grace window absorbs consumer flapping",
       &TestMicrophoneGraceWindowAbsorbsConsumerFlapping, true},
      {"mic consumer detection recovers after errors",
       &TestMicrophoneConsumerDetectionRecoversAfterErrors, true},
      {"speaker pipeline follows consumer gate",
       &TestSpeakerPipelineFollowsConsumerGate},
      {"speaker grace window absorbs consumer flapping",
       &TestSpeakerGraceWindowAbsorbsConsumerFlapping},
      {"speaker loopback pass-through status is not consumer-gated",
       &TestSpeakerLoopbackPassThroughStatusIsNotConsumerGated},
      {"mic pipeline restarts after worker death",
       &TestMicrophonePipelineRestartsWhenWorkerDies, true},
      {"mic pipeline preserves worker death error",
       &TestMicrophonePipelinePreservesWorkerDeathError, true},
      {"status remains responsive during retry sleep",
       &TestStatusDoesNotBlockDuringRetrySleep, true},
      {"mic null pipeline factory fails without crash",
       &TestMicrophoneNullPipelineFactoryFailsWithoutCrash, true},
      {"speaker pipeline start failure clears route state",
       &TestSpeakerPipelineStartFailureClearsRouteState},
      {"open audio failure cooldown avoids restart churn",
       &TestOpenAudioFailureCooldownAvoidsRestartChurn, true},
      {"forced Maxine mic failure falls back to pass-through",
       &TestForcedMaxineMicrophoneFailureFallsBackToPassthrough, true},
      {"mic availability cache ignores speaker-only changes",
       &TestMicrophoneAvailabilityCacheIgnoresSpeakerOnlyChanges, true},
      {"speaker availability cache ignores microphone-only changes",
       &TestSpeakerAvailabilityCacheIgnoresMicrophoneOnlyChanges},
      {"mic dead worker backs off before restart",
       &TestMicrophoneDeadWorkerBacksOffBeforeRestart, true},
      {"speaker dead worker backs off and clears route",
       &TestSpeakerDeadWorkerBacksOffAndClearsRoute},
      {"speaker pipeline stats clear when processing disabled",
       &TestSpeakerPipelineStatsClearWhenProcessingDisabled},
      {"speaker loopback restart failure clears route state",
       &TestSpeakerLoopbackRestartFailureClearsRouteState},
      {"speaker real helper stop failure keeps old route active",
       &TestSpeakerLoopbackRealHelperStopFailureKeepsOldRouteActive},
      {"virtual speaker destroy failure backs off and keeps present",
       &TestVirtualSpeakerDestroyFailureBacksOffAndKeepsPresent},
      {"speaker loopback stop failure blocks pipeline start",
       &TestSpeakerLoopbackStopFailureBlocksPipelineStart},
      {"speaker loopback stop failure prevents destroy and keeps route",
       &TestSpeakerLoopbackStopFailurePreventsDestroyAndKeepsRoute},
      {"stop interrupts open after early stop reset",
       &TestStopInterruptsOpenAfterEarlyStopReset},
      {"stop interrupts blocked flush", &TestStopInterruptsBlockedFlush},
      {"start returns open failure and can retry",
       &TestStartReturnsOpenFailureAndCanRetry},
      {"pipeline surfaces capture disconnect",
       &TestPipelineSurfacesCaptureDisconnectError},
      {"latency guard sums capture and playback before resync",
       &TestLatencyGuardSumsCaptureAndPlaybackBeforeResync},
      {"latency query failure clears stale last value",
       &TestLatencyQueryFailureClearsStaleLastValue},
      {"offline passthrough pipeline audio quality",
       &TestOfflinePassthroughPipelineAudioQuality},
      {"stop interrupts blocked capture read",
       &TestStopInterruptsBlockedCaptureRead},
      {"stop interrupts blocked playback write",
       &TestStopInterruptsBlockedPlaybackWrite},
      // The tests below can end the run, with std::_Exit(1) on a wedge or
      // with std::terminate() on a double start, thus they come last: an end
      // in one of them must not hide the result of another test.
      {"concurrent pipeline stop joins the worker once",
       &TestConcurrentPipelineStopJoinsWorkerOnce},
      {"concurrent service stop joins the supervisor once",
       &TestConcurrentServiceStopJoinsSupervisorOnce},
      {"concurrent start/stop keeps the worker handle usable",
       &TestConcurrentStartStopKeepsWorkerHandleUsable},
      {"concurrent service start/stop keeps the supervisor handle usable",
       &TestConcurrentServiceStartStopKeepsSupervisorHandleUsable},
      {"stop keeps the stop flag up for the worker it joins",
       &TestStopKeepsTheStopFlagUpForTheWorkerItJoins},
      {"stop raises the stop flag under the worker lock",
       &TestStopRaisesTheStopFlagUnderTheWorkerLock},
      {"stop releases the backend under the worker lock",
       &TestStopReleasesTheBackendUnderTheWorkerLock},
      {"start joins the previous worker outside the worker lock",
       &TestStartJoinsThePreviousWorkerOutsideTheWorkerLock},
      {"concurrent pipeline start fails and keeps the process",
       &TestConcurrentPipelineStartFailsAndKeepsTheProcess},
      {"concurrent service start fails and keeps the process",
       &TestConcurrentServiceStartFailsAndKeepsTheProcess},
  };

  int failed = 0;
  for (const auto &test : tests) {
    std::optional<ScopedPactlExecHook> safe_mic_source;
    if (test.mock_safe_mic_source)
      safe_mic_source.emplace(SafeMicrophoneSourcePactlHook());
    const bool ok = test.fn();
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << test.name << "\n";
    if (!ok)
      ++failed;
  }

  return failed == 0 ? 0 : 1;
}
