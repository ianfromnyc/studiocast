#pragma once

#include <string>

#include "core/util/json.h"
#include "core/video/effects/broadcast_effects.h"
#include "core/video/legacy_camera_effects.h"

namespace studiocast::video {

// Contract JSON (stable effect IDs + param IDs) serializer.
// This matches the schema in `core/video/effects/broadcast_effect_contract.h`.
std::string BroadcastCameraEffectsContractToJson(
    const studiocast::video::effects::BroadcastCameraEffects &effects);

// Apply a JSON patch (contract IDs or compatible convenience forms) onto an
// existing BroadcastCameraEffects instance.
// Returns false on schema/type errors.
bool ApplyBroadcastCameraEffectsPatchJson(
    const studiocast::util::json::Value &root,
    studiocast::video::effects::BroadcastCameraEffects *effects,
    std::string *error);

bool ApplyBroadcastCameraEffectsPatchJsonText(
    const std::string &jsonText,
    studiocast::video::effects::BroadcastCameraEffects *effects,
    std::string *error);

// Legacy compatibility helpers.
//
// These allow translating older `CameraEffects`-based surfaces (config/IPC) to
// the canonical Broadcast model.
studiocast::video::effects::BroadcastCameraEffects
ToBroadcastCameraEffects(const studiocast::video::CameraEffects &legacy);
studiocast::video::CameraEffects ToLegacyCameraEffects(
    const studiocast::video::effects::BroadcastCameraEffects &fx);

} // namespace studiocast::video
