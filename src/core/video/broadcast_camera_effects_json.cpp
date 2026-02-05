#include "core/video/broadcast_camera_effects_json.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string_view>

#include "core/video/effects/broadcast_effect_contract.h"
#include "core/video/legacy_camera_effects.h"

namespace studiocast::video {
namespace {

using studiocast::util::json::Value;
using studiocast::video::effects::BroadcastCameraEffects;
using studiocast::video::effects::EffectsEnginePreference;
using studiocast::video::effects::ParseEffectsEnginePreference;
using studiocast::video::effects::ParseVirtualBackgroundMode;
using studiocast::video::effects::ToString;
using studiocast::video::effects::VirtualBackgroundMode;

const Value::Object* AsObjectOrNull(const Value* v) {
    return v ? v->AsObject() : nullptr;
}

const Value* Find(const Value::Object& obj, const std::string& key) {
    auto it = obj.find(key);
    if (it == obj.end()) return nullptr;
    return &it->second;
}

int ClampInt(int v, int lo, int hi) {
    return std::max(lo, std::min(hi, v));
}

float ClampFloat(float v, float lo, float hi) {
    return std::max(lo, std::min(hi, v));
}

bool ParseHexColorRgb(const std::string& s, std::uint32_t* outRgb, std::string* error) {
    if (!outRgb) return false;
    *outRgb = 0;
    if (s.size() != 7 || s[0] != '#') {
        if (error) *error = "expected '#RRGGBB'";
        return false;
    }
    std::uint32_t rgb = 0;
    for (std::size_t i = 1; i < 7; ++i) {
        const char c = s[i];
        std::uint32_t v = 0;
        if (c >= '0' && c <= '9') v = static_cast<std::uint32_t>(c - '0');
        else if (c >= 'a' && c <= 'f') v = static_cast<std::uint32_t>(10 + (c - 'a'));
        else if (c >= 'A' && c <= 'F') v = static_cast<std::uint32_t>(10 + (c - 'A'));
        else {
            if (error) *error = "invalid hex digit";
            return false;
        }
        rgb = (rgb << 4u) | v;
    }
    *outRgb = rgb;
    return true;
}

std::string RgbToHexColor(std::uint32_t rgb) {
    std::ostringstream oss;
    oss << '#';
    oss << std::hex << std::setw(6) << std::setfill('0') << (rgb & 0x00ffffffu);
    return oss.str();
}

std::optional<int> KelvinFromPreset(int preset) {
    switch (preset) {
        case 1: return 3200;
        case 2: return 6500;
        default: return 4500;
    }
}

int TemperaturePresetFromString(const std::string& s) {
    if (s == "warm") return 1;
    if (s == "cool") return 2;
    return 0;
}

std::string TemperaturePresetToString(int preset) {
    switch (preset) {
        case 1: return "warm";
        case 2: return "cool";
        default: return "neutral";
    }
}

bool TryGetBool(const Value::Object& obj, const std::string& key, bool* found, bool* out, std::string* error) {
    *found = false;
    const Value* v = Find(obj, key);
    if (!v) return true;
    const bool* b = v->AsBool();
    if (!b) {
        if (error) *error = "expected boolean for '" + key + "'";
        return false;
    }
    *found = true;
    *out = *b;
    return true;
}

bool TryGetString(const Value::Object& obj,
                  const std::string& key,
                  bool* found,
                  std::string* out,
                  std::string* error) {
    *found = false;
    const Value* v = Find(obj, key);
    if (!v) return true;
    const std::string* s = v->AsString();
    if (!s) {
        if (error) *error = "expected string for '" + key + "'";
        return false;
    }
    *found = true;
    *out = *s;
    return true;
}

bool TryGetInt(const Value::Object& obj, const std::string& key, bool* found, int* out, std::string* error) {
    *found = false;
    const Value* v = Find(obj, key);
    if (!v) return true;
    const double* n = v->AsNumber();
    if (!n) {
        if (error) *error = "expected number for '" + key + "'";
        return false;
    }
    const double r = std::round(*n);
    if (std::fabs(*n - r) > 1e-9) {
        if (error) *error = "expected integer for '" + key + "'";
        return false;
    }
    *found = true;
    *out = static_cast<int>(r);
    return true;
}

bool TryGetFloat(const Value::Object& obj, const std::string& key, bool* found, float* out, std::string* error) {
    *found = false;
    const Value* v = Find(obj, key);
    if (!v) return true;
    const double* n = v->AsNumber();
    if (!n) {
        if (error) *error = "expected number for '" + key + "'";
        return false;
    }
    *found = true;
    *out = static_cast<float>(*n);
    return true;
}

const Value::Object* GetObj(const Value::Object& obj, const std::string& key, std::string* error) {
    const Value* v = Find(obj, key);
    if (!v) return nullptr;
    const auto* o = v->AsObject();
    if (!o) {
        if (error) *error = "expected object for '" + key + "'";
        return nullptr;
    }
    return o;
}

bool ParseRootObject(const Value& root, const Value::Object** out, std::string* error) {
    const auto* obj0 = root.AsObject();
    if (!obj0) {
        if (error) *error = "effects patch JSON must be an object";
        return false;
    }
    if (const auto* ve = AsObjectOrNull(Find(*obj0, "video_effects"))) {
        *out = ve;
        return true;
    }
    if (const auto* be = AsObjectOrNull(Find(*obj0, "broadcast_effects"))) {
        *out = be;
        return true;
    }
    *out = obj0;
    return true;
}

bool ApplyVirtualBackgroundEffectPatch(VirtualBackgroundMode mode,
                                      const Value::Object& patch,
                                      BroadcastCameraEffects* fx,
                                      std::string* error) {
    bool found = false;

    bool enabled = false;
    if (!TryGetBool(patch, std::string(effects::contract::param::kEnabled), &found, &enabled, error)) return false;
    if (found) {
        if (enabled) {
            fx->virtual_background.mode = mode;
            // If a virtual background mode is explicitly enabled by a patch,
            // it should disable auto-frame unless the same patch re-enables it.
            fx->auto_frame.enabled = false;
        } else {
            if (fx->virtual_background.mode == mode) fx->virtual_background.mode = VirtualBackgroundMode::none;
        }
    }

    int strength = 0;
    if (!TryGetInt(patch, std::string(effects::contract::param::kStrength), &found, &strength, error)) return false;
    if (found) {
        fx->virtual_background.strength = ClampInt(strength,
                                                   effects::contract::kVbStrengthMin,
                                                   effects::contract::kVbStrengthMax);
    }

    std::string removeColor;
    if (!TryGetString(patch, std::string(effects::contract::param::kVbRemoveColor), &found, &removeColor, error)) return false;
    if (found) {
        std::uint32_t rgb = 0;
        std::string err;
        if (!ParseHexColorRgb(removeColor, &rgb, &err)) {
            if (error) *error = "invalid remove_color: " + err;
            return false;
        }
        fx->virtual_background.remove_color = RgbToHexColor(rgb);
    }

    int gsm = static_cast<int>(fx->virtual_background.greenscreen_mode);
    if (!TryGetInt(patch, std::string(effects::contract::param::kGreenscreenMode), &found, &gsm, error)) return false;
    if (found) fx->virtual_background.greenscreen_mode = static_cast<std::uint32_t>(std::max(0, gsm));

    bool gst = fx->virtual_background.greenscreen_temporal;
    if (!TryGetBool(patch, std::string(effects::contract::param::kGreenscreenTemporal), &found, &gst, error)) return false;
    if (found) fx->virtual_background.greenscreen_temporal = gst;

    std::string rp;
    if (!TryGetString(patch, std::string(effects::contract::param::kVbReplacePath), &found, &rp, error)) return false;
    if (found) fx->virtual_background.replace_path = rp;

    if (mode == VirtualBackgroundMode::replace && fx->virtual_background.mode == VirtualBackgroundMode::replace &&
        fx->virtual_background.replace_path.empty()) {
        if (error) *error = "replace_path is required when virtual background mode is 'replace'";
        return false;
    }

    return true;
}

void ResolveBackgroundMutex(BroadcastCameraEffects* fx) {
    if (!fx) return;
    if (fx->auto_frame.enabled) {
        fx->virtual_background.mode = VirtualBackgroundMode::none;
        return;
    }
    if (fx->virtual_background.mode != VirtualBackgroundMode::none) {
        fx->auto_frame.enabled = false;
    }
}

}  // namespace

std::string BroadcastCameraEffectsContractToJson(const studiocast::video::effects::BroadcastCameraEffects& effects) {
    using studiocast::video::effects::contract::kEffectIdAutoFrame;
    using studiocast::video::effects::contract::kEffectIdEyeContact;
    using studiocast::video::effects::contract::kEffectIdMirror;
    using studiocast::video::effects::contract::kEffectIdVideoNoiseRemoval;
    using studiocast::video::effects::contract::kEffectIdVignette;
    using studiocast::video::effects::contract::kEffectIdVirtualBackgroundBlur;
    using studiocast::video::effects::contract::kEffectIdVirtualBackgroundRemove;
    using studiocast::video::effects::contract::kEffectIdVirtualBackgroundReplace;
    using studiocast::video::effects::contract::kEffectIdVirtualKeyLight;

    using studiocast::video::effects::contract::param::kCenterOnTrackedFace;
    using studiocast::video::effects::contract::param::kDirectionPanDegrees;
    using studiocast::video::effects::contract::param::kEnabled;
    using studiocast::video::effects::contract::param::kGreenscreenMode;
    using studiocast::video::effects::contract::param::kGreenscreenTemporal;
    using studiocast::video::effects::contract::param::kHdriPath;
    using studiocast::video::effects::contract::param::kHeadroom;
    using studiocast::video::effects::contract::param::kIntensity;
    using studiocast::video::effects::contract::param::kLookAwayEnabled;
    using studiocast::video::effects::contract::param::kSmoothing;
    using studiocast::video::effects::contract::param::kStrength;
    using studiocast::video::effects::contract::param::kTemperaturePreset;
    using studiocast::video::effects::contract::param::kVbRemoveColor;
    using studiocast::video::effects::contract::param::kVbReplacePath;

    const bool vbBlur = effects.virtual_background.mode == VirtualBackgroundMode::blur;
    const bool vbRemove = effects.virtual_background.mode == VirtualBackgroundMode::remove;
    const bool vbReplace = effects.virtual_background.mode == VirtualBackgroundMode::replace;

    std::ostringstream oss;
    oss << '{';

    oss << "\"engine\":\"" << studiocast::util::json::EscapeString(ToString(effects.engine)) << "\",";

    // Mirror
    oss << "\"" << kEffectIdMirror << "\":{";
    oss << "\"" << kEnabled << "\":" << (effects.mirror ? "true" : "false");
    oss << "},";

    // Virtual background modes
    oss << "\"" << kEffectIdVirtualBackgroundBlur << "\":{";
    oss << "\"" << kEnabled << "\":" << (vbBlur ? "true" : "false") << ',';
    oss << "\"" << kStrength << "\":" << effects.virtual_background.strength;
    oss << "},";

    oss << "\"" << kEffectIdVirtualBackgroundRemove << "\":{";
    oss << "\"" << kEnabled << "\":" << (vbRemove ? "true" : "false") << ',';
    oss << "\"" << kStrength << "\":" << effects.virtual_background.strength << ',';
    oss << "\"" << kVbRemoveColor << "\":\"" << studiocast::util::json::EscapeString(effects.virtual_background.remove_color) << "\",";
    oss << "\"" << kGreenscreenMode << "\":" << effects.virtual_background.greenscreen_mode << ',';
    oss << "\"" << kGreenscreenTemporal << "\":" << (effects.virtual_background.greenscreen_temporal ? "true" : "false");
    oss << "},";

    oss << "\"" << kEffectIdVirtualBackgroundReplace << "\":{";
    oss << "\"" << kEnabled << "\":" << (vbReplace ? "true" : "false") << ',';
    oss << "\"" << kStrength << "\":" << effects.virtual_background.strength << ',';
    oss << "\"" << kVbRemoveColor << "\":\"" << studiocast::util::json::EscapeString(effects.virtual_background.remove_color) << "\",";
    oss << "\"" << kVbReplacePath << "\":\"" << studiocast::util::json::EscapeString(effects.virtual_background.replace_path) << "\",";
    oss << "\"" << kGreenscreenMode << "\":" << effects.virtual_background.greenscreen_mode << ',';
    oss << "\"" << kGreenscreenTemporal << "\":" << (effects.virtual_background.greenscreen_temporal ? "true" : "false");
    oss << "},";

    // Auto frame
    oss << "\"" << kEffectIdAutoFrame << "\":{";
    oss << "\"" << kEnabled << "\":" << (effects.auto_frame.enabled ? "true" : "false") << ',';
    oss << "\"" << kStrength << "\":" << effects.auto_frame.strength << ',';
    oss << "\"" << kSmoothing << "\":" << effects.auto_frame.smoothing << ',';
    oss << "\"" << kHeadroom << "\":" << effects.auto_frame.headroom;
    oss << "},";

    // Eye contact
    oss << "\"" << kEffectIdEyeContact << "\":{";
    oss << "\"" << kEnabled << "\":" << (effects.eye_contact.enabled ? "true" : "false") << ',';
    oss << "\"" << kStrength << "\":" << effects.eye_contact.strength << ',';
    oss << "\"" << kLookAwayEnabled << "\":" << (effects.eye_contact.look_away_enabled ? "true" : "false");
    oss << "},";

    // Noise removal
    oss << "\"" << kEffectIdVideoNoiseRemoval << "\":{";
    oss << "\"" << kEnabled << "\":" << (effects.video_noise_removal.enabled ? "true" : "false") << ',';
    oss << "\"" << kStrength << "\":" << effects.video_noise_removal.strength;
    oss << "},";

    // Virtual key light
    oss << "\"" << kEffectIdVirtualKeyLight << "\":{";
    oss << "\"" << kEnabled << "\":" << (effects.virtual_key_light.enabled ? "true" : "false") << ',';
    oss << "\"" << kIntensity << "\":" << effects.virtual_key_light.intensity << ',';
    oss << "\"" << kTemperaturePreset << "\":\"" << TemperaturePresetToString(effects.virtual_key_light.temperature_preset) << "\",";
    oss << "\"" << kDirectionPanDegrees << "\":" << effects.virtual_key_light.direction_pan_degrees << ',';
    oss << "\"" << kHdriPath << "\":\"" << studiocast::util::json::EscapeString(effects.virtual_key_light.hdri_path) << "\"";
    oss << "},";

    // Vignette
    oss << "\"" << kEffectIdVignette << "\":{";
    oss << "\"" << kEnabled << "\":" << (effects.vignette.enabled ? "true" : "false") << ',';
    oss << "\"" << kIntensity << "\":" << effects.vignette.intensity << ',';
    oss << "\"" << kCenterOnTrackedFace << "\":" << (effects.vignette.center_on_tracked_face ? "true" : "false");
    oss << "}";

    oss << '}';
    return oss.str();
}

bool ApplyBroadcastCameraEffectsPatchJson(const studiocast::util::json::Value& root,
                                         studiocast::video::effects::BroadcastCameraEffects* effects,
                                         std::string* error) {
    if (!effects) {
        if (error) *error = "output pointer is null";
        return false;
    }

    const Value::Object* obj = nullptr;
    if (!ParseRootObject(root, &obj, error)) return false;

    bool found = false;

    // Engine selector.
    std::string engine;
    if (!TryGetString(*obj, "engine", &found, &engine, error)) return false;
    if (found) {
        EffectsEnginePreference ep{};
        if (!ParseEffectsEnginePreference(engine, &ep)) {
            if (error) *error = "engine must be 'auto' or 'maxine'";
            return false;
        }
        effects->engine = ep;
    }

    // Convenience: top-level boolean mirror.
    if (const Value* mv = Find(*obj, "mirror")) {
        if (const bool* b = mv->AsBool()) {
            effects->mirror = *b;
        }
    }

    // Mirror effect object.
    if (const auto* mo = GetObj(*obj, std::string(effects::contract::kEffectIdMirror), error)) {
        bool en = false;
        if (!TryGetBool(*mo, std::string(effects::contract::param::kEnabled), &found, &en, error)) return false;
        if (found) effects->mirror = en;
    }

    // Canonical virtual background mode effects.
    if (const auto* vb = GetObj(*obj, std::string(effects::contract::kEffectIdVirtualBackgroundBlur), error)) {
        if (!ApplyVirtualBackgroundEffectPatch(VirtualBackgroundMode::blur, *vb, effects, error)) return false;
    }
    if (const auto* vb = GetObj(*obj, std::string(effects::contract::kEffectIdVirtualBackgroundRemove), error)) {
        if (!ApplyVirtualBackgroundEffectPatch(VirtualBackgroundMode::remove, *vb, effects, error)) return false;
    }
    if (const auto* vb = GetObj(*obj, std::string(effects::contract::kEffectIdVirtualBackgroundReplace), error)) {
        if (!ApplyVirtualBackgroundEffectPatch(VirtualBackgroundMode::replace, *vb, effects, error)) return false;
    }

    // Convenience nested virtual_background object.
    if (const auto* vbo = GetObj(*obj, "virtual_background", error)) {
        std::string mode;
        if (!TryGetString(*vbo, "mode", &found, &mode, error)) return false;
        if (found) {
            if (mode == "auto_frame") {
                effects->auto_frame.enabled = true;
                effects->virtual_background.mode = VirtualBackgroundMode::none;
            } else {
                VirtualBackgroundMode m{};
                if (!ParseVirtualBackgroundMode(mode, &m)) {
                    if (error) *error = "unknown virtual_background.mode '" + mode + "'";
                    return false;
                }
                effects->virtual_background.mode = m;
                if (m != VirtualBackgroundMode::none) {
                    effects->auto_frame.enabled = false;
                }
            }
        }

        int strength = 0;
        if (!TryGetInt(*vbo, "strength", &found, &strength, error)) return false;
        if (found) {
            effects->virtual_background.strength = ClampInt(strength,
                                                            effects::contract::kVbStrengthMin,
                                                            effects::contract::kVbStrengthMax);
        }

        std::string removeColor;
        if (!TryGetString(*vbo, "remove_color", &found, &removeColor, error)) return false;
        if (found) {
            std::uint32_t rgb = 0;
            std::string err;
            if (!ParseHexColorRgb(removeColor, &rgb, &err)) {
                if (error) *error = "invalid virtual_background.remove_color: " + err;
                return false;
            }
            effects->virtual_background.remove_color = RgbToHexColor(rgb);
        }

        std::string rp;
        if (!TryGetString(*vbo, "replace_path", &found, &rp, error)) return false;
        if (found) effects->virtual_background.replace_path = rp;

        int gsm = 0;
        if (!TryGetInt(*vbo, "greenscreen_mode", &found, &gsm, error)) return false;
        if (found) effects->virtual_background.greenscreen_mode = static_cast<std::uint32_t>(std::max(0, gsm));

        bool gst = false;
        if (!TryGetBool(*vbo, "greenscreen_temporal", &found, &gst, error)) return false;
        if (found) effects->virtual_background.greenscreen_temporal = gst;
    }

    // Auto frame.
    if (const auto* af = GetObj(*obj, std::string(effects::contract::kEffectIdAutoFrame), error)) {
        bool en = effects->auto_frame.enabled;
        if (!TryGetBool(*af, std::string(effects::contract::param::kEnabled), &found, &en, error)) return false;
        if (found) {
            effects->auto_frame.enabled = en;
            if (en) effects->virtual_background.mode = VirtualBackgroundMode::none;
        }

        int strength = effects->auto_frame.strength;
        if (!TryGetInt(*af, std::string(effects::contract::param::kStrength), &found, &strength, error)) return false;
        if (found) effects->auto_frame.strength = ClampInt(strength, 0, 100);

        int smoothing = effects->auto_frame.smoothing;
        if (!TryGetInt(*af, std::string(effects::contract::param::kSmoothing), &found, &smoothing, error)) return false;
        if (found) effects->auto_frame.smoothing = ClampInt(smoothing, 0, 100);

        float headroom = effects->auto_frame.headroom;
        if (!TryGetFloat(*af, std::string(effects::contract::param::kHeadroom), &found, &headroom, error)) return false;
        if (found) effects->auto_frame.headroom = ClampFloat(headroom, 0.0f, 1.0f);
    }
    if (const auto* af = GetObj(*obj, "auto_frame", error)) {
        bool en = effects->auto_frame.enabled;
        if (!TryGetBool(*af, "enabled", &found, &en, error)) return false;
        if (found) {
            effects->auto_frame.enabled = en;
            if (en) effects->virtual_background.mode = VirtualBackgroundMode::none;
        }

        int strength = effects->auto_frame.strength;
        if (!TryGetInt(*af, "strength", &found, &strength, error)) return false;
        if (found) effects->auto_frame.strength = ClampInt(strength, 0, 100);

        int smoothing = effects->auto_frame.smoothing;
        if (!TryGetInt(*af, "smoothing", &found, &smoothing, error)) return false;
        if (found) effects->auto_frame.smoothing = ClampInt(smoothing, 0, 100);

        float headroom = effects->auto_frame.headroom;
        if (!TryGetFloat(*af, "headroom", &found, &headroom, error)) return false;
        if (found) effects->auto_frame.headroom = ClampFloat(headroom, 0.0f, 1.0f);
    }

    // Eye contact.
    if (const auto* ec = GetObj(*obj, std::string(effects::contract::kEffectIdEyeContact), error)) {
        bool en = effects->eye_contact.enabled;
        if (!TryGetBool(*ec, std::string(effects::contract::param::kEnabled), &found, &en, error)) return false;
        if (found) effects->eye_contact.enabled = en;

        int strength = effects->eye_contact.strength;
        if (!TryGetInt(*ec, std::string(effects::contract::param::kStrength), &found, &strength, error)) return false;
        if (found) effects->eye_contact.strength = ClampInt(strength, 0, 100);

        bool lookAway = effects->eye_contact.look_away_enabled;
        if (!TryGetBool(*ec, std::string(effects::contract::param::kLookAwayEnabled), &found, &lookAway, error)) return false;
        if (found) effects->eye_contact.look_away_enabled = lookAway;
    }
    if (const auto* ec = GetObj(*obj, "eye_contact", error)) {
        bool en = effects->eye_contact.enabled;
        if (!TryGetBool(*ec, "enabled", &found, &en, error)) return false;
        if (found) effects->eye_contact.enabled = en;

        int strength = effects->eye_contact.strength;
        if (!TryGetInt(*ec, "strength", &found, &strength, error)) return false;
        if (found) effects->eye_contact.strength = ClampInt(strength, 0, 100);

        bool lookAway = effects->eye_contact.look_away_enabled;
        if (!TryGetBool(*ec, "look_away_enabled", &found, &lookAway, error)) return false;
        if (found) effects->eye_contact.look_away_enabled = lookAway;
    }

    // Noise removal.
    if (const auto* dn = GetObj(*obj, std::string(effects::contract::kEffectIdVideoNoiseRemoval), error)) {
        bool en = effects->video_noise_removal.enabled;
        if (!TryGetBool(*dn, std::string(effects::contract::param::kEnabled), &found, &en, error)) return false;
        if (found) effects->video_noise_removal.enabled = en;

        int strength = effects->video_noise_removal.strength;
        if (!TryGetInt(*dn, std::string(effects::contract::param::kStrength), &found, &strength, error)) return false;
        if (found) effects->video_noise_removal.strength = ClampInt(strength, 0, 100);
    }
    if (const auto* dn = GetObj(*obj, "video_noise_removal", error)) {
        bool en = effects->video_noise_removal.enabled;
        if (!TryGetBool(*dn, "enabled", &found, &en, error)) return false;
        if (found) effects->video_noise_removal.enabled = en;

        int strength = effects->video_noise_removal.strength;
        if (!TryGetInt(*dn, "strength", &found, &strength, error)) return false;
        if (found) effects->video_noise_removal.strength = ClampInt(strength, 0, 100);
    }

    // Virtual key light.
    if (const auto* vkl = GetObj(*obj, std::string(effects::contract::kEffectIdVirtualKeyLight), error)) {
        bool en = effects->virtual_key_light.enabled;
        if (!TryGetBool(*vkl, std::string(effects::contract::param::kEnabled), &found, &en, error)) return false;
        if (found) effects->virtual_key_light.enabled = en;

        int intensity = effects->virtual_key_light.intensity;
        if (!TryGetInt(*vkl, std::string(effects::contract::param::kIntensity), &found, &intensity, error)) return false;
        if (found) effects->virtual_key_light.intensity = ClampInt(intensity, 0, 100);

        std::string preset;
        if (!TryGetString(*vkl, std::string(effects::contract::param::kTemperaturePreset), &found, &preset, error)) return false;
        if (found) {
            const int p = TemperaturePresetFromString(preset);
            effects->virtual_key_light.temperature_preset = p;
            effects->virtual_key_light.temperature = *KelvinFromPreset(p);
        }

        int pan = effects->virtual_key_light.direction_pan_degrees;
        if (!TryGetInt(*vkl, std::string(effects::contract::param::kDirectionPanDegrees), &found, &pan, error)) return false;
        if (found) {
            effects->virtual_key_light.direction_pan_degrees = ClampInt(pan,
                                                                       effects::contract::kVirtualKeyLightPanMin,
                                                                       effects::contract::kVirtualKeyLightPanMax);
        }

        std::string hp;
        if (!TryGetString(*vkl, std::string(effects::contract::param::kHdriPath), &found, &hp, error)) return false;
        if (found) effects->virtual_key_light.hdri_path = hp;
    }
    if (const auto* vkl = GetObj(*obj, "virtual_key_light", error)) {
        bool en = effects->virtual_key_light.enabled;
        if (!TryGetBool(*vkl, "enabled", &found, &en, error)) return false;
        if (found) effects->virtual_key_light.enabled = en;

        int intensity = effects->virtual_key_light.intensity;
        if (!TryGetInt(*vkl, "intensity", &found, &intensity, error)) return false;
        if (found) effects->virtual_key_light.intensity = ClampInt(intensity, 0, 100);

        // Accept preset as either a numeric code (0..2) or a string (neutral/warm/cool)
        // to avoid conflicts with the contract JSON form.
        if (const Value* tp = Find(*vkl, "temperature_preset")) {
            if (const auto* s = tp->AsString()) {
                effects->virtual_key_light.temperature_preset = TemperaturePresetFromString(*s);
                effects->virtual_key_light.temperature = *KelvinFromPreset(effects->virtual_key_light.temperature_preset);
            } else if (const double* n = tp->AsNumber()) {
                const int r = static_cast<int>(std::lround(*n));
                effects->virtual_key_light.temperature_preset = ClampInt(r, 0, 2);
                effects->virtual_key_light.temperature = *KelvinFromPreset(effects->virtual_key_light.temperature_preset);
            } else {
                if (error) *error = "expected string or number for 'temperature_preset'";
                return false;
            }
        }

        int pan = effects->virtual_key_light.direction_pan_degrees;
        if (!TryGetInt(*vkl, "direction_pan_degrees", &found, &pan, error)) return false;
        if (found) {
            effects->virtual_key_light.direction_pan_degrees = ClampInt(pan,
                                                                       effects::contract::kVirtualKeyLightPanMin,
                                                                       effects::contract::kVirtualKeyLightPanMax);
        }

        std::string hp;
        if (!TryGetString(*vkl, "hdri_path", &found, &hp, error)) return false;
        if (found) effects->virtual_key_light.hdri_path = hp;
    }

    // Vignette.
    if (const auto* vg = GetObj(*obj, std::string(effects::contract::kEffectIdVignette), error)) {
        bool en = effects->vignette.enabled;
        if (!TryGetBool(*vg, std::string(effects::contract::param::kEnabled), &found, &en, error)) return false;
        if (found) effects->vignette.enabled = en;

        int intensity = effects->vignette.intensity;
        if (!TryGetInt(*vg, std::string(effects::contract::param::kIntensity), &found, &intensity, error)) return false;
        if (found) effects->vignette.intensity = ClampInt(intensity, 0, 100);

        bool center = effects->vignette.center_on_tracked_face;
        if (!TryGetBool(*vg, std::string(effects::contract::param::kCenterOnTrackedFace), &found, &center, error)) return false;
        if (found) effects->vignette.center_on_tracked_face = center;
    }
    if (const auto* vg = GetObj(*obj, "vignette", error)) {
        bool en = effects->vignette.enabled;
        if (!TryGetBool(*vg, "enabled", &found, &en, error)) return false;
        if (found) effects->vignette.enabled = en;

        int intensity = effects->vignette.intensity;
        if (!TryGetInt(*vg, "intensity", &found, &intensity, error)) return false;
        if (found) effects->vignette.intensity = ClampInt(intensity, 0, 100);

        bool center = effects->vignette.center_on_tracked_face;
        if (!TryGetBool(*vg, "center_on_tracked_face", &found, &center, error)) return false;
        if (found) effects->vignette.center_on_tracked_face = center;
    }

    ResolveBackgroundMutex(effects);

    // Final consistency checks.
    effects->virtual_background.strength = ClampInt(effects->virtual_background.strength,
                                                   effects::contract::kVbStrengthMin,
                                                   effects::contract::kVbStrengthMax);
    effects->auto_frame.strength = ClampInt(effects->auto_frame.strength,
                                            effects::contract::kAutoFrameStrengthMin,
                                            effects::contract::kAutoFrameStrengthMax);
    effects->auto_frame.smoothing = ClampInt(effects->auto_frame.smoothing, 0, 100);
    effects->auto_frame.headroom = ClampFloat(effects->auto_frame.headroom,
                                              effects::contract::kAutoFrameHeadroomMin,
                                              effects::contract::kAutoFrameHeadroomMax);

    effects->eye_contact.strength = ClampInt(effects->eye_contact.strength, 0, 100);
    effects->video_noise_removal.strength = ClampInt(effects->video_noise_removal.strength, 0, 100);

    effects->virtual_key_light.intensity = ClampInt(effects->virtual_key_light.intensity, 0, 100);
    effects->virtual_key_light.temperature_preset = ClampInt(effects->virtual_key_light.temperature_preset, 0, 2);
    effects->virtual_key_light.direction_pan_degrees = ClampInt(effects->virtual_key_light.direction_pan_degrees,
                                                                effects::contract::kVirtualKeyLightPanMin,
                                                                effects::contract::kVirtualKeyLightPanMax);

    effects->vignette.intensity = ClampInt(effects->vignette.intensity, 0, 100);

    if (effects->virtual_background.mode == VirtualBackgroundMode::replace &&
        effects->virtual_background.replace_path.empty()) {
        if (error) *error = "replace_path is required when virtual background mode is 'replace'";
        return false;
    }

    return true;
}

bool ApplyBroadcastCameraEffectsPatchJsonText(const std::string& jsonText,
                                             studiocast::video::effects::BroadcastCameraEffects* effects,
                                             std::string* error) {
    studiocast::util::json::Value root;
    std::string err;
    if (!studiocast::util::json::Parse(jsonText, &root, &err)) {
        if (error) *error = err;
        return false;
    }
    return ApplyBroadcastCameraEffectsPatchJson(root, effects, error);
}

studiocast::video::effects::BroadcastCameraEffects ToBroadcastCameraEffects(const studiocast::video::CameraEffects& legacy) {
    BroadcastCameraEffects fx;
    fx.schema_version = studiocast::video::effects::kBroadcastEffectsSchemaVersion;
    fx.mirror = legacy.mirror;
    fx.engine = (legacy.background_backend == studiocast::video::effects::EffectBackend::maxine)
                    ? EffectsEnginePreference::maxine
                    : EffectsEnginePreference::auto_select;

    fx.virtual_background.strength = ClampInt(legacy.background_strength,
                                             effects::contract::kVbStrengthMin,
                                             effects::contract::kVbStrengthMax);
    fx.virtual_background.remove_color = RgbToHexColor(legacy.background_remove_color_rgb);
    fx.virtual_background.replace_path = legacy.background_replace_image.string();
    fx.virtual_background.greenscreen_mode = legacy.green_screen.mode;
    fx.virtual_background.greenscreen_temporal = legacy.green_screen.temporal;

    fx.auto_frame.enabled = (legacy.background == studiocast::video::effects::BackgroundEffect::auto_frame);
    fx.auto_frame.strength = ClampInt(legacy.auto_frame.strength,
                                      effects::contract::kAutoFrameStrengthMin,
                                      effects::contract::kAutoFrameStrengthMax);
    fx.auto_frame.smoothing = ClampInt(legacy.auto_frame.smoothing, 0, 100);
    fx.auto_frame.headroom = ClampFloat(legacy.auto_frame.headroom,
                                        effects::contract::kAutoFrameHeadroomMin,
                                        effects::contract::kAutoFrameHeadroomMax);

    switch (legacy.background) {
        case studiocast::video::effects::BackgroundEffect::none:
            fx.virtual_background.mode = VirtualBackgroundMode::none;
            break;
        case studiocast::video::effects::BackgroundEffect::blur:
            fx.virtual_background.mode = VirtualBackgroundMode::blur;
            break;
        case studiocast::video::effects::BackgroundEffect::remove:
            fx.virtual_background.mode = VirtualBackgroundMode::remove;
            break;
        case studiocast::video::effects::BackgroundEffect::replace:
            fx.virtual_background.mode = VirtualBackgroundMode::replace;
            break;
        case studiocast::video::effects::BackgroundEffect::auto_frame:
            fx.virtual_background.mode = VirtualBackgroundMode::none;
            break;
    }

    fx.eye_contact.enabled = legacy.eye_contact.enabled;
    fx.eye_contact.strength = ClampInt(legacy.eye_contact.strength, 0, 100);
    fx.eye_contact.look_away_enabled = legacy.eye_contact.look_away_enabled;

    fx.video_noise_removal.enabled = legacy.denoise;
    fx.video_noise_removal.strength = ClampInt(legacy.denoise_strength, 0, 100);

    fx.virtual_key_light.enabled = legacy.virtual_key_light.enabled;
    fx.virtual_key_light.intensity = ClampInt(static_cast<int>(std::lround(legacy.virtual_key_light.intensity * 100.0f)), 0, 100);
    fx.virtual_key_light.temperature_preset = ClampInt(legacy.virtual_key_light.temperature_preset, 0, 2);
    fx.virtual_key_light.temperature = *KelvinFromPreset(fx.virtual_key_light.temperature_preset);
    fx.virtual_key_light.direction_pan_degrees = ClampInt(static_cast<int>(std::lround(legacy.virtual_key_light.direction_pan_degrees)),
                                                         effects::contract::kVirtualKeyLightPanMin,
                                                         effects::contract::kVirtualKeyLightPanMax);
    fx.virtual_key_light.hdri_path = legacy.virtual_key_light.hdri_path.string();

    fx.vignette.enabled = legacy.vignette.enabled;
    fx.vignette.intensity = ClampInt(static_cast<int>(std::lround(legacy.vignette.intensity * 100.0f)), 0, 100);
    fx.vignette.center_on_tracked_face = legacy.vignette.center_on_tracked_face;

    ResolveBackgroundMutex(&fx);
    return fx;
}

studiocast::video::CameraEffects ToLegacyCameraEffects(const studiocast::video::effects::BroadcastCameraEffects& fx) {
    studiocast::video::CameraEffects out;
    out.mirror = fx.mirror;
    out.background_backend = (fx.engine == EffectsEnginePreference::maxine)
                                 ? studiocast::video::effects::EffectBackend::maxine
                                 : studiocast::video::effects::EffectBackend::auto_select;

    // Resolve background vs auto-frame in legacy representation.
    if (fx.auto_frame.enabled) {
        out.background = studiocast::video::effects::BackgroundEffect::auto_frame;
    } else {
        switch (fx.virtual_background.mode) {
            case VirtualBackgroundMode::none: out.background = studiocast::video::effects::BackgroundEffect::none; break;
            case VirtualBackgroundMode::blur: out.background = studiocast::video::effects::BackgroundEffect::blur; break;
            case VirtualBackgroundMode::remove: out.background = studiocast::video::effects::BackgroundEffect::remove; break;
            case VirtualBackgroundMode::replace: out.background = studiocast::video::effects::BackgroundEffect::replace; break;
        }
    }

    out.background_strength = ClampInt(fx.virtual_background.strength,
                                       effects::contract::kVbStrengthMin,
                                       effects::contract::kVbStrengthMax);

    std::uint32_t rgb = 0;
    std::string err;
    if (!ParseHexColorRgb(fx.virtual_background.remove_color, &rgb, &err)) {
        rgb = 0x000000u;
    }
    out.background_remove_color_rgb = rgb;
    out.background_replace_image = fx.virtual_background.replace_path;

    out.green_screen.mode = fx.virtual_background.greenscreen_mode;
    out.green_screen.temporal = fx.virtual_background.greenscreen_temporal;

    out.auto_frame.strength = ClampInt(fx.auto_frame.strength,
                                       effects::contract::kAutoFrameStrengthMin,
                                       effects::contract::kAutoFrameStrengthMax);
    out.auto_frame.smoothing = ClampInt(fx.auto_frame.smoothing, 0, 100);
    out.auto_frame.headroom = ClampFloat(fx.auto_frame.headroom,
                                         effects::contract::kAutoFrameHeadroomMin,
                                         effects::contract::kAutoFrameHeadroomMax);

    out.eye_contact.enabled = fx.eye_contact.enabled;
    out.eye_contact.strength = ClampInt(fx.eye_contact.strength, 0, 100);
    out.eye_contact.look_away_enabled = fx.eye_contact.look_away_enabled;

    out.denoise = fx.video_noise_removal.enabled;
    out.denoise_strength = ClampInt(fx.video_noise_removal.strength, 0, 100);

    out.virtual_key_light.enabled = fx.virtual_key_light.enabled;
    out.virtual_key_light.intensity = ClampFloat(static_cast<float>(ClampInt(fx.virtual_key_light.intensity, 0, 100)) / 100.0f,
                                                 0.0f,
                                                 1.0f);
    out.virtual_key_light.temperature_preset = ClampInt(fx.virtual_key_light.temperature_preset, 0, 2);
    out.virtual_key_light.direction_pan_degrees = static_cast<float>(ClampInt(fx.virtual_key_light.direction_pan_degrees,
                                                                              effects::contract::kVirtualKeyLightPanMin,
                                                                              effects::contract::kVirtualKeyLightPanMax));
    out.virtual_key_light.hdri_path = fx.virtual_key_light.hdri_path;

    out.vignette.enabled = fx.vignette.enabled;
    out.vignette.intensity = ClampFloat(static_cast<float>(ClampInt(fx.vignette.intensity, 0, 100)) / 100.0f,
                                        0.0f,
                                        1.0f);
    out.vignette.center_on_tracked_face = fx.vignette.center_on_tracked_face;

    return out;
}

}  // namespace studiocast::video
