#include "core/audio/virtual_audio_service.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <filesystem>
#include <thread>

#include "core/audio/audio_backend_resolver.h"
#include "core/audio/virtual_mic.h"
#include "core/audio/virtual_speaker.h"
#include "core/config/settings.h"
#include "core/maxine/availability.h"
#include "core/maxine/gpu_selection.h"
#include "core/maxine/paths.h"
#include "core/open_audio/open_audio_audio_processor.h"

// Effect planning is build-time independent from the Pulse audio pipeline.
#include "core/maxine/afx/afx_effect.h"

#if STUDIOCAST_HAVE_PULSE_SIMPLE
#include "core/audio/audio_pipeline.h"
#include "core/audio/audio_processor.h"
#include "core/maxine/afx/afx_audio_processor.h"
#include "core/maxine/afx_api.h"
#endif

namespace studiocast::audio {

namespace {

AudioBackendAvailability ProbeAudioBackendAvailability(const VirtualAudioServiceConfig& cfg) {
    AudioBackendAvailability out;

#if !STUDIOCAST_HAVE_ONNXRUNTIME
    (void)cfg;
#endif

    // Maxine availability probe (audio needs AFX).
    if (!studiocast::maxine::BackendBuilt()) {
        out.maxine_ok = false;
        out.maxine_reason = "Maxine support not enabled in this build.";
    } else {
        const auto settings = studiocast::config::LoadSettings();
        const auto sel = studiocast::maxine::SelectGpu(settings.gpu);
        if (!sel.selected || !sel.selected->compute_capability) {
            out.maxine_ok = false;
            out.maxine_reason = "Failed to select a supported NVIDIA GPU.";
            if (!sel.error.empty()) out.maxine_reason += " " + sel.error;
        } else {
            const auto paths = studiocast::maxine::ResolveMaxinePaths();
            if (!paths.afx.ok) {
                out.maxine_ok = false;
                out.maxine_reason = "AFX SDK not available";
                if (!paths.afx.problems.empty()) {
                    out.maxine_reason += ": ";
                    out.maxine_reason += paths.afx.problems.front();
                }
            } else {
                out.maxine_ok = true;
                out.maxine_reason.clear();
            }
        }
    }

#if !STUDIOCAST_ENABLE_OPEN_AUDIO
    out.open_source_ok = false;
    out.open_source_reason = "Open Audio backend is disabled in this build.";
#elif STUDIOCAST_HAVE_ONNXRUNTIME
    // Open Audio backend availability probe.
    // Phase 5: validate that a model can be resolved (installed pack or user path).
    {
        std::string oerr;
        if (studiocast::open_audio::ResolveOpenAudioModelForMicrophone(cfg.effects, nullptr, &oerr)) {
            out.open_source_ok = true;
            out.open_source_reason.clear();
        } else {
            out.open_source_ok = false;
            out.open_source_reason = oerr.empty() ? "Open Audio backend unavailable." : oerr;
        }
    }
#else
    out.open_source_ok = false;
    out.open_source_reason = "Open Audio backend unavailable: ONNX Runtime not found at build time.";
#endif

    return out;
}

}  // namespace

VirtualAudioService::~VirtualAudioService() { Stop(); }

bool VirtualAudioService::Start(const VirtualAudioServiceConfig& cfg, std::string* error) {
    Stop();
    {
        std::lock_guard<std::mutex> lock(mu_);
        cfg_ = cfg;
        st_ = VirtualAudioServiceStatus{};
        st_.service_running = false;
        mic_created_ = false;
        speakers_created_ = false;
        speakers_loopback_running_ = false;
        speakers_loopback_target_.clear();
        speakers_loopback_latency_ms_ = 0;
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

    // If the Open Audio backend fails to initialize, latch-disable it for a short
    // cooldown to avoid rapid restart loops.
    steady_clock::time_point openAudioCooldownUntil{};
    std::string openAudioCooldownReason;

#if STUDIOCAST_HAVE_PULSE_SIMPLE
    std::unique_ptr<studiocast::maxine::afx::AfxApi> api;
    std::unique_ptr<studiocast::maxine::afx::AfxEffect> fx;
    std::unique_ptr<AudioProcessor> processor;
    std::unique_ptr<studiocast::audio::AudioPipeline> pipeline;

    std::string lastBackend;
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

        // Ensure virtual devices are present (best-effort).
        //
        // Note: `enabled` controls the microphone processing pipeline; virtual devices may be
        // created and routed independently.
        {
            // Mic device.
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

            // Speakers device and pass-through routing.
            auto setSpeakersError = [&](std::string msg) {
                std::lock_guard<std::mutex> lock(mu_);
                st_.speakers_last_error = std::move(msg);
            };
            auto clearSpeakersError = [&]() {
                std::lock_guard<std::mutex> lock(mu_);
                st_.speakers_last_error.clear();
            };

            const bool wantSpeakersDevice = cfg.create_virtual_speakers || cfg.speakers_enabled;

            if (wantSpeakersDevice && !speakers_created_) {
                std::string err;
                if (studiocast::audio::CreateVirtualSpeaker(&err)) {
                    speakers_created_ = true;
                    {
                        std::lock_guard<std::mutex> lock(mu_);
                        st_.speakers_present = true;
                    }
                    clearSpeakersError();
                } else {
                    setSpeakersError("Failed to create virtual speakers: " + err);
                }
            }

            // Keep routing state consistent with config.
            if (cfg.speakers_enabled) {
                const bool needLoopbackRestart = (!speakers_loopback_running_) ||
                                                (speakers_loopback_target_ != cfg.speaker_target_sink) ||
                                                (speakers_loopback_latency_ms_ != cfg.speaker_latency_ms);
                if (needLoopbackRestart) {
                    std::string err;
                    if (studiocast::audio::StartSpeakerLoopback(cfg.speaker_target_sink,
                                                              std::max(1, cfg.speaker_latency_ms),
                                                              &err)) {
                        speakers_created_ = true;
                        speakers_loopback_running_ = true;
                        speakers_loopback_target_ = cfg.speaker_target_sink;
                        speakers_loopback_latency_ms_ = cfg.speaker_latency_ms;

                        const auto state = studiocast::audio::LoadVirtualSpeakerState();
                        {
                            std::lock_guard<std::mutex> lock(mu_);
                            st_.speakers_present = true;
                            st_.speakers_routing_active = true;
                            st_.speaker_target_sink_active = state.loopback_target_sink_name.value_or(std::string());
                        }
                        clearSpeakersError();
                    } else {
                        setSpeakersError("Failed to start speakers routing: " + err);
                    }
                }
            } else {
                if (speakers_loopback_running_) {
                    std::string err;
                    if (studiocast::audio::StopSpeakerLoopback(&err)) {
                        speakers_loopback_running_ = false;
                        speakers_loopback_target_.clear();
                        speakers_loopback_latency_ms_ = 0;
                        {
                            std::lock_guard<std::mutex> lock(mu_);
                            st_.speakers_routing_active = false;
                            st_.speaker_target_sink_active.clear();
                        }
                        clearSpeakersError();
                    } else {
                        setSpeakersError("Failed to stop speakers routing: " + err);
                    }
                }

                // Optional cleanup: if the user disables the device, and we previously created it,
                // destroy it. This keeps daemon-managed speaker state predictable.
                if (!cfg.create_virtual_speakers && speakers_created_) {
                    std::string err;
                    if (studiocast::audio::DestroyVirtualSpeaker(&err)) {
                        speakers_created_ = false;
                        speakers_loopback_running_ = false;
                        speakers_loopback_target_.clear();
                        speakers_loopback_latency_ms_ = 0;
                        {
                            std::lock_guard<std::mutex> lock(mu_);
                            st_.speakers_present = false;
                            st_.speakers_routing_active = false;
                            st_.speaker_target_sink_active.clear();
                        }
                        clearSpeakersError();
                    } else {
                        setSpeakersError("Failed to destroy virtual speakers: " + err);
                    }
                }
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
            lastBackend.clear();
            lastAfxLib.clear();
#endif
            {
                std::lock_guard<std::mutex> lock(mu_);
                st_.pipeline_running = false;
                st_.pipeline_starting = false;
                st_.effect_selector.clear();
                st_.feature_id.clear();
                st_.intensity = 0.0f;
                st_.effects_backend_active.clear();
                st_.effects_note.clear();
            }
            std::this_thread::sleep_for(milliseconds(pollMs));
            continue;
        }

        auto plan = studiocast::maxine::afx::PlanBroadcastMicrophoneEffect(
            cfg.effects.microphone.studio_voice_enabled,
            cfg.effects.microphone.noise_removal_enabled,
            cfg.effects.microphone.room_echo_removal_enabled,
            cfg.effects.microphone.strength);

        if (!plan.enabled) {
            // Planner maps strength -> intensity even when disabled; normalize for pass-through status.
            plan.intensity = 0.0f;
        }

        {
            std::lock_guard<std::mutex> lock(mu_);
            st_.effect_selector = plan.effect_selector;
            st_.feature_id = plan.feature_id;
            st_.intensity = plan.intensity;
        }

        // Backend selection.
        AudioBackendAvailability avail;
        if (AnyMicrophoneEffectRequested(cfg.effects)) {
            avail = ProbeAudioBackendAvailability(cfg);
            const auto now2 = steady_clock::now();
            if (now2 < openAudioCooldownUntil) {
                avail.open_source_ok = false;
                avail.open_source_reason = openAudioCooldownReason.empty()
                                              ? "Open Audio backend is temporarily disabled due to a previous failure."
                                              : openAudioCooldownReason;
            }
        }
        const auto decision = ResolveAudioBackend(cfg.effects, avail);
        {
            std::lock_guard<std::mutex> lock(mu_);
            st_.effects_backend_active = std::string(ToString(decision.backend));
            st_.effects_note = decision.note;
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

        const bool wantMaxine = (decision.backend == AudioBackendKind::kMaxine) && plan.enabled;
        const bool wantOpenAudio = (decision.backend == AudioBackendKind::kOpenSource) && plan.enabled;

        std::string desiredBackend = "passthrough";
        if (wantMaxine) {
            desiredBackend = "maxine";
        } else if (wantOpenAudio) {
            desiredBackend = "open_source";
        }

        const bool effectsChanged = (!lastFx.has_value() || *lastFx != cfg.effects);
        const bool needRestart = (!pipeline) || (lastBackend != desiredBackend) || (lastSource != cfg.source_name) ||
                                 ((wantMaxine || wantOpenAudio) && effectsChanged);

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
            if (!wantMaxine) {
                // Pass-through mode does not need the AFX runtime.
                api.reset();
                lastAfxLib.clear();
                lastFx.reset();
            }
        }

        if (wantOpenAudio) {
            // Open-source backend (Phase 4 stub): validate model selection and keep the pipeline alive.
            if (needRestart) {
                {
                    std::lock_guard<std::mutex> lock(mu_);
                    st_.pipeline_starting = true;
                    st_.pipeline_running = false;

                    // Open-source backend does not require Maxine GPU selection.
                    st_.gpu_index = -1;
                    st_.gpu_name.clear();
                    st_.gpu_compute_cap.clear();
                }

                studiocast::open_audio::ResolvedOpenAudioModel selected;
                std::string oerr;
                auto oa = studiocast::open_audio::OpenAudioAudioProcessor::CreateForMicrophone(cfg.effects, &selected, &oerr);
                if (!oa) {
                    // Fall back to pass-through with a cooldown to avoid restart loops.
                    SetLastError("Open Audio initialization failed: " + oerr);
                    openAudioCooldownUntil = now + milliseconds(std::max(250, cfg.start_retry_ms));
                    openAudioCooldownReason = oerr;

                    desiredBackend = "passthrough";
                    processor = std::make_unique<PassthroughAudioProcessor>();

                    // Update status to reflect actual backend (decision may still say open_source this tick).
                    {
                        std::lock_guard<std::mutex> lock(mu_);
                        st_.effects_backend_active = "passthrough";
                        st_.effects_note = "Open-source audio backend failed to initialize; using pass-through.\n" + oerr;
                    }
                } else {
                    processor = std::move(oa);
                }

                pipeline = std::make_unique<studiocast::audio::AudioPipeline>(processor.get());

                studiocast::audio::AudioPipelineConfig pcfg;
                pcfg.source_name = cfg.source_name;
                pcfg.sink_name = "studiocast_sink";

                std::string perr;
                if (!pipeline->Start(pcfg, &perr)) {
                    SetLastError("Failed to start audio pipeline: " + perr);
                    pipeline.reset();
                    processor.reset();
                    nextStartRetry = now + milliseconds(std::max(250, cfg.start_retry_ms));
                    std::lock_guard<std::mutex> lock(mu_);
                    st_.pipeline_starting = false;
                    std::this_thread::sleep_for(milliseconds(pollMs));
                    continue;
                }

                lastBackend = desiredBackend;
                lastSource = cfg.source_name;
                lastFx = cfg.effects;

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
            continue;
        }

        if (!wantMaxine) {
            // Effects are disabled: keep the pipeline alive with pass-through audio.
            if (needRestart) {
                {
                    std::lock_guard<std::mutex> lock(mu_);
                    st_.pipeline_starting = true;
                    st_.pipeline_running = false;

                    // No GPU requirement in pass-through mode.
                    st_.gpu_index = -1;
                    st_.gpu_name.clear();
                    st_.gpu_compute_cap.clear();
                }

                processor = std::make_unique<PassthroughAudioProcessor>();
                pipeline = std::make_unique<studiocast::audio::AudioPipeline>(processor.get());

                studiocast::audio::AudioPipelineConfig pcfg;
                pcfg.source_name = cfg.source_name;
                pcfg.sink_name = "studiocast_sink";

                std::string perr;
                if (!pipeline->Start(pcfg, &perr)) {
                    SetLastError("Failed to start audio pipeline: " + perr);
                    pipeline.reset();
                    processor.reset();
                    nextStartRetry = now + milliseconds(std::max(250, cfg.start_retry_ms));
                    std::lock_guard<std::mutex> lock(mu_);
                    st_.pipeline_starting = false;
                    std::this_thread::sleep_for(milliseconds(pollMs));
                    continue;
                }

                lastBackend = desiredBackend;
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
            continue;
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
            lastBackend = desiredBackend;
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
