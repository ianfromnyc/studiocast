#include "core/video/effects/broadcast_effects_json.h"

#include <cmath>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>

#include "core/video/effects/broadcast_effect_contract.h"

namespace studiocast::video::effects {
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

std::string JoinPath(std::string_view parent, std::string_view key) {
    if (parent.empty()) return std::string(key);
    return std::string(parent) + "." + std::string(key);
}

void AddWarning(std::vector<std::string>* warnings, const std::string& s) {
    if (warnings) warnings->push_back(s);
}

bool Fail(std::string* error, const std::string& msg) {
    if (error) *error = msg;
    return false;
}

bool CheckUnknownKeys(const Value::Object& obj,
                      const std::set<std::string_view>& allowed,
                      std::string_view path,
                      const BroadcastEffectsJsonParseOptions& options,
                      std::vector<std::string>* warnings,
                      std::string* error) {
    for (const auto& [k, _] : obj) {
        if (allowed.find(k) != allowed.end()) continue;
        const std::string p = JoinPath(path, k);
        if (!options.allow_unknown_keys) {
            return Fail(error, "unknown key '" + p + "'");
        }
        AddWarning(warnings, "ignored unknown key '" + p + "'");
    }
    return true;
}

bool TryGetBool(const Value::Object& obj,
                std::string_view path,
                std::string_view key,
                bool* found,
                bool* out,
                std::string* error) {
    *found = false;
    const Value* v = Find(obj, std::string(key));
    if (!v) return true;
    const bool* b = v->AsBool();
    if (!b) return Fail(error, JoinPath(path, key) + " must be a boolean");
    *found = true;
    *out = *b;
    return true;
}

bool TryGetString(const Value::Object& obj,
                  std::string_view path,
                  std::string_view key,
                  bool* found,
                  std::string* out,
                  std::string* error) {
    *found = false;
    const Value* v = Find(obj, std::string(key));
    if (!v) return true;
    const std::string* s = v->AsString();
    if (!s) return Fail(error, JoinPath(path, key) + " must be a string");
    *found = true;
    *out = *s;
    return true;
}

bool TryGetInt(const Value::Object& obj,
               std::string_view path,
               std::string_view key,
               bool* found,
               int* out,
               std::string* error) {
    *found = false;
    const Value* v = Find(obj, std::string(key));
    if (!v) return true;
    const double* n = v->AsNumber();
    if (!n) return Fail(error, JoinPath(path, key) + " must be a number");

    const double r = std::round(*n);
    if (std::fabs(*n - r) > 1e-9) {
        return Fail(error, JoinPath(path, key) + " must be an integer");
    }

    *found = true;
    *out = static_cast<int>(r);
    return true;
}

const Value::Object* GetObj(const Value::Object& obj,
                            std::string_view path,
                            std::string_view key,
                            std::string* error) {
    const Value* v = Find(obj, std::string(key));
    if (!v) return nullptr;
    const auto* o = v->AsObject();
    if (!o) {
        if (error) *error = JoinPath(path, key) + " must be an object";
        return nullptr;
    }
    return o;
}

bool RequireRangeInt(std::string_view path,
                     int v,
                     int lo,
                     int hi,
                     std::string* error) {
    if (v < lo || v > hi) {
        return Fail(error,
                    std::string(path) + " must be in range " + std::to_string(lo) + ".." + std::to_string(hi));
    }
    return true;
}

// Compatibility: map legacy temperature preset names to an approximate Kelvin.
std::optional<int> KelvinFromPreset(const std::string& s) {
    if (s == "neutral") return 4500;
    if (s == "warm") return 3200;
    if (s == "cool") return 6500;
    return std::nullopt;
}

bool ParseRootObject(const Value& root,
                     const Value::Object** out,
                     std::vector<std::string>* warnings,
                     std::string* error) {
    const auto* obj0 = root.AsObject();
    if (!obj0) return Fail(error, "effects JSON must be an object");

    // Allow wrappers as a convenience for IPC callers.
    if (const auto* ve = AsObjectOrNull(Find(*obj0, "video_effects"))) {
        AddWarning(warnings, "parsed effects from wrapper key 'video_effects'");
        *out = ve;
        return true;
    }
    if (const auto* be = AsObjectOrNull(Find(*obj0, "broadcast_effects"))) {
        AddWarning(warnings, "parsed effects from wrapper key 'broadcast_effects'");
        *out = be;
        return true;
    }
    *out = obj0;
    return true;
}

bool ValidateNoBackgroundConflict(const BroadcastCameraEffects& fx, std::string* error) {
    if (fx.auto_frame.enabled && fx.virtual_background.mode != VirtualBackgroundMode::none) {
        return Fail(error, "auto_frame.enabled cannot be true when virtual_background.mode is not 'none'");
    }
    return true;
}

}  // namespace

std::string BroadcastCameraEffectsToJson(const BroadcastCameraEffects& effects) {
    std::ostringstream oss;
    oss << "{";
    oss << "\"schema_version\":" << effects.schema_version << ",";
    oss << "\"mirror\":" << (effects.mirror ? "true" : "false") << ",";
    oss << "\"engine\":\"" << studiocast::util::json::EscapeString(ToString(effects.engine)) << "\",";

    // Virtual background.
    oss << "\"virtual_background\":{";
    oss << "\"mode\":\"" << studiocast::util::json::EscapeString(ToString(effects.virtual_background.mode)) << "\",";
    oss << "\"strength\":" << effects.virtual_background.strength << ",";
    oss << "\"replace_path\":\"" << studiocast::util::json::EscapeString(effects.virtual_background.replace_path) << "\"";
    oss << "},";

    // Auto frame.
    oss << "\"auto_frame\":{";
    oss << "\"enabled\":" << (effects.auto_frame.enabled ? "true" : "false") << ",";
    oss << "\"strength\":" << effects.auto_frame.strength << ",";
    oss << "\"smoothing\":" << effects.auto_frame.smoothing;
    oss << "},";

    // Eye contact.
    oss << "\"eye_contact\":{";
    oss << "\"enabled\":" << (effects.eye_contact.enabled ? "true" : "false") << ",";
    oss << "\"strength\":" << effects.eye_contact.strength << ",";
    oss << "\"look_away_enabled\":" << (effects.eye_contact.look_away_enabled ? "true" : "false");
    oss << "},";

    // Video noise removal.
    oss << "\"video_noise_removal\":{";
    oss << "\"enabled\":" << (effects.video_noise_removal.enabled ? "true" : "false") << ",";
    oss << "\"strength\":" << effects.video_noise_removal.strength;
    oss << "},";

    // Virtual key light.
    oss << "\"virtual_key_light\":{";
    oss << "\"enabled\":" << (effects.virtual_key_light.enabled ? "true" : "false") << ",";
    oss << "\"intensity\":" << effects.virtual_key_light.intensity << ",";
    oss << "\"temperature\":" << effects.virtual_key_light.temperature;
    oss << "},";

    // Vignette.
    oss << "\"vignette\":{";
    oss << "\"enabled\":" << (effects.vignette.enabled ? "true" : "false") << ",";
    oss << "\"intensity\":" << effects.vignette.intensity;
    oss << "}";

    oss << "}";
    return oss.str();
}

bool ParseBroadcastCameraEffectsJson(const studiocast::util::json::Value& root,
                                    BroadcastCameraEffects* out,
                                    const BroadcastEffectsJsonParseOptions& options,
                                    std::vector<std::string>* warnings,
                                    std::string* error) {
    if (!out) return Fail(error, "output pointer is null");
    *out = BroadcastCameraEffects{};

    const Value::Object* obj = nullptr;
    if (!ParseRootObject(root, &obj, warnings, error)) return false;

    // Root keys.
    if (!CheckUnknownKeys(*obj,
                          {"schema_version",
                           "mirror",
                           "engine",
                           "virtual_background",
                           "auto_frame",
                           "eye_contact",
                           "video_noise_removal",
                           "virtual_key_light",
                           "vignette"},
                          "",
                          options,
                          warnings,
                          error)) {
        return false;
    }

    bool found = false;

    int schema = out->schema_version;
    if (!TryGetInt(*obj, "", "schema_version", &found, &schema, error)) return false;
    if (found) {
        if (schema != kBroadcastEffectsSchemaVersion) {
            return Fail(error,
                        "unsupported schema_version " + std::to_string(schema) +
                            " (expected " + std::to_string(kBroadcastEffectsSchemaVersion) + ")");
        }
        out->schema_version = schema;
    } else {
        AddWarning(warnings, "schema_version missing; assuming " + std::to_string(kBroadcastEffectsSchemaVersion));
        out->schema_version = kBroadcastEffectsSchemaVersion;
    }

    bool mirror = out->mirror;
    if (!TryGetBool(*obj, "", "mirror", &found, &mirror, error)) return false;
    if (found) {
        out->mirror = mirror;
    } else if (options.allow_compat_keys) {
        // Compatibility with legacy object form: { "mirror": { "enabled": true } }
        if (const auto* mo = AsObjectOrNull(Find(*obj, "mirror"))) {
            AddWarning(warnings, "mirror: accepted legacy object form; use boolean 'mirror' instead");
            bool en = out->mirror;
            if (!TryGetBool(*mo, "mirror", "enabled", &found, &en, error)) return false;
            if (found) out->mirror = en;
        }
    }

    std::string engine;
    if (!TryGetString(*obj, "", "engine", &found, &engine, error)) return false;
    if (found) {
        EffectsEnginePreference ep{};
        if (!ParseEffectsEnginePreference(engine, &ep)) {
            return Fail(error, "engine must be 'auto' or 'maxine'");
        }
        out->engine = ep;
    }

    // Virtual background.
    if (const auto* vb = GetObj(*obj, "", "virtual_background", error)) {
        if (!CheckUnknownKeys(*vb, {"mode", "strength", "replace_path"}, "virtual_background", options, warnings, error)) {
            return false;
        }

        std::string mode;
        if (!TryGetString(*vb, "virtual_background", "mode", &found, &mode, error)) return false;
        if (found) {
            VirtualBackgroundMode m{};
            if (!ParseVirtualBackgroundMode(mode, &m)) {
                return Fail(error,
                            "virtual_background.mode must be one of: none, blur, remove, replace");
            }
            out->virtual_background.mode = m;
        }

        int strength = out->virtual_background.strength;
        if (!TryGetInt(*vb, "virtual_background", "strength", &found, &strength, error)) return false;
        if (found) {
            if (!RequireRangeInt("virtual_background.strength",
                                 strength,
                                 contract::kVbStrengthMin,
                                 contract::kVbStrengthMax,
                                 error)) {
                return false;
            }
            out->virtual_background.strength = strength;
        }

        std::string rp;
        if (!TryGetString(*vb, "virtual_background", "replace_path", &found, &rp, error)) return false;
        if (found) out->virtual_background.replace_path = rp;

        if (out->virtual_background.mode == VirtualBackgroundMode::replace &&
            out->virtual_background.replace_path.empty()) {
            return Fail(error, "virtual_background.replace_path is required when mode is 'replace'");
        }
        if (out->virtual_background.mode != VirtualBackgroundMode::replace &&
            !out->virtual_background.replace_path.empty()) {
            AddWarning(warnings,
                       "virtual_background.replace_path is set but mode is not 'replace' (value will be ignored)");
        }
    }

    // Auto frame.
    if (const auto* af = GetObj(*obj, "", "auto_frame", error)) {
        if (!CheckUnknownKeys(*af, {"enabled", "strength", "smoothing"}, "auto_frame", options, warnings, error)) return false;

        bool en = out->auto_frame.enabled;
        if (!TryGetBool(*af, "auto_frame", "enabled", &found, &en, error)) return false;
        if (found) out->auto_frame.enabled = en;

        int strength = out->auto_frame.strength;
        if (!TryGetInt(*af, "auto_frame", "strength", &found, &strength, error)) return false;
        if (found) {
            if (!RequireRangeInt("auto_frame.strength", strength, 0, 100, error)) return false;
            out->auto_frame.strength = strength;
        }

        int smoothing = out->auto_frame.smoothing;
        if (!TryGetInt(*af, "auto_frame", "smoothing", &found, &smoothing, error)) return false;
        if (found) {
            if (!RequireRangeInt("auto_frame.smoothing", smoothing, 0, 100, error)) return false;
            out->auto_frame.smoothing = smoothing;
        }
    }

    // Eye contact.
    if (const auto* ec = GetObj(*obj, "", "eye_contact", error)) {
        if (!CheckUnknownKeys(*ec, {"enabled", "strength", "look_away_enabled"}, "eye_contact", options, warnings, error)) return false;

        bool en = out->eye_contact.enabled;
        if (!TryGetBool(*ec, "eye_contact", "enabled", &found, &en, error)) return false;
        if (found) out->eye_contact.enabled = en;

        int strength = out->eye_contact.strength;
        if (!TryGetInt(*ec, "eye_contact", "strength", &found, &strength, error)) return false;
        if (found) {
            if (!RequireRangeInt("eye_contact.strength", strength, 0, 100, error)) return false;
            out->eye_contact.strength = strength;
        }

        bool lookAway = out->eye_contact.look_away_enabled;
        if (!TryGetBool(*ec, "eye_contact", "look_away_enabled", &found, &lookAway, error)) return false;
        if (found) out->eye_contact.look_away_enabled = lookAway;
    }

    // Video noise removal.
    if (const auto* dn = GetObj(*obj, "", "video_noise_removal", error)) {
        if (!CheckUnknownKeys(*dn, {"enabled", "strength"}, "video_noise_removal", options, warnings, error)) return false;

        bool en = out->video_noise_removal.enabled;
        if (!TryGetBool(*dn, "video_noise_removal", "enabled", &found, &en, error)) return false;
        if (found) out->video_noise_removal.enabled = en;

        int strength = out->video_noise_removal.strength;
        if (!TryGetInt(*dn, "video_noise_removal", "strength", &found, &strength, error)) return false;
        if (found) {
            if (!RequireRangeInt("video_noise_removal.strength", strength, 0, 100, error)) return false;
            out->video_noise_removal.strength = strength;
        }
    }

    // Virtual key light.
    if (const auto* vkl = GetObj(*obj, "", "virtual_key_light", error)) {
        if (!CheckUnknownKeys(*vkl,
                              {"enabled", "intensity", "temperature", "temperature_preset"},
                              "virtual_key_light",
                              options,
                              warnings,
                              error)) {
            return false;
        }

        bool en = out->virtual_key_light.enabled;
        if (!TryGetBool(*vkl, "virtual_key_light", "enabled", &found, &en, error)) return false;
        if (found) out->virtual_key_light.enabled = en;

        int intensity = out->virtual_key_light.intensity;
        if (!TryGetInt(*vkl, "virtual_key_light", "intensity", &found, &intensity, error)) return false;
        if (found) {
            if (!RequireRangeInt("virtual_key_light.intensity", intensity, 0, 100, error)) return false;
            out->virtual_key_light.intensity = intensity;
        }

        int tempK = out->virtual_key_light.temperature;
        if (!TryGetInt(*vkl, "virtual_key_light", "temperature", &found, &tempK, error)) return false;
        if (found) {
            if (!RequireRangeInt("virtual_key_light.temperature", tempK, 1000, 10000, error)) return false;
            out->virtual_key_light.temperature = tempK;
        } else if (options.allow_compat_keys) {
            std::string preset;
            if (!TryGetString(*vkl, "virtual_key_light", "temperature_preset", &found, &preset, error)) return false;
            if (found) {
                const auto k = KelvinFromPreset(preset);
                if (!k) return Fail(error, "virtual_key_light.temperature_preset must be 'neutral', 'warm', or 'cool'");
                AddWarning(warnings,
                           "virtual_key_light.temperature_preset is deprecated; use integer virtual_key_light.temperature (Kelvin)");
                out->virtual_key_light.temperature = *k;
            }
        }
    }

    // Vignette.
    if (const auto* vg = GetObj(*obj, "", "vignette", error)) {
        if (!CheckUnknownKeys(*vg, {"enabled", "intensity"}, "vignette", options, warnings, error)) return false;

        bool en = out->vignette.enabled;
        if (!TryGetBool(*vg, "vignette", "enabled", &found, &en, error)) return false;
        if (found) out->vignette.enabled = en;

        int intensity = out->vignette.intensity;
        if (!TryGetInt(*vg, "vignette", "intensity", &found, &intensity, error)) return false;
        if (found) {
            if (!RequireRangeInt("vignette.intensity", intensity, 0, 100, error)) return false;
            out->vignette.intensity = intensity;
        }
    }

    if (!ValidateNoBackgroundConflict(*out, error)) return false;
    return true;
}

bool ParseBroadcastCameraEffectsJsonText(const std::string& jsonText,
                                        BroadcastCameraEffects* out,
                                        const BroadcastEffectsJsonParseOptions& options,
                                        std::vector<std::string>* warnings,
                                        std::string* error) {
    studiocast::util::json::Value root;
    std::string err;
    if (!studiocast::util::json::Parse(jsonText, &root, &err)) {
        if (error) *error = err;
        return false;
    }
    return ParseBroadcastCameraEffectsJson(root, out, options, warnings, error);
}

}  // namespace studiocast::video::effects
