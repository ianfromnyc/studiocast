#include "core/video/camera_effects_json.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string_view>

#include "core/video/effects/effect_types.h"
#include "core/video/effects/broadcast_effect_contract.h"

namespace studiocast::video {

    // DEPRECATED (Task 3): Legacy `CameraEffects` patching/serialization.
    //
    // The canonical effects model is now `studiocast::video::effects::BroadcastCameraEffects`,
    // implemented in `core/video/broadcast_camera_effects_json.{h,cpp}`.
    // This file is kept only for historical reference during the migration and
    // is no longer used by the daemon/CLI/probe code paths.

    namespace {
        using studiocast::util::json::Value;

        const Value::Object* AsObjectOrNull(const Value* v) {
            return v ? v->AsObject() : nullptr;
        }

        const Value* Find(const Value::Object& obj, const std::string& key) {
            auto it = obj.find(key);
            if (it == obj.end()) return nullptr;
            return &it->second;
        }

        bool TryGetBool(const Value::Object& obj, const std::string& key, bool* found, bool* out, std::string* err) {
            *found = false;
            const Value* v = Find(obj, key);
            if (!v) return true;
            const bool* b = v->AsBool();
            if (!b) {
                if (err) *err = "expected boolean for '" + key + "'";
                return false;
            }
            *found = true;
            *out = *b;
            return true;
        }

        bool TryGetString(const Value::Object& obj, const std::string& key, bool* found, std::string* out, std::string* err) {
            *found = false;
            const Value* v = Find(obj, key);
            if (!v) return true;
            const std::string* s = v->AsString();
            if (!s) {
                if (err) *err = "expected string for '" + key + "'";
                return false;
            }
            *found = true;
            *out = *s;
            return true;
        }

        bool TryGetInt(const Value::Object& obj, const std::string& key, bool* found, int* out, std::string* err) {
            *found = false;
            const Value* v = Find(obj, key);
            if (!v) return true;
            const double* n = v->AsNumber();
            if (!n) {
                if (err) *err = "expected number for '" + key + "'";
                return false;
            }
            *found = true;
            *out = static_cast<int>(std::lround(*n));
            return true;
        }

        bool TryGetFloat(const Value::Object& obj, const std::string& key, bool* found, float* out, std::string* err) {
            *found = false;
            const Value* v = Find(obj, key);
            if (!v) return true;
            const double* n = v->AsNumber();
            if (!n) {
                if (err) *err = "expected number for '" + key + "'";
                return false;
            }
            *found = true;
            *out = static_cast<float>(*n);
            return true;
        }

        int ClampInt(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }
        float ClampFloat(float v, float lo, float hi) { return std::max(lo, std::min(hi, v)); }

        bool ParseHexColorRgb(const std::string& s, std::uint32_t* out, std::string* err) {
            std::string_view v = s;
            if (!v.empty() && v.front() == '#') v.remove_prefix(1);
            if (v.size() != 6) {
                if (err) *err = "expected color like '#RRGGBB'";
                return false;
            }
            auto hex = [](char c) -> std::optional<int> {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
                if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
                return std::nullopt;
            };
            std::uint32_t rgb = 0;
            for (char c : v) {
                const auto n = hex(c);
                if (!n) {
                    if (err) *err = "invalid hex in color";
                    return false;
                }
                rgb = (rgb << 4) | static_cast<std::uint32_t>(*n);
            }
            *out = rgb;
            return true;
        }

        std::string EngineToString(studiocast::video::effects::EffectBackend b) {
            // Production rule: cpu is not a supported engine.
            return b == studiocast::video::effects::EffectBackend::maxine ? "maxine" : "auto";
        }

        studiocast::video::effects::EffectBackend EngineFromString(const std::string& s) {
            if (s == "maxine") return studiocast::video::effects::EffectBackend::maxine;
            return studiocast::video::effects::EffectBackend::auto_select;
        }

        std::string VbModeToString(studiocast::video::effects::BackgroundEffect b) {
            using studiocast::video::effects::BackgroundEffect;
            switch (b) {
                case BackgroundEffect::none: return "none";
                case BackgroundEffect::blur: return "blur";
                case BackgroundEffect::remove: return "remove";
                case BackgroundEffect::replace: return "replace";
                case BackgroundEffect::auto_frame: return "auto_frame";
            }
            return "none";
        }

        int Float01ToPercent(float v) { return ClampInt(static_cast<int>(std::lround(ClampFloat(v, 0.0f, 1.0f) * 100.0f)), 0, 100); }
        float PercentToFloat01(int v) { return ClampFloat(static_cast<float>(ClampInt(v, 0, 100)) / 100.0f, 0.0f, 1.0f); }

        std::string TemperaturePresetToString(int preset) {
            switch (preset) {
                case 1: return "warm";
                case 2: return "cool";
                default: return "neutral";
            }
        }

        int TemperaturePresetFromString(const std::string& s) {
            if (s == "warm") return 1;
            if (s == "cool") return 2;
            return 0;
        }

        const Value::Object* GetObj(const Value::Object& obj, const std::string& key, std::string* err) {
            const Value* v = Find(obj, key);
            if (!v) return nullptr;
            const auto* o = v->AsObject();
            if (!o) {
                if (err) *err = "expected object for '" + key + "'";
                return nullptr;
            }
            return o;
        }

        bool ApplyVirtualBackgroundPatch(const Value::Object& vb, CameraEffects* effects, bool* vbModeSet, studiocast::video::effects::BackgroundEffect* vbMode, std::string* err) {
            bool found = false;
            std::string mode;
            if (!TryGetString(vb, "mode", &found, &mode, err)) return false;
            if (found) {
                studiocast::video::effects::BackgroundEffect parsed = effects->background;
                if (!studiocast::video::effects::ParseBackgroundEffect(mode, &parsed)) {
                    if (err) *err = "unknown virtual_background.mode '" + mode + "'";
                    return false;
                }
                *vbModeSet = true;
                *vbMode = parsed;
            }

            // Legacy key: blur_strength. Canonical key: strength.
            int strength = 0;
            if (!TryGetInt(vb, "strength", &found, &strength, err)) return false;
            if (!found) {
                if (!TryGetInt(vb, "blur_strength", &found, &strength, err)) return false;
            }
            if (found) {
                effects->background_strength = ClampInt(strength,
                                                      studiocast::video::effects::contract::kVbStrengthMin,
                                                      studiocast::video::effects::contract::kVbStrengthMax);
            }

            std::string removeColor;
            if (!TryGetString(vb, "remove_color", &found, &removeColor, err)) return false;
            if (found) {
                std::uint32_t rgb = 0;
                if (!ParseHexColorRgb(removeColor, &rgb, err)) {
                    if (err && err->empty()) *err = "invalid virtual_background.remove_color";
                    return false;
                }
                effects->background_remove_color_rgb = rgb;
            }

            std::string replacePath;
            if (!TryGetString(vb, "replace_path", &found, &replacePath, err)) return false;
            if (found) effects->background_replace_image = replacePath;

            return true;
        }

    }  // namespace

    std::string CameraEffectsToJson(const CameraEffects& effects) {
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

        std::ostringstream cs;
        cs << "#";
        cs.setf(std::ios::hex, std::ios::basefield);
        cs.width(6);
        cs.fill('0');
        cs << static_cast<unsigned>(effects.background_remove_color_rgb & 0xFFFFFFu);
        const std::string removeColor = cs.str();

        const bool autoFrameEnabled = (effects.background == studiocast::video::effects::BackgroundEffect::auto_frame);
        const bool vbBlurEnabled = (!autoFrameEnabled && effects.background == studiocast::video::effects::BackgroundEffect::blur);
        const bool vbRemoveEnabled = (!autoFrameEnabled && effects.background == studiocast::video::effects::BackgroundEffect::remove);
        const bool vbReplaceEnabled = (!autoFrameEnabled && effects.background == studiocast::video::effects::BackgroundEffect::replace);

        std::ostringstream oss;
        oss << "{";
        oss << "\"" << studiocast::util::json::EscapeString(std::string(kEffectIdMirror)) << "\":{";
        oss << "\"" << studiocast::util::json::EscapeString(std::string(kEnabled)) << "\":" << (effects.mirror ? "true" : "false");
        oss << "},";

        oss << "\"engine\":\"" << studiocast::util::json::EscapeString(EngineToString(effects.background_backend)) << "\",";

        // Virtual background modes (mutually exclusive).
        oss << "\"" << studiocast::util::json::EscapeString(std::string(kEffectIdVirtualBackgroundBlur)) << "\":{";
        oss << "\"" << studiocast::util::json::EscapeString(std::string(kEnabled)) << "\":" << (vbBlurEnabled ? "true" : "false") << ",";
        oss << "\"" << studiocast::util::json::EscapeString(std::string(kStrength)) << "\":" << effects.background_strength;
        oss << "},";

        oss << "\"" << studiocast::util::json::EscapeString(std::string(kEffectIdVirtualBackgroundRemove)) << "\":{";
        oss << "\"" << studiocast::util::json::EscapeString(std::string(kEnabled)) << "\":" << (vbRemoveEnabled ? "true" : "false") << ",";
        oss << "\"" << studiocast::util::json::EscapeString(std::string(kStrength)) << "\":" << effects.background_strength << ",";
        oss << "\"" << studiocast::util::json::EscapeString(std::string(kVbRemoveColor)) << "\":\"" << studiocast::util::json::EscapeString(removeColor) << "\",";
        oss << "\"" << studiocast::util::json::EscapeString(std::string(kGreenscreenMode)) << "\":" << effects.green_screen.mode << ",";
        oss << "\"" << studiocast::util::json::EscapeString(std::string(kGreenscreenTemporal)) << "\":" << (effects.green_screen.temporal ? "true" : "false");
        oss << "},";

        oss << "\"" << studiocast::util::json::EscapeString(std::string(kEffectIdVirtualBackgroundReplace)) << "\":{";
        oss << "\"" << studiocast::util::json::EscapeString(std::string(kEnabled)) << "\":" << (vbReplaceEnabled ? "true" : "false") << ",";
        oss << "\"" << studiocast::util::json::EscapeString(std::string(kStrength)) << "\":" << effects.background_strength << ",";
        oss << "\"" << studiocast::util::json::EscapeString(std::string(kVbReplacePath)) << "\":\"" << studiocast::util::json::EscapeString(effects.background_replace_image.string()) << "\",";
        oss << "\"" << studiocast::util::json::EscapeString(std::string(kVbRemoveColor)) << "\":\"" << studiocast::util::json::EscapeString(removeColor) << "\",";
        oss << "\"" << studiocast::util::json::EscapeString(std::string(kGreenscreenMode)) << "\":" << effects.green_screen.mode << ",";
        oss << "\"" << studiocast::util::json::EscapeString(std::string(kGreenscreenTemporal)) << "\":" << (effects.green_screen.temporal ? "true" : "false");
        oss << "},";

        // Auto Frame (mutually exclusive with virtual background).
        oss << "\"" << studiocast::util::json::EscapeString(std::string(kEffectIdAutoFrame)) << "\":{";
        oss << "\"" << studiocast::util::json::EscapeString(std::string(kEnabled)) << "\":" << (autoFrameEnabled ? "true" : "false") << ",";
        oss << "\"" << studiocast::util::json::EscapeString(std::string(kStrength)) << "\":" << effects.auto_frame.strength << ",";
        oss << "\"" << studiocast::util::json::EscapeString(std::string(kSmoothing)) << "\":" << effects.auto_frame.smoothing << ",";
        oss << "\"" << studiocast::util::json::EscapeString(std::string(kHeadroom)) << "\":" << effects.auto_frame.headroom;
        oss << "},";

        // VFX/AR effects.
        oss << "\"" << studiocast::util::json::EscapeString(std::string(kEffectIdVideoNoiseRemoval)) << "\":{";
        oss << "\"" << studiocast::util::json::EscapeString(std::string(kEnabled)) << "\":" << (effects.denoise ? "true" : "false") << ",";
        oss << "\"" << studiocast::util::json::EscapeString(std::string(kStrength)) << "\":" << effects.denoise_strength;
        oss << "},";

        oss << "\"" << studiocast::util::json::EscapeString(std::string(kEffectIdEyeContact)) << "\":{";
        oss << "\"" << studiocast::util::json::EscapeString(std::string(kEnabled)) << "\":" << (effects.eye_contact.enabled ? "true" : "false") << ",";
        oss << "\"" << studiocast::util::json::EscapeString(std::string(kStrength)) << "\":" << effects.eye_contact.strength << ",";
        oss << "\"" << studiocast::util::json::EscapeString(std::string(kLookAwayEnabled)) << "\":" << (effects.eye_contact.look_away_enabled ? "true" : "false");
        oss << "},";

        oss << "\"" << studiocast::util::json::EscapeString(std::string(kEffectIdVirtualKeyLight)) << "\":{";
        oss << "\"" << studiocast::util::json::EscapeString(std::string(kEnabled)) << "\":" << (effects.virtual_key_light.enabled ? "true" : "false") << ",";
        oss << "\"" << studiocast::util::json::EscapeString(std::string(kIntensity)) << "\":" << Float01ToPercent(effects.virtual_key_light.intensity) << ",";
        oss << "\"" << studiocast::util::json::EscapeString(std::string(kTemperaturePreset)) << "\":\"" << studiocast::util::json::EscapeString(TemperaturePresetToString(effects.virtual_key_light.temperature_preset)) << "\",";
        oss << "\"" << studiocast::util::json::EscapeString(std::string(kDirectionPanDegrees)) << "\":" << static_cast<int>(std::lround(effects.virtual_key_light.direction_pan_degrees)) << ",";
        oss << "\"" << studiocast::util::json::EscapeString(std::string(kHdriPath)) << "\":\"" << studiocast::util::json::EscapeString(effects.virtual_key_light.hdri_path.string()) << "\"";
        oss << "},";

        oss << "\"" << studiocast::util::json::EscapeString(std::string(kEffectIdVignette)) << "\":{";
        oss << "\"" << studiocast::util::json::EscapeString(std::string(kEnabled)) << "\":" << (effects.vignette.enabled ? "true" : "false") << ",";
        oss << "\"" << studiocast::util::json::EscapeString(std::string(kIntensity)) << "\":" << Float01ToPercent(effects.vignette.intensity) << ",";
        oss << "\"" << studiocast::util::json::EscapeString(std::string(kCenterOnTrackedFace)) << "\":" << (effects.vignette.center_on_tracked_face ? "true" : "false");
        oss << "}";

        oss << "}";
        return oss.str();
    }

    bool ApplyCameraEffectsPatchJson(const studiocast::util::json::Value& root,
                                     CameraEffects* effects,
                                     std::string* error) {
        if (!effects) return false;
        const auto* obj0 = root.AsObject();
        if (!obj0) {
            if (error) *error = "effects JSON must be an object";
            return false;
        }

        // Allow `{ "video_effects": { ... } }` as an input convenience.
        const Value* ve = Find(*obj0, "video_effects");
        const Value::Object* obj = obj0;
        if (const auto* veObj = AsObjectOrNull(ve)) obj = veObj;

        bool found = false;

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
        using studiocast::video::effects::contract::param::kHeadroom;
        using studiocast::video::effects::contract::param::kIntensity;
        using studiocast::video::effects::contract::param::kLookAwayEnabled;
        using studiocast::video::effects::contract::param::kSmoothing;
        using studiocast::video::effects::contract::param::kStrength;
        using studiocast::video::effects::contract::param::kTemperaturePreset;
        using studiocast::video::effects::contract::param::kVbRemoveColor;
        using studiocast::video::effects::contract::param::kVbReplacePath;

        // mirror: canonical form is an object { enabled: bool }, but accept legacy boolean.
        if (const Value* mv = Find(*obj, std::string(kEffectIdMirror))) {
            if (const auto* mo = mv->AsObject()) {
                bool en = effects->mirror;
                if (!TryGetBool(*mo, std::string(kEnabled), &found, &en, error)) return false;
                if (found) effects->mirror = en;
            } else if (const bool* mb = mv->AsBool()) {
                effects->mirror = *mb;
            } else {
                if (error) *error = "expected object or boolean for '" + std::string(kEffectIdMirror) + "'";
                return false;
            }
        }

        std::string engine;
        if (!TryGetString(*obj, "engine", &found, &engine, error)) return false;
        if (found) {
            effects->background_backend = EngineFromString(engine);
        }

        // We treat changes to `virtual_background.mode` as implicitly disabling
        // auto-frame unless the patch explicitly enables it.
        bool vbModeSet = false;
        studiocast::video::effects::BackgroundEffect vbMode = effects->background;

        // Canonical VB schema: keys are `virtual_background.*` with `{ enabled, strength, ... }`.
        {
            auto vb_set_mode = [&](studiocast::video::effects::BackgroundEffect newMode) -> bool {
                if (vbModeSet && vbMode != newMode) {
                    if (error) *error = "multiple virtual background modes set in one patch";
                    return false;
                }
                vbModeSet = true;
                vbMode = newMode;
                return true;
            };

            auto apply_vb_obj = [&](std::string_view effectId,
                                   studiocast::video::effects::BackgroundEffect mode) -> bool {
                const Value* vv = Find(*obj, std::string(effectId));
                if (!vv) return true;
                const auto* vo = vv->AsObject();
                if (!vo) {
                    if (error) *error = "expected object for '" + std::string(effectId) + "'";
                    return false;
                }

                bool en = false;
                bool enFound = false;
                if (!TryGetBool(*vo, std::string(kEnabled), &enFound, &en, error)) return false;
                if (enFound) {
                    if (en) {
                        if (!vb_set_mode(mode)) return false;
                    } else {
                        if (effects->background == mode) {
                            if (!vb_set_mode(studiocast::video::effects::BackgroundEffect::none)) return false;
                        }
                    }
                }

                int strength = effects->background_strength;
                if (!TryGetInt(*vo, std::string(kStrength), &found, &strength, error)) return false;
                if (found) {
                    effects->background_strength = ClampInt(strength,
                                                          studiocast::video::effects::contract::kVbStrengthMin,
                                                          studiocast::video::effects::contract::kVbStrengthMax);
                }

                std::string removeColor;
                if (!TryGetString(*vo, std::string(kVbRemoveColor), &found, &removeColor, error)) return false;
                if (found) {
                    std::uint32_t rgb = 0;
                    if (!ParseHexColorRgb(removeColor, &rgb, error)) return false;
                    effects->background_remove_color_rgb = rgb;
                }

                std::string replacePath;
                if (!TryGetString(*vo, std::string(kVbReplacePath), &found, &replacePath, error)) return false;
                if (found) effects->background_replace_image = replacePath;

                int gsMode = 0;
                if (!TryGetInt(*vo, std::string(kGreenscreenMode), &found, &gsMode, error)) return false;
                if (found) effects->green_screen.mode = static_cast<std::uint32_t>(std::max(0, gsMode));
                bool gsTemporal = effects->green_screen.temporal;
                if (!TryGetBool(*vo, std::string(kGreenscreenTemporal), &found, &gsTemporal, error)) return false;
                if (found) effects->green_screen.temporal = gsTemporal;

                return true;
            };

            if (!apply_vb_obj(kEffectIdVirtualBackgroundBlur, studiocast::video::effects::BackgroundEffect::blur)) return false;
            if (!apply_vb_obj(kEffectIdVirtualBackgroundRemove, studiocast::video::effects::BackgroundEffect::remove)) return false;
            if (!apply_vb_obj(kEffectIdVirtualBackgroundReplace, studiocast::video::effects::BackgroundEffect::replace)) return false;
        }

        // Canonical nested keys.
        if (const auto* vb = GetObj(*obj, "virtual_background", error)) {
            if (!ApplyVirtualBackgroundPatch(*vb, effects, &vbModeSet, &vbMode, error)) return false;
        }

        // Legacy flat keys.
        {
            std::string mode;
            if (!TryGetString(*obj, "background", &found, &mode, error)) return false;
            if (found) {
                studiocast::video::effects::BackgroundEffect parsed = effects->background;
                if (!studiocast::video::effects::ParseBackgroundEffect(mode, &parsed)) {
                    if (error) *error = "unknown background '" + mode + "'";
                    return false;
                }
                vbModeSet = true;
                vbMode = parsed;
            }
        }

        int bgStrength = 0;
        if (!TryGetInt(*obj, "background_strength", &found, &bgStrength, error)) return false;
        if (found) effects->background_strength = ClampInt(bgStrength,
                                                           studiocast::video::effects::contract::kVbStrengthMin,
                                                           studiocast::video::effects::contract::kVbStrengthMax);

        {
            std::string c;
            if (!TryGetString(*obj, "background_remove_color", &found, &c, error)) return false;
            if (found) {
                std::uint32_t rgb = 0;
                if (!ParseHexColorRgb(c, &rgb, error)) return false;
                effects->background_remove_color_rgb = rgb;
            }
        }
        {
            std::string p;
            if (!TryGetString(*obj, "background_replace_image", &found, &p, error)) return false;
            if (found) effects->background_replace_image = p;
        }

        std::string backend;
        if (!TryGetString(*obj, "background_backend", &found, &backend, error)) return false;
        if (found) {
            effects->background_backend = EngineFromString(backend == "maxine" ? "maxine" : "auto");
        }

        // Auto Frame.
        bool autoFrameEnabledSet = false;
        bool autoFrameEnabled = (effects->background == studiocast::video::effects::BackgroundEffect::auto_frame);
        if (const auto* af = GetObj(*obj, "auto_frame", error)) {
            if (!TryGetBool(*af, "enabled", &found, &autoFrameEnabled, error)) return false;
            if (found) autoFrameEnabledSet = true;

            int strength = 0;
            if (!TryGetInt(*af, std::string(kStrength), &found, &strength, error)) return false;
            if (!found) {
                if (!TryGetInt(*af, "zoom", &found, &strength, error)) return false;
            }
            if (found) effects->auto_frame.strength = ClampInt(strength, 0, 100);

            int smoothing = 0;
            if (!TryGetInt(*af, std::string(kSmoothing), &found, &smoothing, error)) return false;
            if (found) effects->auto_frame.smoothing = ClampInt(smoothing, 0, 100);

            float headroom = 0;
            if (!TryGetFloat(*af, std::string(kHeadroom), &found, &headroom, error)) return false;
            if (found) effects->auto_frame.headroom = ClampFloat(headroom, 0.0f, 1.0f);
        }

        // Denoise.
        if (const auto* dn = GetObj(*obj, "video_noise_removal", error)) {
            bool en = effects->denoise;
            if (!TryGetBool(*dn, "enabled", &found, &en, error)) return false;
            if (found) effects->denoise = en;
            int strength = 0;
            if (!TryGetInt(*dn, std::string(kStrength), &found, &strength, error)) return false;
            if (found) effects->denoise_strength = ClampInt(strength, 0, 100);
        }
        bool enDenoise = effects->denoise;
        if (!TryGetBool(*obj, "denoise", &found, &enDenoise, error)) return false;
        if (found) effects->denoise = enDenoise;
        int denoiseStrength = 0;
        if (!TryGetInt(*obj, "denoise_strength", &found, &denoiseStrength, error)) return false;
        if (found) effects->denoise_strength = ClampInt(denoiseStrength, 0, 100);

        // Eye Contact.
        if (const auto* ec = GetObj(*obj, "eye_contact", error)) {
            bool en = effects->eye_contact.enabled;
            if (!TryGetBool(*ec, "enabled", &found, &en, error)) return false;
            if (found) effects->eye_contact.enabled = en;
            int strength = 0;
            if (!TryGetInt(*ec, std::string(kStrength), &found, &strength, error)) return false;
            if (found) effects->eye_contact.strength = ClampInt(strength, 0, 100);
            bool lookAway = effects->eye_contact.look_away_enabled;
            if (!TryGetBool(*ec, std::string(kLookAwayEnabled), &found, &lookAway, error)) return false;
            if (!found) {
                if (!TryGetBool(*ec, "look_away", &found, &lookAway, error)) return false;
            }
            if (found) effects->eye_contact.look_away_enabled = lookAway;
        }
        // Legacy flat boolean (do not error if the canonical object form is present).
        if (const Value* v = Find(*obj, "eye_contact")) {
            if (const bool* b = v->AsBool()) {
                effects->eye_contact.enabled = *b;
            }
        }
        int ecStrength = 0;
        if (!TryGetInt(*obj, "eye_contact_strength", &found, &ecStrength, error)) return false;
        if (found) effects->eye_contact.strength = ClampInt(ecStrength, 0, 100);
        bool ecLookAway = effects->eye_contact.look_away_enabled;
        if (!TryGetBool(*obj, "eye_contact_look_away", &found, &ecLookAway, error)) return false;
        if (found) effects->eye_contact.look_away_enabled = ecLookAway;

        // Virtual Key Light.
        if (const auto* vkl = GetObj(*obj, "virtual_key_light", error)) {
            bool en = effects->virtual_key_light.enabled;
            if (!TryGetBool(*vkl, "enabled", &found, &en, error)) return false;
            if (found) effects->virtual_key_light.enabled = en;

            int intensityPct = 0;
            if (!TryGetInt(*vkl, std::string(kIntensity), &found, &intensityPct, error)) return false;
            if (found) effects->virtual_key_light.intensity = PercentToFloat01(intensityPct);

            std::string temp;
            if (!TryGetString(*vkl, std::string(kTemperaturePreset), &found, &temp, error)) return false;
            if (!found) {
                if (!TryGetString(*vkl, "temperature", &found, &temp, error)) return false;
            }
            if (found) effects->virtual_key_light.temperature_preset = TemperaturePresetFromString(temp);

            int pan = 0;
            if (!TryGetInt(*vkl, std::string(kDirectionPanDegrees), &found, &pan, error)) return false;
            if (!found) {
                if (!TryGetInt(*vkl, "pan", &found, &pan, error)) return false;
            }
            if (found) effects->virtual_key_light.direction_pan_degrees = static_cast<float>(ClampInt(pan, -180, 180));

            std::string hdri;
            if (!TryGetString(*vkl, "hdri_path", &found, &hdri, error)) return false;
            if (found) effects->virtual_key_light.hdri_path = hdri;
        }
        // Legacy flat boolean (do not error if the canonical object form is present).
        if (const Value* v = Find(*obj, "virtual_key_light")) {
            if (const bool* b = v->AsBool()) {
                effects->virtual_key_light.enabled = *b;
            }
        }
        int vklIntensity = 0;
        if (!TryGetInt(*obj, "virtual_key_light_intensity", &found, &vklIntensity, error)) return false;
        if (found) effects->virtual_key_light.intensity = PercentToFloat01(vklIntensity);
        std::string vklTemp;
        if (!TryGetString(*obj, "virtual_key_light_temperature", &found, &vklTemp, error)) return false;
        if (found) effects->virtual_key_light.temperature_preset = TemperaturePresetFromString(vklTemp);
        int vklPan = 0;
        if (!TryGetInt(*obj, "virtual_key_light_pan", &found, &vklPan, error)) return false;
        if (found) effects->virtual_key_light.direction_pan_degrees = static_cast<float>(ClampInt(vklPan, -180, 180));
        std::string vklHdri;
        if (!TryGetString(*obj, "virtual_key_light_hdri", &found, &vklHdri, error)) return false;
        if (found) effects->virtual_key_light.hdri_path = vklHdri;

        // Vignette.
        if (const auto* vg = GetObj(*obj, "vignette", error)) {
            bool en = effects->vignette.enabled;
            if (!TryGetBool(*vg, "enabled", &found, &en, error)) return false;
            if (found) effects->vignette.enabled = en;
            int intensityPct = 0;
            if (!TryGetInt(*vg, std::string(kIntensity), &found, &intensityPct, error)) return false;
            if (found) effects->vignette.intensity = PercentToFloat01(intensityPct);
            bool center = effects->vignette.center_on_tracked_face;
            if (!TryGetBool(*vg, std::string(kCenterOnTrackedFace), &found, &center, error)) return false;
            if (!found) {
                if (!TryGetBool(*vg, "center_on_face", &found, &center, error)) return false;
            }
            if (found) effects->vignette.center_on_tracked_face = center;
        }
        // Legacy flat boolean (do not error if the canonical object form is present).
        if (const Value* v = Find(*obj, "vignette")) {
            if (const bool* b = v->AsBool()) {
                effects->vignette.enabled = *b;
            }
        }
        int vgIntensity = 0;
        if (!TryGetInt(*obj, "vignette_intensity", &found, &vgIntensity, error)) return false;
        if (found) effects->vignette.intensity = PercentToFloat01(vgIntensity);
        bool vgCenter = effects->vignette.center_on_tracked_face;
        if (!TryGetBool(*obj, "vignette_center_on_face", &found, &vgCenter, error)) return false;
        if (found) effects->vignette.center_on_tracked_face = vgCenter;

        // Green screen.
        if (const auto* gs = GetObj(*obj, "green_screen", error)) {
            int mode = 0;
            if (!TryGetInt(*gs, "mode", &found, &mode, error)) return false;
            if (found) effects->green_screen.mode = static_cast<std::uint32_t>(std::max(0, mode));
            bool temporal = effects->green_screen.temporal;
            if (!TryGetBool(*gs, "temporal", &found, &temporal, error)) return false;
            if (found) effects->green_screen.temporal = temporal;
        }

        // Background effect finalization.
        // Prefer explicit auto-frame enable/disable.
        if (autoFrameEnabledSet) {
            if (autoFrameEnabled) {
                effects->background = studiocast::video::effects::BackgroundEffect::auto_frame;
            } else {
                // If VB mode was specified, apply it; otherwise leave current background.
                if (vbModeSet) {
                    effects->background = (vbMode == studiocast::video::effects::BackgroundEffect::auto_frame)
                                             ? studiocast::video::effects::BackgroundEffect::none
                                             : vbMode;
                } else if (effects->background == studiocast::video::effects::BackgroundEffect::auto_frame) {
                    effects->background = studiocast::video::effects::BackgroundEffect::none;
                }
            }
        } else if (vbModeSet) {
            effects->background = vbMode;
        }

        return true;
    }

    bool ApplyCameraEffectsPatchJsonText(const std::string& jsonText,
                                         CameraEffects* effects,
                                         std::string* error) {
        studiocast::util::json::Value root;
        std::string err;
        if (!studiocast::util::json::Parse(jsonText, &root, &err)) {
            if (error) *error = err;
            return false;
        }
        return ApplyCameraEffectsPatchJson(root, effects, error);
    }

}  // namespace studiocast::video
