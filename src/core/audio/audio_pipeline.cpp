#include "core/audio/audio_pipeline.h"

#include <string>
#include <vector>

#include <pulse/error.h>
#include <pulse/simple.h>

#include "core/maxine/afx/afx_effect.h"

namespace studiocast::audio {

namespace {

std::string PulseErrorString(int pa_error) {
    const char* s = ::pa_strerror(pa_error);
    if (!s || *s == '\0') {
        return "PulseAudio error " + std::to_string(pa_error);
    }
    return std::string(s);
}

}  // namespace

AudioPipeline::AudioPipeline(maxine::afx::AfxEffect* effect) : effect_(effect) {}

AudioPipeline::~AudioPipeline() { Stop(); }

bool AudioPipeline::Start(const AudioPipelineConfig& cfg, std::string* error) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (stats_.running) {
            if (error) *error = "Audio pipeline is already running.";
            return false;
        }
        stats_ = AudioPipelineStats{};
    }

    if (!effect_) {
        SetLastError("Audio pipeline effect is null.");
        if (error) *error = GetStats().last_error;
        Stop();
        return false;
    }
    if (!effect_->IsLoaded()) {
        SetLastError("AFX effect is not loaded. Configure+Load must succeed before starting the audio pipeline.");
        if (error) *error = GetStats().last_error;
        Stop();
        return false;
    }
    if (cfg.sample_rate != 48000 || cfg.channels != 1) {
        SetLastError("Unsupported audio format: MVP requires mono 48kHz float32 (use Pulse to resample/downmix).");
        if (error) *error = GetStats().last_error;
        Stop();
        return false;
    }
    if (cfg.frame_samples != 480) {
        SetLastError("Unsupported frame size: MVP requires 480 samples (10ms @ 48kHz).");
        if (error) *error = GetStats().last_error;
        Stop();
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        stats_.running = true;
    }

    stop_.store(false, std::memory_order_release);
    thread_ = std::thread([this, cfg] { ThreadMain(cfg); });
    return true;
}

void AudioPipeline::Stop() {
    stop_.store(true, std::memory_order_release);
    if (thread_.joinable()) {
        thread_.join();
    }
    std::lock_guard<std::mutex> lock(mu_);
    stats_.running = false;
}

AudioPipelineStats AudioPipeline::GetStats() const {
    std::lock_guard<std::mutex> lock(mu_);
    return stats_;
}

void AudioPipeline::SetLastError(std::string msg) {
    std::lock_guard<std::mutex> lock(mu_);
    stats_.last_error = std::move(msg);
}

void AudioPipeline::ThreadMain(AudioPipelineConfig cfg) {
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

    while (!stop_.load(std::memory_order_acquire)) {
        if (::pa_simple_read(rec, in.data(), bytes_per_frame, &pa_err) < 0) {
            SetLastError("Pulse capture read failed: " + PulseErrorString(pa_err));
            break;
        }

        std::string run_err;
        if (!effect_->Run(in.data(), out.data(), samples_per_frame, &run_err)) {
            SetLastError("AFX Run failed: " + run_err);
            break;
        }

        if (::pa_simple_write(play, out.data(), bytes_per_frame, &pa_err) < 0) {
            SetLastError("Pulse playback write failed: " + PulseErrorString(pa_err));
            break;
        }

        {
            std::lock_guard<std::mutex> lock(mu_);
            ++stats_.frames_processed;
        }
    }

    ::pa_simple_drain(play, &pa_err);  // best-effort
    ::pa_simple_free(play);
    ::pa_simple_free(rec);

    std::lock_guard<std::mutex> lock(mu_);
    stats_.running = false;
}

}  // namespace studiocast::audio
