#include "core/audio/audio_pipeline.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

#include <pulse/error.h>
#include <pulse/simple.h>

#include "core/audio/audio_processor.h"

namespace studiocast::audio {

namespace {

std::string PulseErrorString(int pa_error) {
    const char* s = ::pa_strerror(pa_error);
    if (!s || *s == '\0') {
        return "PulseAudio error " + std::to_string(pa_error);
    }
    return std::string(s);
}

void AtomicMax(std::atomic<std::uint64_t>* v, std::uint64_t candidate) {
    if (!v) return;
    std::uint64_t cur = v->load(std::memory_order_relaxed);
    while (candidate > cur && !v->compare_exchange_weak(cur, candidate, std::memory_order_relaxed)) {
        // cur is updated with the latest value.
    }
}

}  // namespace

AudioPipeline::AudioPipeline(AudioProcessor* processor) : processor_(processor) {}

AudioPipeline::~AudioPipeline() { Stop(); }

bool AudioPipeline::Start(const AudioPipelineConfig& cfg, std::string* error) {
    if (error) error->clear();

    if (running_.load(std::memory_order_acquire)) {
        if (error) *error = "Audio pipeline is already running.";
        return false;
    }

    // Reset stats.
    frames_processed_.store(0, std::memory_order_relaxed);
    process_time_us_sum_.store(0, std::memory_order_relaxed);
    process_time_us_max_.store(0, std::memory_order_relaxed);
    process_time_us_last_.store(0, std::memory_order_relaxed);
    process_overruns_.store(0, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(mu_);
        last_error_.clear();
    }

    if (!processor_) {
        SetLastError("Audio pipeline processor is null.");
        if (error) *error = GetStats().last_error;
        return false;
    }
    if (cfg.sample_rate != 48000 || cfg.channels != 1) {
        SetLastError("Unsupported audio format: MVP requires mono 48kHz float32 (use Pulse to resample/downmix).");
        if (error) *error = GetStats().last_error;
        return false;
    }
    if (cfg.frame_samples != 480) {
        SetLastError("Unsupported frame size: MVP requires 480 samples (10ms @ 48kHz).");
        if (error) *error = GetStats().last_error;
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
    out.process_time_us_sum = process_time_us_sum_.load(std::memory_order_relaxed);
    out.process_time_us_max = process_time_us_max_.load(std::memory_order_relaxed);
    out.process_time_us_last = process_time_us_last_.load(std::memory_order_relaxed);
    out.process_overruns = process_overruns_.load(std::memory_order_relaxed);
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
        AudioPipeline* self;
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
    const std::size_t bytes_per_frame = static_cast<std::size_t>(samples_per_frame) * sizeof(float);

    // Frame budget for processor time (10ms at 48k).
    const std::uint64_t frame_budget_us =
        static_cast<std::uint64_t>(cfg.frame_samples) * 1000000u / static_cast<std::uint64_t>(cfg.sample_rate);

    pa_buffer_attr rec_attr{};
    rec_attr.maxlength = static_cast<uint32_t>(bytes_per_frame * 4);
    rec_attr.fragsize = static_cast<uint32_t>(bytes_per_frame);
    rec_attr.tlength = static_cast<uint32_t>(-1);
    rec_attr.prebuf = static_cast<uint32_t>(-1);
    rec_attr.minreq = static_cast<uint32_t>(-1);

    pa_buffer_attr play_attr{};
    play_attr.maxlength = static_cast<uint32_t>(bytes_per_frame * 8);
    play_attr.tlength = static_cast<uint32_t>(bytes_per_frame * 4);
    play_attr.prebuf = static_cast<uint32_t>(bytes_per_frame);
    play_attr.minreq = static_cast<uint32_t>(bytes_per_frame);
    play_attr.fragsize = static_cast<uint32_t>(-1);

    int pa_err = 0;

    const char* capture_dev = cfg.source_name.empty() ? nullptr : cfg.source_name.c_str();
    const char* playback_dev = cfg.sink_name.empty() ? nullptr : cfg.sink_name.c_str();

    pa_simple* rec = ::pa_simple_new(
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
        SetLastError("Pulse capture stream open failed: " + PulseErrorString(pa_err));
        return;
    }

    pa_simple* play = ::pa_simple_new(
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
        SetLastError("Pulse playback stream open failed: " + PulseErrorString(pa_err));
        ::pa_simple_free(rec);
        return;
    }

    std::vector<float> in(samples_per_frame);
    std::vector<float> out(samples_per_frame);

    processor_->Reset();

    std::string proc_err;
    std::string last_proc_warning;
    while (!stop_.load(std::memory_order_acquire)) {
        if (::pa_simple_read(rec, in.data(), bytes_per_frame, &pa_err) < 0) {
            SetLastError("Pulse capture read failed: " + PulseErrorString(pa_err));
            break;
        }

        const auto t0 = std::chrono::steady_clock::now();
        proc_err.clear();
        const bool ok = processor_->Process(in.data(), out.data(), cfg.frame_samples, cfg.channels, &proc_err);
        const auto t1 = std::chrono::steady_clock::now();

        const std::uint64_t proc_us =
            static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
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

    ::pa_simple_drain(play, &pa_err);  // best-effort
    ::pa_simple_free(play);
    ::pa_simple_free(rec);
}

}  // namespace studiocast::audio
