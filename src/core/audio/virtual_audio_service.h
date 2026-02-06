#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "core/audio/effects/broadcast_audio_effects.h"

namespace studiocast::audio {

struct VirtualAudioServiceConfig {
    // If false, the service will not run the real-time processing pipeline.
    // The virtual devices may still be created (see `create_virtual_mic`).
    bool enabled = false;

    // Keep the virtual microphone device available even when processing is disabled.
    // This is the preferred daemon-owned behavior for MVP.
    bool create_virtual_mic = true;

    // Selected input source (Pulse source name). Empty = Pulse default source.
    std::string source_name;

    // Canonical Broadcast-style audio effect settings.
    studiocast::audio::effects::BroadcastAudioEffects effects{};

    // Supervisor cadence.
    int poll_ms = 250;

    // When a start attempt fails, wait this long before retrying.
    int start_retry_ms = 2000;
};

struct VirtualAudioServiceStatus {
    bool service_running = false;

    bool mic_present = false;

    bool pipeline_running = false;
    bool pipeline_starting = false;

    std::string selected_source;
    std::string pipeline_sink = "studiocast_sink";

    // What effect is currently active (derived from the Broadcast effect model).
    std::string effect_selector;
    std::string feature_id;
    float intensity = 0.0f;

    // Best-effort selected GPU summary (for actual pipeline configuration).
    int gpu_index = -1;
    std::string gpu_name;
    std::string gpu_compute_cap;

    std::string last_error;
};

// Minimal daemon-friendly owner of StudioCast virtual audio devices and processing pipelines.
//
// MVP scope:
//  - Virtual microphone is created via `pactl` modules.
//  - Real-time processing is Maxine AFX-backed (no CPU fallbacks).
//  - Speaker processing is intentionally left optional for future iteration.
class VirtualAudioService final {
public:
    VirtualAudioService() = default;
    ~VirtualAudioService();

    VirtualAudioService(const VirtualAudioService&) = delete;
    VirtualAudioService& operator=(const VirtualAudioService&) = delete;

    // Starts the supervisor thread.
    bool Start(const VirtualAudioServiceConfig& cfg, std::string* error);
    void Stop();

    void UpdateConfig(const VirtualAudioServiceConfig& cfg);

    VirtualAudioServiceConfig Config() const;
    VirtualAudioServiceStatus Status() const;

private:
    void ThreadMain();

    void SetLastError(std::string msg);

    mutable std::mutex mu_;
    std::thread th_;
    std::atomic_bool stop_{false};

    bool running_ = false;
    bool mic_created_ = false;

    VirtualAudioServiceConfig cfg_{};
    VirtualAudioServiceStatus st_{};
};

}  // namespace studiocast::audio
