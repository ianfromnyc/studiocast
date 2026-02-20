#pragma once

#include <string>
#include <vector>

#include "core/audio/effects/broadcast_audio_effects.h"
#include "core/util/json.h"

namespace studiocast::audio::effects {

struct BroadcastAudioEffectsJsonParseOptions {
  // If false (default), unknown keys cause parsing to fail.
  // If true, unknown keys are ignored and reported via `warnings`.
  bool allow_unknown_keys = false;

  // If true (default), accept a small set of legacy/alias keys/values and
  // report them via `warnings`.
  bool allow_compat_keys = true;
};

// Canonical JSON for `BroadcastAudioEffects` used for persistence + IPC.
// The returned string is a JSON object (no surrounding field name).
std::string BroadcastAudioEffectsToJson(const BroadcastAudioEffects &effects);

// Parses canonical JSON into `out`.
//
// Strictness:
// - By default, unknown keys are rejected.
// - When `options.allow_unknown_keys` is true, unknown keys are ignored but
//   appended to `warnings` (if provided).
//
// Errors:
// - On failure, returns false and writes a human-readable message into `error`
//   (if provided). Messages are intended to be GUI-friendly.
bool ParseBroadcastAudioEffectsJson(
    const studiocast::util::json::Value &root, BroadcastAudioEffects *out,
    const BroadcastAudioEffectsJsonParseOptions &options,
    std::vector<std::string> *warnings, std::string *error);

bool ParseBroadcastAudioEffectsJsonText(
    const std::string &jsonText, BroadcastAudioEffects *out,
    const BroadcastAudioEffectsJsonParseOptions &options,
    std::vector<std::string> *warnings, std::string *error);

} // namespace studiocast::audio::effects
