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

#include "core/audio/audio_pipeline.h"
#include "core/audio/audio_processor.h"
#include "core/audio/virtual_audio_service.h"

namespace {

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
      : running_(running), last_error_(std::move(last_error)),
        stop_calls_(stop_calls) {}

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
    stats.running = running_;
    stats.last_error = last_error_;
    return stats;
  }

private:
  bool running_ = false;
  std::string last_error_;
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
