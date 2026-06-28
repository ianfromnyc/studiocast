#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "core/audio/audio_backend_resolver.h"
#include "core/audio/audio_pipeline.h"
#include "core/audio/audio_processor.h"
#include "core/audio/virtual_audio_service.h"

namespace {

using studiocast::audio::AudioBackendAvailability;
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

using namespace std::chrono_literals;

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

struct ResettingOpenIoState {
  std::mutex mu;
  std::condition_variable cv;
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

bool TestMicrophonePipelineRestartsWhenWorkerDies() {
  std::atomic<int> pipeline_creates{0};
  std::atomic<int> pipeline_stops{0};
  std::atomic<int> sleep_calls{0};

  VirtualAudioServiceHooks hooks;
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

  VirtualAudioServiceHooks hooks;
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

  VirtualAudioServiceHooks hooks;
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
  VirtualAudioServiceHooks hooks;
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

  VirtualAudioServiceHooks hooks;
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

  VirtualAudioServiceHooks hooks;
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

bool TestMicrophoneAvailabilityCacheIgnoresSpeakerOnlyChanges() {
  std::atomic<int> mic_probes{0};

  VirtualAudioServiceHooks hooks;
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

  VirtualAudioServiceHooks hooks;
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

  VirtualAudioServiceHooks hooks;
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

  VirtualAudioServiceHooks hooks;
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
  hooks.start_speaker_loopback =
      [&](const std::string &, int, std::string *error) {
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
              << loopback_start_calls.load() << " active="
              << status.speakers_routing_active << " route='"
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
  if (!pipeline.Start(cfg, &err)) {
    std::cerr << "pipeline.Start failed: " << err << "\n";
    return false;
  }

  auto stop_future = std::async(std::launch::async, [&] {
    pipeline.Stop();
    return true;
  });
  const bool stop_ready =
      (stop_future.wait_for(100ms) == std::future_status::ready);
  (void)stop_future.get();

  int request_stop_calls = 0;
  {
    std::lock_guard<std::mutex> lock(state->mu);
    request_stop_calls = state->request_stop_calls;
  }

  if (!stop_ready) {
    std::cerr << "Stop() stayed blocked while Open() reset an early stop\n";
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

bool TestStartReusesPipelineAfterWorkerExit() {
  std::atomic<int> open_calls{0};
  CopyProcessor processor;

  AudioPipelineHooks hooks;
  hooks.create_io = [&] { return std::make_unique<OpenFailIo>(&open_calls); };

  AudioPipeline pipeline(&processor, std::move(hooks));
  AudioPipelineConfig cfg;

  std::string err;
  if (!pipeline.Start(cfg, &err)) {
    std::cerr << "first pipeline.Start failed: " << err << "\n";
    return false;
  }

  if (!WaitUntil(
          [&] {
            return open_calls.load(std::memory_order_relaxed) >= 1 &&
                   !pipeline.GetStats().running;
          },
          250ms)) {
    std::cerr << "first worker did not exit after open failure\n";
    pipeline.Stop();
    return false;
  }

  if (!pipeline.Start(cfg, &err)) {
    std::cerr << "second pipeline.Start failed: " << err << "\n";
    return false;
  }

  if (!WaitUntil(
          [&] {
            return open_calls.load(std::memory_order_relaxed) >= 2 &&
                   !pipeline.GetStats().running;
          },
          250ms)) {
    std::cerr << "second worker did not exit after open failure\n";
    pipeline.Stop();
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

} // namespace

int main() {
  const struct {
    const char *name;
    bool (*fn)();
  } tests[] = {
      {"mic pipeline restarts after worker death",
       &TestMicrophonePipelineRestartsWhenWorkerDies},
      {"mic pipeline preserves worker death error",
       &TestMicrophonePipelinePreservesWorkerDeathError},
      {"status remains responsive during retry sleep",
       &TestStatusDoesNotBlockDuringRetrySleep},
      {"mic null pipeline factory fails without crash",
       &TestMicrophoneNullPipelineFactoryFailsWithoutCrash},
      {"speaker pipeline start failure clears route state",
       &TestSpeakerPipelineStartFailureClearsRouteState},
      {"open audio failure cooldown avoids restart churn",
       &TestOpenAudioFailureCooldownAvoidsRestartChurn},
      {"mic availability cache ignores speaker-only changes",
       &TestMicrophoneAvailabilityCacheIgnoresSpeakerOnlyChanges},
      {"speaker availability cache ignores microphone-only changes",
       &TestSpeakerAvailabilityCacheIgnoresMicrophoneOnlyChanges},
      {"mic dead worker backs off before restart",
       &TestMicrophoneDeadWorkerBacksOffBeforeRestart},
      {"speaker dead worker backs off and clears route",
       &TestSpeakerDeadWorkerBacksOffAndClearsRoute},
      {"speaker pipeline stats clear when processing disabled",
       &TestSpeakerPipelineStatsClearWhenProcessingDisabled},
      {"speaker loopback restart failure clears route state",
       &TestSpeakerLoopbackRestartFailureClearsRouteState},
      {"stop interrupts open after early stop reset",
       &TestStopInterruptsOpenAfterEarlyStopReset},
      {"stop interrupts blocked flush", &TestStopInterruptsBlockedFlush},
      {"start reuses pipeline after worker exit",
       &TestStartReusesPipelineAfterWorkerExit},
      {"pipeline surfaces capture disconnect",
       &TestPipelineSurfacesCaptureDisconnectError},
      {"latency guard sums capture and playback before resync",
       &TestLatencyGuardSumsCaptureAndPlaybackBeforeResync},
      {"stop interrupts blocked capture read",
       &TestStopInterruptsBlockedCaptureRead},
      {"stop interrupts blocked playback write",
       &TestStopInterruptsBlockedPlaybackWrite},
  };

  int failed = 0;
  for (const auto &test : tests) {
    const bool ok = test.fn();
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << test.name << "\n";
    if (!ok)
      ++failed;
  }

  return failed == 0 ? 0 : 1;
}
