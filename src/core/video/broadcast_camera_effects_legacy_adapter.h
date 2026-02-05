#pragma once

#include "core/video/effects/broadcast_effects.h"
#include "core/video/legacy_camera_effects.h"

namespace studiocast::video {

// Temporary adapters for migration/compatibility tooling.
// Intentionally split out of `broadcast_camera_effects_json.h` so that GUI/IPC
// users of the canonical Broadcast schema do not depend on legacy effect types.
studiocast::video::effects::BroadcastCameraEffects ToBroadcastCameraEffects(const studiocast::video::CameraEffects& legacy);
studiocast::video::CameraEffects ToLegacyCameraEffects(const studiocast::video::effects::BroadcastCameraEffects& fx);

}  // namespace studiocast::video