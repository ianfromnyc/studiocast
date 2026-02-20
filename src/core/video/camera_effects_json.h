#pragma once

#include <string>

#include "core/util/json.h"
#include "core/video/legacy_camera_effects.h"

namespace studiocast::video {

// DEPRECATED (Task 3): Legacy `CameraEffects` patching/serialization.
//
// The canonical effects model is now
// `studiocast::video::effects::BroadcastCameraEffects`, implemented in
// `core/video/broadcast_camera_effects_json.{h,cpp}`.
//
// This header is kept only for historical reference during the migration and
// is no longer used by the daemon/CLI/probe code paths.

// Canonical JSON shape for video effects used in IPC and config.
// This returns a JSON object string (no surrounding field name).
std::string CameraEffectsToJson(const CameraEffects &effects);

// Applies a partial update (patch) to an existing CameraEffects struct.
// The JSON root may be either the effect object itself or an object
// containing a `video_effects` object.
bool ApplyCameraEffectsPatchJson(const studiocast::util::json::Value &root,
                                 CameraEffects *effects, std::string *error);

bool ApplyCameraEffectsPatchJsonText(const std::string &jsonText,
                                     CameraEffects *effects,
                                     std::string *error);

} // namespace studiocast::video
