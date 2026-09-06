#include "core/audio/audio_pipeline.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <pulse/context.h>
#include <pulse/error.h>
#include <pulse/operation.h>
#include <pulse/stream.h>
#include <pulse/thread-mainloop.h>

#include "core/audio/audio_device_safety.h"
#include "core/audio/audio_processor.h"

namespace studiocast::audio {

namespace {

std::string PulseErrorString(int pa_error) {
  const char *s = ::pa_strerror(pa_error);
  if (!s || *s == '\0') {
    return "PulseAudio error " + std::to_string(pa_error);
  }
  return std::string(s);
}

bool DebugAudioStats() {
  static const bool enabled =
      (std::getenv("STUDIOCAST_DEBUG_AUDIO_STATS") != nullptr);
  return enabled;
}

std::uint64_t ParseEnvU64(const char *name, std::uint64_t fallback) {
  if (!name)
    return fallback;
  const char *env = std::getenv(name);
  if (!env || *env == '\0')
    return fallback;
  char *end = nullptr;
  const unsigned long long v = std::strtoull(env, &end, 10);
  if (!end || *end != '\0')
    return fallback;
  return static_cast<std::uint64_t>(v);
}

void AtomicMax(std::atomic<std::uint64_t> *v, std::uint64_t candidate) {
  if (!v)
    return;
  std::uint64_t cur = v->load(std::memory_order_relaxed);
  while (candidate > cur &&
         !v->compare_exchange_weak(cur, candidate, std::memory_order_relaxed)) {
    // cur is updated with the latest value.
  }
}

class PulseAsyncAudioIo final : public AudioPipelineIo {
public:
  ~PulseAsyncAudioIo() override { Shutdown(); }

  void SetStopRequestedFlag(const std::atomic<bool> *stop_requested) override {
    external_stop_requested_ = stop_requested;
  }

  bool Open(const AudioPipelineConfig &cfg, std::string *error) override {
    if (error)
      error->clear();

    Shutdown();
    if (ExternalStopRequested()) {
      return false;
    }
    stop_requested_.store(false, std::memory_order_release);
    if (ExternalStopRequested()) {
      return false;
    }
    capture_pending_.clear();
    capture_pending_size_ = 0;
    capture_pending_offset_ = 0;
    pending_silence_bytes_ = 0;

    const pa_sample_spec ss{
        .format = PA_SAMPLE_FLOAT32LE,
        .rate = static_cast<uint32_t>(cfg.sample_rate),
        .channels = static_cast<uint8_t>(cfg.channels),
    };
    if (!pa_sample_spec_valid(&ss)) {
      if (error)
        *error = "Invalid PulseAudio sample spec.";
      return false;
    }

    const std::uint32_t samples_per_frame = cfg.frame_samples * cfg.channels;
    const std::size_t bytes_per_frame =
        static_cast<std::size_t>(samples_per_frame) * sizeof(float);

    pa_buffer_attr rec_attr{};
    rec_attr.maxlength = static_cast<uint32_t>(bytes_per_frame * 4);
    rec_attr.fragsize = static_cast<uint32_t>(bytes_per_frame);
    rec_attr.tlength = static_cast<uint32_t>(-1);
    rec_attr.prebuf = static_cast<uint32_t>(-1);
    rec_attr.minreq = static_cast<uint32_t>(-1);

    pa_buffer_attr play_attr{};
    play_attr.maxlength = static_cast<uint32_t>(bytes_per_frame * 4);
    play_attr.tlength = static_cast<uint32_t>(bytes_per_frame * 3);
    play_attr.prebuf = static_cast<uint32_t>(bytes_per_frame);
    play_attr.minreq = static_cast<uint32_t>(bytes_per_frame);
    play_attr.fragsize = static_cast<uint32_t>(-1);

    std::string resolved_capture_dev = cfg.source_name;
    if (!cfg.allow_monitor_source) {
      const auto sourceResolution = ResolveSafeInputSourceName(cfg.source_name);
      if (!sourceResolution.ok) {
        if (error) {
          *error = sourceResolution.error.empty()
                       ? "Failed to resolve a safe Pulse capture source."
                       : sourceResolution.error;
        }
        return false;
      }
      resolved_capture_dev = sourceResolution.source_name;
    }
    const char *capture_dev =
        resolved_capture_dev.empty() ? nullptr : resolved_capture_dev.c_str();
    const char *playback_dev =
        cfg.sink_name.empty() ? nullptr : cfg.sink_name.c_str();

    mainloop_ = ::pa_threaded_mainloop_new();
    if (!mainloop_) {
      if (error)
        *error = "PulseAudio threaded mainloop allocation failed.";
      return false;
    }
    if (::pa_threaded_mainloop_start(mainloop_) < 0) {
      if (error)
        *error = "PulseAudio threaded mainloop start failed.";
      ::pa_threaded_mainloop_free(mainloop_);
      mainloop_ = nullptr;
      return false;
    }

    ::pa_threaded_mainloop_lock(mainloop_);

    context_ = ::pa_context_new(::pa_threaded_mainloop_get_api(mainloop_),
                                "studiocast");
    if (!context_) {
      if (error)
        *error = "PulseAudio context allocation failed.";
      ::pa_threaded_mainloop_unlock(mainloop_);
      Shutdown();
      return false;
    }

    ::pa_context_set_state_callback(context_,
                                    &PulseAsyncAudioIo::ContextStateCb, this);
    if (::pa_context_connect(context_, nullptr, PA_CONTEXT_NOFLAGS, nullptr) <
        0) {
      if (error) {
        *error = "PulseAudio context connect failed: " + ContextErrorLocked();
      }
      ::pa_threaded_mainloop_unlock(mainloop_);
      Shutdown();
      return false;
    }
    if (!WaitForContextReadyLocked(error)) {
      ::pa_threaded_mainloop_unlock(mainloop_);
      Shutdown();
      return false;
    }

    rec_ = ::pa_stream_new(context_, "studiocast-capture", &ss, nullptr);
    if (!rec_) {
      if (error)
        *error = "Pulse capture stream allocation failed.";
      ::pa_threaded_mainloop_unlock(mainloop_);
      Shutdown();
      return false;
    }
    PrepareStream(rec_);
    const auto stream_flags = static_cast<pa_stream_flags_t>(
        PA_STREAM_ADJUST_LATENCY | PA_STREAM_AUTO_TIMING_UPDATE |
        PA_STREAM_INTERPOLATE_TIMING);
    if (::pa_stream_connect_record(rec_, capture_dev, &rec_attr, stream_flags) <
        0) {
      if (error) {
        *error =
            "Pulse capture stream connect failed: " + StreamErrorLocked(rec_);
      }
      ::pa_threaded_mainloop_unlock(mainloop_);
      Shutdown();
      return false;
    }
    if (!WaitForStreamReadyLocked(rec_, "capture", error)) {
      ::pa_threaded_mainloop_unlock(mainloop_);
      Shutdown();
      return false;
    }

    play_ = ::pa_stream_new(context_, "studiocast-playback", &ss, nullptr);
    if (!play_) {
      if (error)
        *error = "Pulse playback stream allocation failed.";
      ::pa_threaded_mainloop_unlock(mainloop_);
      Shutdown();
      return false;
    }
    PrepareStream(play_);
    if (::pa_stream_connect_playback(play_, playback_dev, &play_attr,
                                     stream_flags, nullptr, nullptr) < 0) {
      if (error) {
        *error =
            "Pulse playback stream connect failed: " + StreamErrorLocked(play_);
      }
      ::pa_threaded_mainloop_unlock(mainloop_);
      Shutdown();
      return false;
    }
    if (!WaitForStreamReadyLocked(play_, "playback", error)) {
      ::pa_threaded_mainloop_unlock(mainloop_);
      Shutdown();
      return false;
    }

    capture_pending_.resize(rec_attr.maxlength);
    capture_pending_size_ = 0;
    ::pa_threaded_mainloop_unlock(mainloop_);
    return true;
  }

  bool Read(void *dst, std::size_t bytes, std::string *error) override {
    auto *dst_bytes = static_cast<std::uint8_t *>(dst);
    std::size_t copied = 0;

    ::pa_threaded_mainloop_lock(mainloop_);
    while (copied < bytes) {
      if (ShouldStop()) {
        if (error)
          error->clear();
        ::pa_threaded_mainloop_unlock(mainloop_);
        return false;
      }

      const std::size_t pending_bytes =
          capture_pending_size_ - capture_pending_offset_;
      if (pending_bytes > 0) {
        const std::size_t chunk = std::min(bytes - copied, pending_bytes);
        std::memcpy(dst_bytes + copied,
                    capture_pending_.data() + capture_pending_offset_, chunk);
        copied += chunk;
        capture_pending_offset_ += chunk;
        if (capture_pending_offset_ == capture_pending_size_) {
          capture_pending_size_ = 0;
          capture_pending_offset_ = 0;
        }
        continue;
      }

      if (pending_silence_bytes_ > 0) {
        const std::size_t chunk =
            std::min(bytes - copied, pending_silence_bytes_);
        std::memset(dst_bytes + copied, 0, chunk);
        copied += chunk;
        pending_silence_bytes_ -= chunk;
        continue;
      }

      if (!WaitForReadableLocked(error)) {
        ::pa_threaded_mainloop_unlock(mainloop_);
        return false;
      }

      const void *data = nullptr;
      std::size_t nbytes = 0;
      if (::pa_stream_peek(rec_, &data, &nbytes) < 0) {
        if (error) {
          *error = "Pulse capture read failed: " + StreamErrorLocked(rec_);
        }
        ::pa_threaded_mainloop_unlock(mainloop_);
        return false;
      }

      if (nbytes == 0) {
        continue;
      }

      const std::size_t chunk = std::min(bytes - copied, nbytes);
      if (data) {
        std::memcpy(dst_bytes + copied, data, chunk);
        if (nbytes > chunk) {
          const std::size_t remaining = nbytes - chunk;
          if (remaining > capture_pending_.size()) {
            if (error) {
              *error = "Pulse capture read exceeded reserved buffer.";
            }
            (void)::pa_stream_drop(rec_);
            ::pa_threaded_mainloop_unlock(mainloop_);
            return false;
          }
          std::memcpy(capture_pending_.data(),
                      static_cast<const std::uint8_t *>(data) + chunk,
                      remaining);
          capture_pending_size_ = remaining;
          capture_pending_offset_ = 0;
        }
      } else {
        std::memset(dst_bytes + copied, 0, chunk);
        if (nbytes > chunk) {
          pending_silence_bytes_ = nbytes - chunk;
        }
      }
      copied += chunk;

      if (::pa_stream_drop(rec_) < 0) {
        if (error) {
          *error = "Pulse capture read failed: " + StreamErrorLocked(rec_);
        }
        ::pa_threaded_mainloop_unlock(mainloop_);
        return false;
      }
    }

    ::pa_threaded_mainloop_unlock(mainloop_);
    return true;
  }

  bool Write(const void *src, std::size_t bytes, std::string *error) override {
    auto *src_bytes = static_cast<const std::uint8_t *>(src);
    std::size_t written = 0;

    ::pa_threaded_mainloop_lock(mainloop_);
    while (written < bytes) {
      std::size_t writable = 0;
      if (!WaitForWritableLocked(&writable, error)) {
        ::pa_threaded_mainloop_unlock(mainloop_);
        return false;
      }

      const std::size_t chunk = std::min(bytes - written, writable);
      if (chunk == 0) {
        ::pa_threaded_mainloop_wait(mainloop_);
        continue;
      }

      if (::pa_stream_write(play_, src_bytes + written, chunk, nullptr, 0,
                            PA_SEEK_RELATIVE) < 0) {
        if (error) {
          *error = "Pulse playback write failed: " + StreamErrorLocked(play_);
        }
        ::pa_threaded_mainloop_unlock(mainloop_);
        return false;
      }
      written += chunk;
    }

    ::pa_threaded_mainloop_unlock(mainloop_);
    return true;
  }

  bool GetCaptureLatencyUs(std::uint64_t *latency_us) override {
    return QueryLatency(rec_, latency_us);
  }

  bool GetPlaybackLatencyUs(std::uint64_t *latency_us) override {
    return QueryLatency(play_, latency_us);
  }

  void Flush() override {
    if (!mainloop_)
      return;

    ::pa_threaded_mainloop_lock(mainloop_);
    capture_pending_size_ = 0;
    capture_pending_offset_ = 0;
    pending_silence_bytes_ = 0;
    FlushStreamLocked(rec_);
    FlushStreamLocked(play_);
    ::pa_threaded_mainloop_unlock(mainloop_);
  }

  void RequestStop() override {
    stop_requested_.store(true, std::memory_order_release);
    if (!mainloop_)
      return;

    ::pa_threaded_mainloop_lock(mainloop_);
    if (play_) {
      (void)::pa_stream_disconnect(play_);
    }
    if (rec_) {
      (void)::pa_stream_disconnect(rec_);
    }
    if (context_) {
      ::pa_context_disconnect(context_);
    }
    ::pa_threaded_mainloop_signal(mainloop_, 0);
    ::pa_threaded_mainloop_unlock(mainloop_);
  }

private:
  struct OperationState {
    PulseAsyncAudioIo *self = nullptr;
    bool done = false;
  };

  static void ContextStateCb(pa_context *, void *userdata) {
    static_cast<PulseAsyncAudioIo *>(userdata)->SignalLocked();
  }

  static void StreamStateCb(pa_stream *, void *userdata) {
    static_cast<PulseAsyncAudioIo *>(userdata)->SignalLocked();
  }

  static void StreamRequestCb(pa_stream *, std::size_t, void *userdata) {
    static_cast<PulseAsyncAudioIo *>(userdata)->SignalLocked();
  }

  static void StreamLatencyCb(pa_stream *, void *userdata) {
    static_cast<PulseAsyncAudioIo *>(userdata)->SignalLocked();
  }

  static void StreamOperationCb(pa_stream *, int, void *userdata) {
    auto *state = static_cast<OperationState *>(userdata);
    state->done = true;
    state->self->SignalLocked();
  }

  void PrepareStream(pa_stream *stream) {
    ::pa_stream_set_state_callback(stream, &PulseAsyncAudioIo::StreamStateCb,
                                   this);
    ::pa_stream_set_read_callback(stream, &PulseAsyncAudioIo::StreamRequestCb,
                                  this);
    ::pa_stream_set_write_callback(stream, &PulseAsyncAudioIo::StreamRequestCb,
                                   this);
    ::pa_stream_set_latency_update_callback(
        stream, &PulseAsyncAudioIo::StreamLatencyCb, this);
  }

  bool WaitForContextReadyLocked(std::string *error) {
    while (true) {
      if (ShouldStop()) {
        if (error)
          error->clear();
        return false;
      }

      switch (::pa_context_get_state(context_)) {
      case PA_CONTEXT_READY:
        return true;
      case PA_CONTEXT_FAILED:
      case PA_CONTEXT_TERMINATED:
        if (error) {
          *error = "PulseAudio context connect failed: " + ContextErrorLocked();
        }
        return false;
      default:
        ::pa_threaded_mainloop_wait(mainloop_);
        break;
      }
    }
  }

  bool WaitForStreamReadyLocked(pa_stream *stream, const char *label,
                                std::string *error) {
    while (true) {
      if (ShouldStop()) {
        if (error)
          error->clear();
        return false;
      }

      switch (::pa_stream_get_state(stream)) {
      case PA_STREAM_READY:
        return true;
      case PA_STREAM_FAILED:
      case PA_STREAM_TERMINATED:
        if (error) {
          *error = "Pulse " + std::string(label) +
                   " stream connect failed: " + StreamErrorLocked(stream);
        }
        return false;
      default:
        ::pa_threaded_mainloop_wait(mainloop_);
        break;
      }
    }
  }

  bool WaitForReadableLocked(std::string *error) {
    while (true) {
      if (ShouldStop()) {
        if (error)
          error->clear();
        return false;
      }

      if (::pa_stream_get_state(rec_) != PA_STREAM_READY) {
        if (error) {
          *error = "Pulse capture read failed: " + StreamErrorLocked(rec_);
        }
        return false;
      }

      const std::size_t readable = ::pa_stream_readable_size(rec_);
      if (readable == static_cast<std::size_t>(-1)) {
        if (error) {
          *error = "Pulse capture read failed: " + StreamErrorLocked(rec_);
        }
        return false;
      }
      if (readable > 0) {
        return true;
      }

      ::pa_threaded_mainloop_wait(mainloop_);
    }
  }

  bool WaitForWritableLocked(std::size_t *writable_out, std::string *error) {
    while (true) {
      if (ShouldStop()) {
        if (error)
          error->clear();
        return false;
      }

      if (::pa_stream_get_state(play_) != PA_STREAM_READY) {
        if (error) {
          *error = "Pulse playback write failed: " + StreamErrorLocked(play_);
        }
        return false;
      }

      const std::size_t writable = ::pa_stream_writable_size(play_);
      if (writable == static_cast<std::size_t>(-1)) {
        if (error) {
          *error = "Pulse playback write failed: " + StreamErrorLocked(play_);
        }
        return false;
      }
      if (writable > 0) {
        if (writable_out) {
          *writable_out = writable;
        }
        return true;
      }

      ::pa_threaded_mainloop_wait(mainloop_);
    }
  }

  bool QueryLatency(pa_stream *stream, std::uint64_t *latency_us) {
    if (!mainloop_ || !stream || !latency_us)
      return false;

    ::pa_threaded_mainloop_lock(mainloop_);
    if (::pa_stream_get_state(stream) != PA_STREAM_READY || ShouldStop()) {
      ::pa_threaded_mainloop_unlock(mainloop_);
      return false;
    }

    pa_usec_t latency = 0;
    int negative = 0;
    const int rc = ::pa_stream_get_latency(stream, &latency, &negative);
    ::pa_threaded_mainloop_unlock(mainloop_);
    if (rc < 0) {
      return false;
    }

    *latency_us = negative ? 0u : static_cast<std::uint64_t>(latency);
    return true;
  }

  void FlushStreamLocked(pa_stream *stream) {
    if (!stream || ShouldStop() ||
        ::pa_stream_get_state(stream) != PA_STREAM_READY)
      return;

    OperationState state{this};
    pa_operation *op = ::pa_stream_flush(
        stream, &PulseAsyncAudioIo::StreamOperationCb, &state);
    if (!op)
      return;

    while (!state.done && !ShouldStop() &&
           ::pa_stream_get_state(stream) == PA_STREAM_READY) {
      ::pa_threaded_mainloop_wait(mainloop_);
    }
    ::pa_operation_unref(op);
  }

  std::string ContextErrorLocked() const {
    if (!context_) {
      return "connection state is unavailable";
    }
    return PulseErrorString(::pa_context_errno(context_));
  }

  std::string StreamErrorLocked(pa_stream *stream) const {
    if (!stream) {
      return ContextErrorLocked();
    }
    pa_context *stream_context = ::pa_stream_get_context(stream);
    if (!stream_context) {
      return ContextErrorLocked();
    }
    return PulseErrorString(::pa_context_errno(stream_context));
  }

  void SignalLocked() {
    if (mainloop_) {
      ::pa_threaded_mainloop_signal(mainloop_, 0);
    }
  }

  bool ExternalStopRequested() const {
    return external_stop_requested_ &&
           external_stop_requested_->load(std::memory_order_acquire);
  }

  bool ShouldStop() const {
    return stop_requested_.load(std::memory_order_acquire) ||
           ExternalStopRequested();
  }

  void Shutdown() {
    if (!mainloop_)
      return;

    ::pa_threaded_mainloop_lock(mainloop_);
    stop_requested_.store(true, std::memory_order_release);

    if (play_) {
      ::pa_stream_set_state_callback(play_, nullptr, nullptr);
      ::pa_stream_set_read_callback(play_, nullptr, nullptr);
      ::pa_stream_set_write_callback(play_, nullptr, nullptr);
      ::pa_stream_set_latency_update_callback(play_, nullptr, nullptr);
      (void)::pa_stream_disconnect(play_);
      ::pa_stream_unref(play_);
      play_ = nullptr;
    }
    if (rec_) {
      ::pa_stream_set_state_callback(rec_, nullptr, nullptr);
      ::pa_stream_set_read_callback(rec_, nullptr, nullptr);
      ::pa_stream_set_write_callback(rec_, nullptr, nullptr);
      ::pa_stream_set_latency_update_callback(rec_, nullptr, nullptr);
      (void)::pa_stream_disconnect(rec_);
      ::pa_stream_unref(rec_);
      rec_ = nullptr;
    }
    if (context_) {
      ::pa_context_set_state_callback(context_, nullptr, nullptr);
      ::pa_context_disconnect(context_);
      ::pa_context_unref(context_);
      context_ = nullptr;
    }

    ::pa_threaded_mainloop_signal(mainloop_, 0);
    ::pa_threaded_mainloop_unlock(mainloop_);
    ::pa_threaded_mainloop_stop(mainloop_);
    ::pa_threaded_mainloop_free(mainloop_);
    mainloop_ = nullptr;

    capture_pending_.clear();
    capture_pending_size_ = 0;
    capture_pending_offset_ = 0;
    pending_silence_bytes_ = 0;
  }

  pa_threaded_mainloop *mainloop_ = nullptr;
  pa_context *context_ = nullptr;
  pa_stream *rec_ = nullptr;
  pa_stream *play_ = nullptr;
  const std::atomic<bool> *external_stop_requested_ = nullptr;
  std::atomic<bool> stop_requested_{false};
  std::vector<std::uint8_t> capture_pending_;
  std::size_t capture_pending_size_ = 0;
  std::size_t capture_pending_offset_ = 0;
  std::size_t pending_silence_bytes_ = 0;
};

} // namespace

AudioPipeline::AudioPipeline(AudioProcessor *processor,
                             AudioPipelineHooks hooks)
    : processor_(processor), hooks_(std::move(hooks)) {}

AudioPipeline::~AudioPipeline() { Stop(); }

bool AudioPipeline::Start(const AudioPipelineConfig &cfg, std::string *error) {
  if (error)
    error->clear();

  if (running_.load(std::memory_order_acquire)) {
    if (error)
      *error = "Audio pipeline is already running.";
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(thread_mu_);
    if (thread_.joinable()) {
      thread_.join();
    }
  }

  // Reset stats.
  frames_processed_.store(0, std::memory_order_relaxed);
  process_time_us_sum_.store(0, std::memory_order_relaxed);
  process_time_us_max_.store(0, std::memory_order_relaxed);
  process_time_us_last_.store(0, std::memory_order_relaxed);
  process_overruns_.store(0, std::memory_order_relaxed);
  pulse_capture_latency_us_last_.store(0, std::memory_order_relaxed);
  pulse_playback_latency_us_last_.store(0, std::memory_order_relaxed);
  pulse_latency_us_max_.store(0, std::memory_order_relaxed);
  resync_events_.store(0, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(mu_);
    last_error_.clear();
  }

  if (!processor_) {
    SetLastError("Audio pipeline processor is null.");
    if (error)
      *error = GetStats().last_error;
    return false;
  }
  if (cfg.sample_rate != 48000 || (cfg.channels != 1 && cfg.channels != 2)) {
    SetLastError("Unsupported audio format: requires 48kHz float32 with 1 or 2 "
                 "channels (use Pulse to resample/downmix).");
    if (error)
      *error = GetStats().last_error;
    return false;
  }
  if (cfg.frame_samples != 480) {
    SetLastError(
        "Unsupported frame size: MVP requires 480 samples (10ms @ 48kHz).");
    if (error)
      *error = GetStats().last_error;
    return false;
  }

  stop_.store(false, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lock(startup_mu_);
    startup_complete_ = false;
    startup_ok_ = false;
    startup_error_.clear();
  }
  {
    std::lock_guard<std::mutex> lock(io_mu_);
    io_ = CreateIo();
    if (!io_) {
      SetLastError("Audio pipeline I/O backend creation failed.");
      if (error)
        *error = GetStats().last_error;
      return false;
    }
    io_->SetStopRequestedFlag(&stop_);
  }

  running_.store(true, std::memory_order_release);
  {
    // Start the worker outside the lock so ThreadMain() never waits on it,
    // then publish the handle under the lock.
    std::thread worker([this, cfg] { ThreadMain(cfg); });
    std::lock_guard<std::mutex> lock(thread_mu_);
    thread_ = std::move(worker);
  }

  std::unique_lock<std::mutex> startup_lock(startup_mu_);
  startup_cv_.wait(startup_lock, [this] { return startup_complete_; });
  const bool startup_ok = startup_ok_;
  std::string startup_error = startup_error_;
  startup_lock.unlock();

  if (!startup_ok) {
    if (error)
      *error = std::move(startup_error);
    return false;
  }
  return true;
}

void AudioPipeline::Stop() {
  stop_.store(true, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lock(io_mu_);
    if (io_) {
      io_->RequestStop();
    }
  }
  // Hold the worker lock across the join and the I/O release. A second Stop()
  // caller then waits here instead of joining the same worker a second time,
  // and it cannot release the I/O backend while the worker still uses it.
  std::lock_guard<std::mutex> thread_lock(thread_mu_);
  if (thread_.joinable()) {
    thread_.join();
  }
  {
    std::lock_guard<std::mutex> lock(io_mu_);
    io_.reset();
  }
  running_.store(false, std::memory_order_release);
}

AudioPipelineStats AudioPipeline::GetStats() const {
  AudioPipelineStats out;
  out.running = running_.load(std::memory_order_acquire);
  out.frames_processed = frames_processed_.load(std::memory_order_relaxed);
  out.process_time_us_sum =
      process_time_us_sum_.load(std::memory_order_relaxed);
  out.process_time_us_max =
      process_time_us_max_.load(std::memory_order_relaxed);
  out.process_time_us_last =
      process_time_us_last_.load(std::memory_order_relaxed);
  out.process_overruns = process_overruns_.load(std::memory_order_relaxed);
  out.pulse_capture_latency_us_last =
      pulse_capture_latency_us_last_.load(std::memory_order_relaxed);
  out.pulse_playback_latency_us_last =
      pulse_playback_latency_us_last_.load(std::memory_order_relaxed);
  out.pulse_latency_us_max =
      pulse_latency_us_max_.load(std::memory_order_relaxed);
  out.resync_events = resync_events_.load(std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(mu_);
    out.last_error = last_error_;
  }
  return out;
}

void AudioPipeline::SetLastError(std::string msg) {
  std::lock_guard<std::mutex> lock(mu_);
  last_error_ = std::move(msg);
}

void AudioPipeline::CompleteStartup(bool ok, std::string error) {
  if (!ok) {
    running_.store(false, std::memory_order_release);
  }

  {
    std::lock_guard<std::mutex> lock(startup_mu_);
    startup_ok_ = ok;
    startup_error_ = std::move(error);
    startup_complete_ = true;
  }
  startup_cv_.notify_all();
}

std::unique_ptr<AudioPipelineIo> AudioPipeline::CreateIo() const {
  if (hooks_.create_io) {
    return hooks_.create_io();
  }
  return std::make_unique<PulseAsyncAudioIo>();
}

AudioPipelineIo *AudioPipeline::GetActiveIo() const {
  std::lock_guard<std::mutex> lock(io_mu_);
  return io_.get();
}

void AudioPipeline::ThreadMain(AudioPipelineConfig cfg) {
  struct Guard {
    AudioPipeline *self;
    ~Guard() { self->running_.store(false, std::memory_order_release); }
  } guard{this};

  const std::uint32_t samples_per_frame = cfg.frame_samples * cfg.channels;
  const std::size_t bytes_per_frame =
      static_cast<std::size_t>(samples_per_frame) * sizeof(float);

  // Frame budget for processor time (10ms at 48k).
  const std::uint64_t frame_budget_us =
      static_cast<std::uint64_t>(cfg.frame_samples) * 1000000u /
      static_cast<std::uint64_t>(cfg.sample_rate);

  AudioPipelineIo *io = GetActiveIo();
  if (!io) {
    const std::string startup_error =
        "Audio pipeline I/O backend is not available.";
    SetLastError(startup_error);
    CompleteStartup(false, startup_error);
    return;
  }

  std::string io_err;
  if (!io->Open(cfg, &io_err)) {
    if (!stop_.load(std::memory_order_acquire) && !io_err.empty()) {
      SetLastError(io_err);
    }
    CompleteStartup(false, std::move(io_err));
    return;
  }
  CompleteStartup(true, {});

  std::vector<float> in(samples_per_frame);
  std::vector<float> out(samples_per_frame);

  // Flush any stale buffered audio so the pipeline starts as close to "live" as
  // possible. This helps keep speaker/mic transitions from playing old queued
  // audio after toggling effects.
  io->Flush();

  processor_->Reset();

  const bool debug_stats = DebugAudioStats();

  // Latency guardrail to prevent audio from accumulating unbounded buffering
  // when the pipeline falls behind (a primary cause of perceived A/V drift).
  //
  // When the reported Pulse stream latency exceeds a threshold, we flush the
  // Pulse buffers and reset the processor state. This is a best-effort
  // "stay live" policy analogous to the video pipeline's frame dropping.
  std::uint64_t max_latency_ms =
      ParseEnvU64("STUDIOCAST_AUDIO_MAX_LATENCY_MS", 150u);
  if (max_latency_ms < 50u)
    max_latency_ms = 50u;
  const std::uint64_t max_latency_us = max_latency_ms * 1000u;

  const auto latency_check_interval =
      std::chrono::milliseconds(debug_stats ? 250 : 1000);
  auto next_latency_check =
      std::chrono::steady_clock::now() + latency_check_interval;
  auto last_resync = std::chrono::steady_clock::time_point{};
  const auto resync_cooldown = std::chrono::milliseconds(1000);

  std::string proc_err;
  std::string last_proc_warning;
  while (!stop_.load(std::memory_order_acquire)) {
    const auto now_check = std::chrono::steady_clock::now();
    if (now_check >= next_latency_check) {
      next_latency_check = now_check + latency_check_interval;

      std::uint64_t rec_lat_us = 0;
      std::uint64_t play_lat_us = 0;
      const bool rec_ok = io->GetCaptureLatencyUs(&rec_lat_us);
      const bool play_ok = io->GetPlaybackLatencyUs(&play_lat_us);
      if (rec_ok) {
        pulse_capture_latency_us_last_.store(rec_lat_us,
                                             std::memory_order_relaxed);
      } else {
        pulse_capture_latency_us_last_.store(0, std::memory_order_relaxed);
      }
      if (play_ok) {
        pulse_playback_latency_us_last_.store(play_lat_us,
                                              std::memory_order_relaxed);
      } else {
        pulse_playback_latency_us_last_.store(0, std::memory_order_relaxed);
      }

      // Best-effort end-to-end estimate.
      // If both sides report latency, treat total latency as capture +
      // playback. Otherwise fall back to whichever side is available.
      std::uint64_t observed_us = 0;
      if (rec_ok && play_ok) {
        observed_us = rec_lat_us + play_lat_us;
      } else if (rec_ok) {
        observed_us = rec_lat_us;
      } else if (play_ok) {
        observed_us = play_lat_us;
      }

      if (observed_us) {
        AtomicMax(&pulse_latency_us_max_, observed_us);
      }

      const bool warmed_up =
          frames_processed_.load(std::memory_order_relaxed) > 20u;
      const bool cooldown_ok =
          (last_resync == std::chrono::steady_clock::time_point{}) ||
          (now_check - last_resync) >= resync_cooldown;

      if (observed_us > max_latency_us && warmed_up && cooldown_ok) {
        resync_events_.fetch_add(1, std::memory_order_relaxed);

        io->Flush();
        processor_->Reset();
        last_resync = now_check;

        SetLastError("Audio pipeline resync: latency " +
                     std::to_string(observed_us / 1000u) + "ms exceeded " +
                     std::to_string(max_latency_ms) +
                     "ms; flushed Pulse buffers.");
      }
    }

    io_err.clear();
    if (!io->Read(in.data(), bytes_per_frame, &io_err)) {
      if (!stop_.load(std::memory_order_acquire) && !io_err.empty()) {
        SetLastError(std::move(io_err));
      }
      break;
    }

    const auto t0 = std::chrono::steady_clock::now();
    proc_err.clear();
    const bool ok = processor_->Process(
        in.data(), out.data(), cfg.frame_samples, cfg.channels, &proc_err);
    const auto t1 = std::chrono::steady_clock::now();

    const std::uint64_t proc_us = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
    process_time_us_sum_.fetch_add(proc_us, std::memory_order_relaxed);
    process_time_us_last_.store(proc_us, std::memory_order_relaxed);
    AtomicMax(&process_time_us_max_, proc_us);
    if (proc_us > frame_budget_us) {
      process_overruns_.fetch_add(1, std::memory_order_relaxed);
    }

    if (!ok) {
      SetLastError("AudioProcessor::Process failed: " + proc_err);
      break;
    }

    if (!proc_err.empty() && proc_err != last_proc_warning) {
      SetLastError("AudioProcessor warning: " + proc_err);
      last_proc_warning = proc_err;
    }

    io_err.clear();
    if (!io->Write(out.data(), bytes_per_frame, &io_err)) {
      if (!stop_.load(std::memory_order_acquire) && !io_err.empty()) {
        SetLastError(std::move(io_err));
      }
      break;
    }

    frames_processed_.fetch_add(1, std::memory_order_relaxed);
  }

  // Do not drain on shutdown: dropping queued audio keeps route switches
  // responsive and avoids playing "late" buffered audio after disabling
  // effects.
  io->Flush();
}

} // namespace studiocast::audio
