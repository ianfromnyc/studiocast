#pragma once

#include <string_view>

namespace studiocast::audio::effects::contract {

// Canonical audio effect model contract (stable IDs + parameter IDs).
//
// Hard rule: These IDs are stable across daemon status, IPC/JSON, GUI, and CLI.
// Do not rename existing IDs. If an effect must be replaced, add a new ID.
//
// Effect IDs (stable):
//   - noise_removal
//   - room_echo_removal
//   - studio_voice

// ---- Effect IDs ----
inline constexpr std::string_view kEffectIdNoiseRemoval = "noise_removal";
inline constexpr std::string_view kEffectIdRoomEchoRemoval = "room_echo_removal";
inline constexpr std::string_view kEffectIdStudioVoice = "studio_voice";

// ---- Parameter IDs (stable) ----
namespace param {
inline constexpr std::string_view kEnabled = "enabled";
inline constexpr std::string_view kStrength = "strength";  // integer percent 0..100
inline constexpr std::string_view kEngine = "engine";       // "auto"|"maxine"|"open_source"|"off"
inline constexpr std::string_view kModelId = "model_id";
inline constexpr std::string_view kModelPath = "model_path";

// Studio voice uses a dedicated strength knob to avoid ambiguity with the legacy noise/echo strength.
inline constexpr std::string_view kStudioVoiceStrength = "studio_voice_strength";
}  // namespace param

}  // namespace studiocast::audio::effects::contract
