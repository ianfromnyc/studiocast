#include "core/audio/effects/broadcast_audio_effects_plan.h"

#include <algorithm>

namespace studiocast::audio::effects {

namespace {

bool Contains(const std::vector<std::string> &xs, const std::string &needle) {
  return std::find(xs.begin(), xs.end(), needle) != xs.end();
}

PlannedBroadcastAecEffect PlanAec(const BroadcastMicrophoneEffects::Aec &aec,
                                  const BroadcastAudioEffectsPlanInputs &in) {
  PlannedBroadcastAecEffect out;

  if (!aec.enabled) {
    out.enabled = false;
    out.reference_source.clear();
    out.reason = "disabled by config";
    return out;
  }

  if (aec.reference_source.empty()) {
    out.enabled = false;
    out.reference_source.clear();
    out.reason = "microphone.aec.enabled requested but "
                 "microphone.aec.reference_source is empty";
    return out;
  }

  if (!in.available_pulse_sources.empty() &&
      !Contains(in.available_pulse_sources, aec.reference_source)) {
    out.enabled = false;
    out.reference_source.clear();
    out.reason = "microphone.aec.reference_source not available: " +
                 aec.reference_source;
    return out;
  }

  out.enabled = true;
  out.reference_source = aec.reference_source;
  out.reason.clear();
  return out;
}

PlannedBroadcastSuperresEffect
PlanSuperres(const char *scope, const BroadcastMicrophoneEffects::Superres &sr,
             const BroadcastAudioEffectsPlanInputs &in) {
  PlannedBroadcastSuperresEffect out;
  out.mode = sr.mode;
  if (!sr.enabled) {
    out.enabled = false;
    out.reason = "disabled by config";
    return out;
  }
  if (!in.float32_pcm) {
    out.enabled = false;
    out.reason = std::string(scope) +
                 " superres requested but pipeline is not float32 PCM";
    return out;
  }
  out.enabled = true;
  out.reason.clear();
  return out;
}

PlannedBroadcastSuperresEffect
PlanSuperres(const char *scope, const BroadcastSpeakerEffects::Superres &sr,
             const BroadcastAudioEffectsPlanInputs &in) {
  PlannedBroadcastSuperresEffect out;
  out.mode = sr.mode;
  if (!sr.enabled) {
    out.enabled = false;
    out.reason = "disabled by config";
    return out;
  }
  if (!in.float32_pcm) {
    out.enabled = false;
    out.reason = std::string(scope) +
                 " superres requested but pipeline is not float32 PCM";
    return out;
  }
  out.enabled = true;
  out.reason.clear();
  return out;
}

} // namespace

PlannedBroadcastAudioEffects
PlanBroadcastAudioEffects(const BroadcastAudioEffects &fx,
                          const BroadcastAudioEffectsPlanInputs &in) {
  PlannedBroadcastAudioEffects out;

  out.microphone_aec = PlanAec(fx.microphone.aec, in);

  // Mode validation is enforced by the canonical model + JSON parser (enum).
  // Planning currently only checks runtime constraints (float32 PCM).
  out.microphone_superres =
      PlanSuperres("microphone", fx.microphone.superres, in);
  out.speaker_superres = PlanSuperres("speaker", fx.speaker.superres, in);

  return out;
}

} // namespace studiocast::audio::effects
