#pragma once

#include <string>

#include "core/util/json.h"
#include "core/video/effects/broadcast_effects.h"

namespace studiocast::video {

// Contract JSON (stable effect IDs + param IDs) serializer.
// This matches the schema in `core/video/effects/broadcast_effect_contract.h`.
std::string BroadcastCameraEffectsContractToJson(const studiocast::video::effects::BroadcastCameraEffects& effects);

// Apply a JSON patch (contract IDs or compatible convenience forms) onto an
// existing BroadcastCameraEffects instance.
// Returns false on schema/type errors.
bool ApplyBroadcastCameraEffectsPatchJson(const studiocast::util::json::Value& root,
                                         studiocast::video::effects::BroadcastCameraEffects* effects,
                                         std::string* error);

bool ApplyBroadcastCameraEffectsPatchJsonText(const std::string& jsonText,
                                             studiocast::video::effects::BroadcastCameraEffects* effects,
                                             std::string* error);

}  // namespace studiocast::video
