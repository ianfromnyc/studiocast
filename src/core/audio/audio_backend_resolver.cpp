#include "core/audio/audio_backend_resolver.h"

#include <algorithm>

namespace studiocast::audio {

namespace {

std::string FirstLine(const std::string& s) {
    const auto pos = s.find('\n');
    if (pos == std::string::npos) return s;
    return s.substr(0, pos);
}

}  // namespace

bool AnyMicrophoneEffectRequested(const studiocast::audio::effects::BroadcastAudioEffects& fx) {
    return fx.microphone.studio_voice_enabled || fx.microphone.noise_removal_enabled ||
           fx.microphone.room_echo_removal_enabled;
}

bool AnySpeakerEffectRequested(const studiocast::audio::effects::BroadcastAudioEffects& fx) {
    return fx.speaker.noise_removal_enabled || fx.speaker.room_echo_removal_enabled;
}

AudioBackendDecision ResolveAudioBackend(const studiocast::audio::effects::BroadcastAudioEffects& fx,
                                        const AudioBackendAvailability& avail) {
    AudioBackendDecision out;

    const bool anyRequested = AnyAudioEffectRequested(fx);
    if (!anyRequested) {
        out.backend = AudioBackendKind::kPassthrough;
        out.used_fallback = false;
        out.note.clear();
        return out;
    }

    using Pref = studiocast::audio::effects::AudioEffectsEnginePreference;
    switch (fx.engine) {
        case Pref::kOff: {
            out.backend = AudioBackendKind::kPassthrough;
            out.used_fallback = false;
            out.note = "Audio effects disabled (engine=off).";
            return out;
        }
        case Pref::kMaxine: {
            if (avail.maxine_ok) {
                out.backend = AudioBackendKind::kMaxine;
                out.used_fallback = false;
                out.note.clear();
                return out;
            }
            out.backend = AudioBackendKind::kPassthrough;
            out.used_fallback = true;
            out.note = "Maxine requested but unavailable; using pass-through.";
            const auto r = FirstLine(avail.maxine_reason);
            if (!r.empty()) out.note += "\n" + r;
            return out;
        }
        case Pref::kOpenSource: {
            if (avail.open_source_ok) {
                out.backend = AudioBackendKind::kOpenSource;
                out.used_fallback = false;
                out.note.clear();
                return out;
            }
            out.backend = AudioBackendKind::kPassthrough;
            out.used_fallback = true;
            out.note = "Open-source audio requested but unavailable; using pass-through.";
            const auto r = FirstLine(avail.open_source_reason);
            if (!r.empty()) out.note += "\n" + r;
            return out;
        }
        case Pref::kAuto: {
            // Prefer Maxine; fall back to Open Source; finally disable effects.
            if (avail.maxine_ok) {
                out.backend = AudioBackendKind::kMaxine;
                out.used_fallback = false;
                out.note.clear();
                return out;
            }
            if (avail.open_source_ok) {
                out.backend = AudioBackendKind::kOpenSource;
                out.used_fallback = true;
                out.note = "Maxine unavailable; using open-source audio backend.";
                const auto r = FirstLine(avail.maxine_reason);
                if (!r.empty()) out.note += "\n" + r;
                return out;
            }
            out.backend = AudioBackendKind::kPassthrough;
            out.used_fallback = true;
            out.note = "No supported audio backend available; effects disabled.";
            // Include at most one reason to keep the banner short.
            std::string r;
            if (!avail.maxine_reason.empty()) r = FirstLine(avail.maxine_reason);
            if (r.empty() && !avail.open_source_reason.empty()) r = FirstLine(avail.open_source_reason);
            if (!r.empty()) out.note += "\n" + r;
            return out;
        }
    }

    // Conservative default.
    out.backend = AudioBackendKind::kPassthrough;
    out.used_fallback = true;
    out.note = "Audio effects disabled.";
    return out;
}

}  // namespace studiocast::audio
