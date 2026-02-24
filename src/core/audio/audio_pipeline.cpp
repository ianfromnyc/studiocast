#include "core/audio/audio_pipeline.h"

#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <string>
#include <vector>

#include <pulse/error.h>
#include <pulse/simple.h>

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

} // namespace

AudioPipeline::AudioPipeline(AudioProcessor *processor)
    : processor_(processor) {}

AudioPipeline::~AudioPipeline() { Stop(); }

bool AudioPipeline::Start(const AudioPipelineConfig &cfg, std::string *error) {
  if (error)
    error->clear();

  if (running_.load(std::memory_order_acquire)) {
    if (error)
      *error = "Audio pipeline is already running.";
    return false;
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

  running_.store(true, std::memory_order_release);
  stop_.store(false, std::memory_order_release);
  thread_ = std::thread([this, cfg] { ThreadMain(cfg); });
  return true;
}

void AudioPipeline::Stop() {
  stop_.store(true, std::memory_order_release);
  if (thread_.joinable()) {
    thread_.join();
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

void AudioPipeline::ThreadMain(AudioPipelineConfig cfg) {
  struct Guard {
    AudioPipeline *self;
    ~Guard() { self->running_.store(false, std::memory_order_release); }
  } guard{this};

  const pa_sample_spec ss{
      .format = PA_SAMPLE_FLOAT32LE,
      .rate = static_cast<uint32_t>(cfg.sample_rate),
      .channels = static_cast<uint8_t>(cfg.channels),
  };
  if (!pa_sample_spec_valid(&ss)) {
    SetLastError("Invalid PulseAudio sample spec.");
    return;
  }

  const std::uint32_t samples_per_frame = cfg.frame_samples * cfg.channels;
  const std::size_t bytes_per_frame =
      static_cast<std::size_t>(samples_per_frame) * sizeof(float);

  // Frame budget for processor time (10ms at 48k).
  const std::uint64_t frame_budget_us =
      static_cast<std::uint64_t>(cfg.frame_samples) * 1000000u /
      static_cast<std::uint64_t>(cfg.sample_rate);

  pa_buffer_attr rec_attr{};
  // Keep capture buffering modest to avoid silently "living in the past".
  // The fragsize (read quantum) remains one frame (10ms).
  rec_attr.maxlength = static_cast<uint32_t>(bytes_per_frame * 4);
  rec_attr.fragsize = static_cast<uint32_t>(bytes_per_frame);
  rec_attr.tlength = static_cast<uint32_t>(-1);
  rec_attr.prebuf = static_cast<uint32_t>(-1);
  rec_attr.minreq = static_cast<uint32_t>(-1);

  pa_buffer_attr play_attr{};
  // Tighten playback buffering to reduce end-to-end latency and limit how much
  // queued audio can accumulate inside Pulse when StudioCast is under load.
  play_attr.maxlength = static_cast<uint32_t>(bytes_per_frame * 4);
  play_attr.tlength = static_cast<uint32_t>(bytes_per_frame * 3);
  play_attr.prebuf = static_cast<uint32_t>(bytes_per_frame);
  play_attr.minreq = static_cast<uint32_t>(bytes_per_frame);
  play_attr.fragsize = static_cast<uint32_t>(-1);

  int pa_err = 0;

  const char *capture_dev =
      cfg.source_name.empty() ? nullptr : cfg.source_name.c_str();
  const char *playback_dev =
      cfg.sink_name.empty() ? nullptr : cfg.sink_name.c_str();

  pa_simple *rec = ::pa_simple_new(
      /*server=*/nullptr,
      /*name=*/"studiocast",
      /*dir=*/PA_STREAM_RECORD,
      /*dev=*/capture_dev,
      /*stream_name=*/"studiocast-capture",
      /*ss=*/&ss,
      /*map=*/nullptr,
      /*attr=*/&rec_attr,
      /*error=*/&pa_err);
  if (!rec) {
    SetLastError("Pulse capture stream open failed: " +
                 PulseErrorString(pa_err));
    return;
  }

  pa_simple *play = ::pa_simple_new(
      /*server=*/nullptr,
      /*name=*/"studiocast",
      /*dir=*/PA_STREAM_PLAYBACK,
      /*dev=*/playback_dev,
      /*stream_name=*/"studiocast-playback",
      /*ss=*/&ss,
      /*map=*/nullptr,
      /*attr=*/&play_attr,
      /*error=*/&pa_err);
  if (!play) {
    SetLastError("Pulse playback stream open failed: " +
                 PulseErrorString(pa_err));
    ::pa_simple_free(rec);
    return;
  }

  std::vector<float> in(samples_per_frame);
  std::vector<float> out(samples_per_frame);

  // Flush any stale buffered audio so the pipeline starts as close to "live" as
  // possible. This helps keep speaker/mic transitions from playing old queued
  // audio after toggling effects.
  {
    int ferr = 0;
    (void)::pa_simple_flush(rec, &ferr);
    ferr = 0;
    (void)::pa_simple_flush(play, &ferr);
  }

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
      bool rec_ok = false;
      bool play_ok = false;

      {
        int lerr = 0;
        const pa_usec_t v = ::pa_simple_get_latency(rec, &lerr);
        if (v != static_cast<pa_usec_t>(-1)) {
          rec_lat_us = static_cast<std::uint64_t>(v);
          rec_ok = true;
          pulse_capture_latency_us_last_.store(rec_lat_us,
                                               std::memory_order_relaxed);
        }
      }

      {
        int lerr = 0;
        const pa_usec_t v = ::pa_simple_get_latency(play, &lerr);
        if (v != static_cast<pa_usec_t>(-1)) {
          play_lat_us = static_cast<std::uint64_t>(v);
          play_ok = true;
          pulse_playback_latency_us_last_.store(play_lat_us,
                                                std::memory_order_relaxed);
        }
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

        int ferr = 0;
        (void)::pa_simple_flush(rec, &ferr);
        ferr = 0;
        (void)::pa_simple_flush(play, &ferr);

        processor_->Reset();
        last_resync = now_check;

        SetLastError("Audio pipeline resync: latency " +
                     std::to_string(observed_us / 1000u) + "ms exceeded " +
                     std::to_string(max_latency_ms) +
                     "ms; flushed Pulse buffers.");
      }
    }

    if (::pa_simple_read(rec, in.data(), bytes_per_frame, &pa_err) < 0) {
      SetLastError("Pulse capture read failed: " + PulseErrorString(pa_err));
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

    if (::pa_simple_write(play, out.data(), bytes_per_frame, &pa_err) < 0) {
      SetLastError("Pulse playback write failed: " + PulseErrorString(pa_err));
      break;
    }

    frames_processed_.fetch_add(1, std::memory_order_relaxed);
  }

  // Do not drain on shutdown: dropping queued audio keeps route switches
  // responsive and avoids playing "late" buffered audio after disabling effects.
  (void)::pa_simple_flush(play, &pa_err);
  ::pa_simple_free(play);
  ::pa_simple_free(rec);
}

} // namespace studiocast::audio
