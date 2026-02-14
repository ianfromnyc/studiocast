#include "core/audio/virtual_audio_service.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <thread>

#include "core/audio/virtual_mic.h"
#include "core/config/settings.h"
#include "core/maxine/gpu_selection.h"
#include "core/maxine/paths.h"

// Effect planning is build-time independent from the Pulse audio pipeline.
#include "core/maxine/afx/afx_effect.h"

#if STUDIOCAST_HAVE_PULSE_SIMPLE
#include "core/audio/audio_pipeline.h"
#include "core/maxine/afx/afx_audio_processor.h"
#include "core/maxine/afx_api.h"
#endif

namespace studiocast::audio {

VirtualAudioService::~VirtualAudioService() { Stop(); }

bool VirtualAudioService::Start(const VirtualAudioServiceConfig& cfg, std::string* error) {
    Stop();
    {
        std::lock_guard<std::mutex> lock(mu_);
        cfg_ = cfg;
        st_ = VirtualAudioServiceStatus{};
        st_.service_running = false;
        mic_created_ = false;
    }

    stop_.store(false, std::memory_order_release);
    try {
        th_ = std::thread([this]() { ThreadMain(); });
    } catch (const std::exception& e) {
        if (error) *error = std::string("Failed to start VirtualAudioService thread: ") + e.what();
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(mu_);
        running_ = true;
        st_.service_running = true;
    }

    return true;
}

void VirtualAudioService::Stop() {
    stop_.store(true, std::memory_order_release);
    if (th_.joinable()) {
        th_.join();
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        running_ = false;
        st_.service_running = false;
        st_.pipeline_running = false;
        st_.pipeline_starting = false;
    }
}

void VirtualAudioService::UpdateConfig(const VirtualAudioServiceConfig& cfg) {
    std::lock_guard<std::mutex> lock(mu_);
    cfg_ = cfg;
}

VirtualAudioServiceConfig VirtualAudioService::Config() const {
    std::lock_guard<std::mutex> lock(mu_);
    return cfg_;
}

VirtualAudioServiceStatus VirtualAudioService::Status() const {
    std::lock_guard<std::mutex> lock(mu_);
    return st_;
}

void VirtualAudioService::SetLastError(std::string msg) {
    std::lock_guard<std::mutex> lock(mu_);
    st_.last_error = std::move(msg);
}

void VirtualAudioService::ThreadMain() {
    using namespace std::chrono;

    steady_clock::time_point nextStartRetry{};

#if STUDIOCAST_HAVE_PULSE_SIMPLE
    std::unique_ptr<studiocast::maxine::afx::AfxApi> api;
    std::unique_ptr<studiocast::maxine::afx::AfxEffect> fx;
    std::unique_ptr<studiocast::maxine::afx::AfxAudioProcessor> processor;
    std::unique_ptr<studiocast::audio::AudioPipeline> pipeline;

    std::optional<studiocast::audio::effects::BroadcastAudioEffects> lastFx;
    std::string lastSource;
    std::filesystem::path lastAfxLib;
#endif

    while (!stop_.load(std::memory_order_acquire)) {
        VirtualAudioServiceConfig cfg;
        {
            std::lock_guard<std::mutex> lock(mu_);
            cfg = cfg_;
        }

        const int pollMs = std::max(25, cfg.poll_ms);

        // Ensure virtual mic is present (best-effort).
        if (cfg.create_virtual_mic && !mic_created_) {
            std::string err;
            if (studiocast::audio::CreateVirtualMic(&err)) {
                std::lock_guard<std::mutex> lock(mu_);
                mic_created_ = true;
                st_.mic_present = true;
            } else {
                SetLastError("Failed to create virtual mic: " + err);
            }
        }

        {
            std::lock_guard<std::mutex> lock(mu_);
            st_.selected_source = cfg.source_name;
        }

        if (!cfg.enabled) {
#if STUDIOCAST_HAVE_PULSE_SIMPLE
            if (pipeline) {
                pipeline->Stop();
                pipeline.reset();
            }
            processor.reset();
            if (fx) {
                fx->Destroy();
                fx.reset();
            }
            api.reset();
            lastFx.reset();
            lastSource.clear();
            lastAfxLib.clear();
#endif
            {
                std::lock_guard<std::mutex> lock(mu_);
                st_.pipeline_running = false;
                st_.pipeline_starting = false;
                st_.effect_selector.clear();
                st_.feature_id.clear();
                st_.intensity = 0.0f;
            }
            std::this_thread::sleep_for(milliseconds(pollMs));
            continue;
        }

        const auto plan = studiocast::maxine::afx::PlanBroadcastMicrophoneEffect(
            cfg.effects.microphone.studio_voice_enabled,
            cfg.effects.microphone.noise_removal_enabled,
            cfg.effects.microphone.room_echo_removal_enabled,
            cfg.effects.microphone.strength);

        {
            std::lock_guard<std::mutex> lock(mu_);
            st_.effect_selector = plan.effect_selector;
            st_.feature_id = plan.feature_id;
            st_.intensity = plan.intensity;
        }

        if (!plan.enabled) {
#if STUDIOCAST_HAVE_PULSE_SIMPLE
            if (pipeline) {
                pipeline->Stop();
                pipeline.reset();
            }
            processor.reset();
            if (fx) {
                fx->Destroy();
                fx.reset();
            }
            api.reset();
            lastFx.reset();
            lastSource.clear();
            lastAfxLib.clear();
#endif
            {
                std::lock_guard<std::mutex> lock(mu_);
                st_.pipeline_running = false;
                st_.pipeline_starting = false;
            }
            SetLastError("No Maxine AFX microphone effect enabled");
            std::this_thread::sleep_for(milliseconds(pollMs));
            continue;
        }

#if !STUDIOCAST_HAVE_PULSE_SIMPLE
        SetLastError("Audio pipeline disabled at build time (libpulse-simple not found)");
        {
            std::lock_guard<std::mutex> lock(mu_);
            st_.pipeline_running = false;
            st_.pipeline_starting = false;
        }
        std::this_thread::sleep_for(milliseconds(pollMs));
        continue;
#else

        const auto now = steady_clock::now();
        if (now < nextStartRetry) {
            std::this_thread::sleep_for(milliseconds(pollMs));
            continue;
        }

        const bool needRestart = (!lastFx.has_value() || *lastFx != cfg.effects) || (lastSource != cfg.source_name);

        if (needRestart) {
            if (pipeline) {
                pipeline->Stop();
                pipeline.reset();
            }
            processor.reset();
            if (fx) {
                fx->Destroy();
                fx.reset();
            }
        }

        // Resolve GPU selection (settings.conf) and AFX SDK paths.
        const auto settings = studiocast::config::LoadSettings();
        const auto sel = studiocast::maxine::SelectGpu(settings.gpu);
        {
            std::lock_guard<std::mutex> lock(mu_);
            st_.gpu_index = sel.selected ? sel.selected->index : -1;
            st_.gpu_name = sel.selected ? sel.selected->name : std::string();
            st_.gpu_compute_cap = (sel.selected && sel.selected->compute_capability)
                                     ? sel.selected->ComputeCapString()
                                     : std::string();
        }
        if (!sel.selected || !sel.selected->compute_capability) {
            SetLastError("Failed to select a supported NVIDIA GPU: " + sel.error);
            nextStartRetry = now + milliseconds(std::max(250, cfg.start_retry_ms));
            std::this_thread::sleep_for(milliseconds(pollMs));
            continue;
        }

        const auto paths = studiocast::maxine::ResolveMaxinePaths();
        if (!paths.afx.ok) {
            std::string msg = "AFX SDK not available";
            if (!paths.afx.problems.empty()) {
                msg += ": ";
                msg += paths.afx.problems.front();
            }
            SetLastError(msg);
            nextStartRetry = now + milliseconds(std::max(250, cfg.start_retry_ms));
            std::this_thread::sleep_for(milliseconds(pollMs));
            continue;
        }

        if (!api || paths.afx.library != lastAfxLib) {
            api = std::make_unique<studiocast::maxine::afx::AfxApi>();
            std::string aerr;
            if (!api->InitializeFromLibraryPath(paths.afx.library, &aerr)) {
                SetLastError("Failed to initialize AFX runtime: " + aerr);
                api.reset();
                nextStartRetry = now + milliseconds(std::max(250, cfg.start_retry_ms));
                std::this_thread::sleep_for(milliseconds(pollMs));
                continue;
            }
            lastAfxLib = paths.afx.library;
        }

        if (!fx) {
            fx = std::make_unique<studiocast::maxine::afx::AfxEffect>(api.get());
        } else {
            fx->SetApi(api.get());
        }

        if (needRestart) {
            studiocast::maxine::afx::AfxEffectConfig e;
            e.effect_selector = plan.effect_selector;
            e.feature_id = plan.feature_id;
            e.features_dir = paths.afx.features_dir;
            e.compute_capability = sel.selected->compute_capability;
            e.sample_rate = 48000;
            e.frame_samples = 480;
            e.channels = 1;
            e.intensity = plan.intensity;
            e.use_denoiser_v2_model = plan.use_denoiser_v2_model;

            {
                std::lock_guard<std::mutex> lock(mu_);
                st_.pipeline_starting = true;
            }

            std::string ferr;
            if (!fx->Configure(e, &ferr)) {
                SetLastError("Failed to configure AFX effect: " + ferr);
                fx->Destroy();
                fx.reset();
                nextStartRetry = now + milliseconds(std::max(250, cfg.start_retry_ms));
                std::lock_guard<std::mutex> lock(mu_);
                st_.pipeline_starting = false;
                std::this_thread::sleep_for(milliseconds(pollMs));
                continue;
            }
            if (!fx->Load(&ferr)) {
                SetLastError("Failed to load AFX effect: " + ferr);
                fx->Destroy();
                fx.reset();
                nextStartRetry = now + milliseconds(std::max(250, cfg.start_retry_ms));
                std::lock_guard<std::mutex> lock(mu_);
                st_.pipeline_starting = false;
                std::this_thread::sleep_for(milliseconds(pollMs));
                continue;
            }

            processor = std::make_unique<studiocast::maxine::afx::AfxAudioProcessor>(fx.get());
            pipeline = std::make_unique<studiocast::audio::AudioPipeline>(processor.get());
            studiocast::audio::AudioPipelineConfig pcfg;
            pcfg.source_name = cfg.source_name;
            pcfg.sink_name = "studiocast_sink";

            std::string perr;
            if (!pipeline->Start(pcfg, &perr)) {
                SetLastError("Failed to start audio pipeline: " + perr);
                pipeline.reset();
                nextStartRetry = now + milliseconds(std::max(250, cfg.start_retry_ms));
                std::lock_guard<std::mutex> lock(mu_);
                st_.pipeline_starting = false;
                std::this_thread::sleep_for(milliseconds(pollMs));
                continue;
            }

            lastFx = cfg.effects;
            lastSource = cfg.source_name;
            {
                std::lock_guard<std::mutex> lock(mu_);
                st_.pipeline_starting = false;
                st_.pipeline_running = true;
                st_.last_error.clear();
            }
        }

        if (pipeline) {
            const auto stats = pipeline->GetStats();
            if (!stats.last_error.empty()) {
                SetLastError(stats.last_error);
            }
            {
                std::lock_guard<std::mutex> lock(mu_);
                st_.pipeline_running = stats.running;
            }
        }

        std::this_thread::sleep_for(milliseconds(pollMs));
#endif
    }

#if STUDIOCAST_HAVE_PULSE_SIMPLE
    if (pipeline) pipeline->Stop();
    processor.reset();
    if (fx) fx->Destroy();
#endif
}

}  // namespace studiocast::audio
