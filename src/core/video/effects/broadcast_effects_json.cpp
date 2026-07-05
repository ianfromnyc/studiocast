#include "core/video/effects/broadcast_effects_json.h"

#include <algorithm>
#include <cmath>
#include <optional>
#include <set>
#include <sstream>
#include <string_view>

#include "core/util/json_helpers.h"
#include "core/util/math.h"
#include "core/video/effects/broadcast_effect_contract.h"
#include "core/video/effects/broadcast_effect_contract_utils.h"

namespace studiocast::video::effects {
namespace {

using studiocast::util::json::Value;
namespace jsonh = studiocast::util::json::helpers;

const Value::Object *AsObjectOrNull(const Value *v) {
  return jsonh::AsObjectOrNull(v);
}

const Value *Find(const Value::Object &obj, const std::string &key) {
  return jsonh::Find(obj, key);
}

std::string JoinPath(std::string_view parent, std::string_view key) {
  return jsonh::JoinPath(parent, key);
}

void AddWarning(std::vector<std::string> *warnings, const std::string &s) {
  jsonh::AddWarning(warnings, s);
}

bool Fail(std::string *error, const std::string &msg) {
  return jsonh::Fail(error, msg);
}

bool CheckUnknownKeys(const Value::Object &obj,
                      const std::set<std::string_view> &allowed,
                      std::string_view path,
                      const BroadcastEffectsJsonParseOptions &options,
                      std::vector<std::string> *warnings, std::string *error) {
  for (const auto &[k, _] : obj) {
    if (allowed.find(k) != allowed.end())
      continue;
    const std::string p = JoinPath(path, k);
    if (!options.allow_unknown_keys) {
      return Fail(error, "unknown key '" + p + "'");
    }
    AddWarning(warnings, "ignored unknown key '" + p + "'");
  }
  return true;
}

bool TryGetBool(const Value::Object &obj, std::string_view path,
                std::string_view key, bool *found, bool *out,
                std::string *error) {
  const auto status = jsonh::TryGetBool(obj, key, out);
  if (status == jsonh::LookupStatus::missing) {
    *found = false;
    return true;
  }
  if (status == jsonh::LookupStatus::wrong_type)
    return Fail(error, JoinPath(path, key) + " must be a boolean");

  *found = true;
  return true;
}

bool TryGetString(const Value::Object &obj, std::string_view path,
                  std::string_view key, bool *found, std::string *out,
                  std::string *error) {
  const auto status = jsonh::TryGetString(obj, key, out);
  if (status == jsonh::LookupStatus::missing) {
    *found = false;
    return true;
  }
  if (status == jsonh::LookupStatus::wrong_type)
    return Fail(error, JoinPath(path, key) + " must be a string");

  *found = true;
  return true;
}

bool TryGetInt(const Value::Object &obj, std::string_view path,
               std::string_view key, bool *found, int *out,
               std::string *error) {
  double n = 0.0;
  const auto status = jsonh::TryGetNumber(obj, key, &n);
  if (status == jsonh::LookupStatus::missing) {
    *found = false;
    return true;
  }
  if (status == jsonh::LookupStatus::wrong_type)
    return Fail(error, JoinPath(path, key) + " must be a number");

  if (!jsonh::ConvertNumberToInt(n, jsonh::IntConversionMode::strict_integer,
                                 out)) {
    return Fail(error, JoinPath(path, key) + " must be an integer");
  }

  *found = true;
  return true;
}

const Value::Object *GetObj(const Value::Object &obj, std::string_view path,
                            std::string_view key, std::string *error) {
  const Value::Object *o = nullptr;
  const auto status = jsonh::TryGetObject(obj, key, &o);
  if (status == jsonh::LookupStatus::missing)
    return nullptr;
  if (status == jsonh::LookupStatus::wrong_type) {
    if (error)
      *error = JoinPath(path, key) + " must be an object";
    return nullptr;
  }
  return o;
}

bool RequireRangeInt(std::string_view path, int v, int lo, int hi,
                     std::string *error) {
  if (v < lo || v > hi) {
    return Fail(error, std::string(path) + " must be in range " +
                           std::to_string(lo) + ".." + std::to_string(hi));
  }
  return true;
}

// Compatibility: map legacy temperature preset names to an approximate Kelvin.
std::optional<int> KelvinFromPreset(const std::string &s) {
  return contract::KelvinFromTemperaturePreset(s);
}

bool ParseRootObject(const Value &root, const Value::Object **out,
                     std::vector<std::string> *warnings, std::string *error) {
  const auto *obj0 = root.AsObject();
  if (!obj0)
    return Fail(error, "effects JSON must be an object");

  // Allow wrappers as a convenience for IPC callers.
  if (const auto *ve = AsObjectOrNull(Find(*obj0, "video_effects"))) {
    AddWarning(warnings, "parsed effects from wrapper key 'video_effects'");
    *out = ve;
    return true;
  }
  if (const auto *be = AsObjectOrNull(Find(*obj0, "broadcast_effects"))) {
    AddWarning(warnings, "parsed effects from wrapper key 'broadcast_effects'");
    *out = be;
    return true;
  }
  *out = obj0;
  return true;
}

int ClampInt(int v, int lo, int hi) {
  return studiocast::util::ClampInt(v, lo, hi);
}

bool TryGetFloat(const Value::Object &obj, std::string_view path,
                 std::string_view key, bool *found, float *out,
                 std::string *error) {
  double n = 0.0;
  const auto status = jsonh::TryGetNumber(obj, key, &n);
  if (status == jsonh::LookupStatus::missing) {
    *found = false;
    return true;
  }
  if (status == jsonh::LookupStatus::wrong_type)
    return Fail(error, JoinPath(path, key) + " must be a number");

  *found = true;
  *out = static_cast<float>(n);
  return true;
}

} // namespace

std::string
BroadcastCameraEffectsToJson(const BroadcastCameraEffects &effects) {
  std::ostringstream oss;
  oss << "{";
  oss << "\"schema_version\":" << effects.schema_version << ",";
  oss << "\"mirror\":" << (effects.mirror ? "true" : "false") << ",";
  oss << "\"engine\":\""
      << studiocast::util::json::EscapeString(ToString(effects.engine))
      << "\",";

  // Virtual background.
  oss << "\"virtual_background\":{";
  oss << "\"mode\":\""
      << studiocast::util::json::EscapeString(
             ToString(effects.virtual_background.mode))
      << "\",";
  oss << "\"model_id\":\""
      << studiocast::util::json::EscapeString(
             effects.virtual_background.model_id)
      << "\",";
  oss << "\"strength\":" << effects.virtual_background.strength << ",";
  oss << "\"replace_path\":\""
      << studiocast::util::json::EscapeString(
             effects.virtual_background.replace_path)
      << "\",";
  oss << "\"remove_color\":\""
      << studiocast::util::json::EscapeString(
             effects.virtual_background.remove_color)
      << "\",";
  oss << "\"greenscreen_mode\":" << effects.virtual_background.greenscreen_mode
      << ",";
  oss << "\"greenscreen_temporal\":"
      << (effects.virtual_background.greenscreen_temporal ? "true" : "false");
  oss << "},";

  // Auto frame.
  oss << "\"auto_frame\":{";
  oss << "\"enabled\":" << (effects.auto_frame.enabled ? "true" : "false")
      << ",";
  oss << "\"strength\":" << effects.auto_frame.strength << ",";
  oss << "\"smoothing\":" << effects.auto_frame.smoothing << ",";
  oss << "\"headroom\":" << effects.auto_frame.headroom << ",";
  oss << "\"model_id\":\""
      << studiocast::util::json::EscapeString(effects.auto_frame.model_id)
      << "\"";
  oss << "},";

  // Eye contact.
  oss << "\"eye_contact\":{";
  oss << "\"enabled\":" << (effects.eye_contact.enabled ? "true" : "false")
      << ",";
  oss << "\"strength\":" << effects.eye_contact.strength << ",";
  oss << "\"look_away_enabled\":"
      << (effects.eye_contact.look_away_enabled ? "true" : "false") << ",";
  oss << "\"model_id\":\""
      << studiocast::util::json::EscapeString(effects.eye_contact.model_id)
      << "\"";
  oss << "},";

  // Video noise removal.
  oss << "\"video_noise_removal\":{";
  oss << "\"enabled\":"
      << (effects.video_noise_removal.enabled ? "true" : "false") << ",";
  oss << "\"strength\":" << effects.video_noise_removal.strength << ",";
  oss << "\"model_id\":\""
      << studiocast::util::json::EscapeString(
             effects.video_noise_removal.model_id)
      << "\"";
  oss << "},";

  // Virtual key light.
  oss << "\"virtual_key_light\":{";
  oss << "\"enabled\":"
      << (effects.virtual_key_light.enabled ? "true" : "false") << ",";
  oss << "\"intensity\":" << effects.virtual_key_light.intensity << ",";
  oss << "\"temperature\":" << effects.virtual_key_light.temperature << ",";
  oss << "\"temperature_preset\":"
      << effects.virtual_key_light.temperature_preset << ",";
  oss << "\"direction_pan_degrees\":"
      << effects.virtual_key_light.direction_pan_degrees << ",";
  oss << "\"hdri_path\":\""
      << studiocast::util::json::EscapeString(
             effects.virtual_key_light.hdri_path)
      << "\"";
  oss << "},";

  // Vignette.
  oss << "\"vignette\":{";
  oss << "\"enabled\":" << (effects.vignette.enabled ? "true" : "false") << ",";
  oss << "\"intensity\":" << effects.vignette.intensity << ",";
  oss << "\"center_on_tracked_face\":"
      << (effects.vignette.center_on_tracked_face ? "true" : "false");
  oss << "}";

  oss << "}";
  return oss.str();
}

bool ParseBroadcastCameraEffectsJson(
    const studiocast::util::json::Value &root, BroadcastCameraEffects *out,
    const BroadcastEffectsJsonParseOptions &options,
    std::vector<std::string> *warnings, std::string *error) {
  if (!out)
    return Fail(error, "output pointer is null");
  *out = BroadcastCameraEffects{};

  const Value::Object *obj = nullptr;
  if (!ParseRootObject(root, &obj, warnings, error))
    return false;

  // Root keys.
  if (!CheckUnknownKeys(*obj,
                        {"schema_version", "mirror", "engine",
                         "virtual_background", "auto_frame", "eye_contact",
                         "video_noise_removal", "virtual_key_light",
                         "vignette"},
                        "", options, warnings, error)) {
    return false;
  }

  bool found = false;

  int schema = out->schema_version;
  if (!TryGetInt(*obj, "", "schema_version", &found, &schema, error))
    return false;
  if (found) {
    if (schema != kBroadcastEffectsSchemaVersion) {
      return Fail(error, "unsupported schema_version " +
                             std::to_string(schema) + " (expected " +
                             std::to_string(kBroadcastEffectsSchemaVersion) +
                             ")");
    }
    out->schema_version = schema;
  } else {
    AddWarning(warnings, "schema_version missing; assuming " +
                             std::to_string(kBroadcastEffectsSchemaVersion));
    out->schema_version = kBroadcastEffectsSchemaVersion;
  }

  bool mirror = out->mirror;
  if (!TryGetBool(*obj, "", "mirror", &found, &mirror, error))
    return false;
  if (found) {
    out->mirror = mirror;
  } else if (options.allow_compat_keys) {
    // Compatibility with legacy object form: { "mirror": { "enabled": true } }
    if (const auto *mo = AsObjectOrNull(Find(*obj, "mirror"))) {
      AddWarning(
          warnings,
          "mirror: accepted legacy object form; use boolean 'mirror' instead");
      bool en = out->mirror;
      if (!TryGetBool(*mo, "mirror", "enabled", &found, &en, error))
        return false;
      if (found)
        out->mirror = en;
    }
  }

  std::string engine;
  if (!TryGetString(*obj, "", "engine", &found, &engine, error))
    return false;
  if (found) {
    EffectsEnginePreference ep{};
    if (!ParseEffectsEnginePreference(engine, &ep)) {
      return Fail(error, "engine must be one of: auto, maxine, open_cuda");
    }
    out->engine = ep;
  }

  // Virtual background.
  if (const auto *vb = GetObj(*obj, "", "virtual_background", error)) {
    if (!CheckUnknownKeys(*vb,
                          {"mode", "model_id", "strength", "replace_path",
                           "remove_color", "greenscreen_mode",
                           "greenscreen_temporal"},
                          "virtual_background", options, warnings, error)) {
      return false;
    }

    std::string mode;
    if (!TryGetString(*vb, "virtual_background", "mode", &found, &mode, error))
      return false;
    if (found) {
      VirtualBackgroundMode m{};
      if (!ParseVirtualBackgroundMode(mode, &m)) {
        return Fail(error, "virtual_background.mode must be one of: none, "
                           "blur, remove, replace");
      }
      out->virtual_background.mode = m;
    }

    std::string modelId;
    if (!TryGetString(*vb, "virtual_background", "model_id", &found, &modelId,
                      error))
      return false;
    if (found)
      out->virtual_background.model_id = modelId;

    int strength = out->virtual_background.strength;
    if (!TryGetInt(*vb, "virtual_background", "strength", &found, &strength,
                   error))
      return false;
    if (found) {
      if (!RequireRangeInt("virtual_background.strength", strength,
                           contract::kVbStrengthMin, contract::kVbStrengthMax,
                           error)) {
        return false;
      }
      out->virtual_background.strength = strength;
    }

    std::string rp;
    if (!TryGetString(*vb, "virtual_background", "replace_path", &found, &rp,
                      error))
      return false;
    if (found)
      out->virtual_background.replace_path = rp;

    std::string rc;
    if (!TryGetString(*vb, "virtual_background", "remove_color", &found, &rc,
                      error))
      return false;
    if (found)
      out->virtual_background.remove_color = rc;

    int gsm = static_cast<int>(out->virtual_background.greenscreen_mode);
    if (!TryGetInt(*vb, "virtual_background", "greenscreen_mode", &found, &gsm,
                   error))
      return false;
    if (found)
      out->virtual_background.greenscreen_mode =
          static_cast<std::uint32_t>(std::max(0, gsm));

    bool gst = out->virtual_background.greenscreen_temporal;
    if (!TryGetBool(*vb, "virtual_background", "greenscreen_temporal", &found,
                    &gst, error))
      return false;
    if (found)
      out->virtual_background.greenscreen_temporal = gst;

    if (out->virtual_background.mode == VirtualBackgroundMode::replace &&
        out->virtual_background.replace_path.empty()) {
      return Fail(
          error,
          "virtual_background.replace_path is required when mode is 'replace'");
    }
    if (out->virtual_background.mode != VirtualBackgroundMode::replace &&
        !out->virtual_background.replace_path.empty()) {
      AddWarning(warnings, "virtual_background.replace_path is set but mode is "
                           "not 'replace' (value will be ignored)");
    }
  }

  // Auto frame.
  if (const auto *af = GetObj(*obj, "", "auto_frame", error)) {
    if (!CheckUnknownKeys(
            *af, {"enabled", "strength", "smoothing", "headroom", "model_id"},
            "auto_frame", options, warnings, error))
      return false;

    bool en = out->auto_frame.enabled;
    if (!TryGetBool(*af, "auto_frame", "enabled", &found, &en, error))
      return false;
    if (found)
      out->auto_frame.enabled = en;

    int strength = out->auto_frame.strength;
    if (!TryGetInt(*af, "auto_frame", "strength", &found, &strength, error))
      return false;
    if (found) {
      if (!RequireRangeInt("auto_frame.strength", strength, 0, 100, error))
        return false;
      out->auto_frame.strength = strength;
    }

    int smoothing = out->auto_frame.smoothing;
    if (!TryGetInt(*af, "auto_frame", "smoothing", &found, &smoothing, error))
      return false;
    if (found) {
      if (!RequireRangeInt("auto_frame.smoothing", smoothing, 0, 100, error))
        return false;
      out->auto_frame.smoothing = smoothing;
    }

    float headroom = out->auto_frame.headroom;
    if (!TryGetFloat(*af, "auto_frame", "headroom", &found, &headroom, error))
      return false;
    if (found) {
      if (headroom < contract::kAutoFrameHeadroomMin ||
          headroom > contract::kAutoFrameHeadroomMax) {
        return Fail(error, "auto_frame.headroom must be in range " +
                               std::to_string(contract::kAutoFrameHeadroomMin) +
                               ".." +
                               std::to_string(contract::kAutoFrameHeadroomMax));
      }
      out->auto_frame.headroom = headroom;
    }

    std::string modelId;
    if (!TryGetString(*af, "auto_frame", "model_id", &found, &modelId, error))
      return false;
    if (found)
      out->auto_frame.model_id = modelId;
  }

  // Eye contact.
  if (const auto *ec = GetObj(*obj, "", "eye_contact", error)) {
    if (!CheckUnknownKeys(
            *ec, {"enabled", "strength", "look_away_enabled", "model_id"},
            "eye_contact", options, warnings, error))
      return false;

    bool en = out->eye_contact.enabled;
    if (!TryGetBool(*ec, "eye_contact", "enabled", &found, &en, error))
      return false;
    if (found)
      out->eye_contact.enabled = en;

    int strength = out->eye_contact.strength;
    if (!TryGetInt(*ec, "eye_contact", "strength", &found, &strength, error))
      return false;
    if (found) {
      if (!RequireRangeInt("eye_contact.strength", strength, 0, 100, error))
        return false;
      out->eye_contact.strength = strength;
    }

    bool lookAway = out->eye_contact.look_away_enabled;
    if (!TryGetBool(*ec, "eye_contact", "look_away_enabled", &found, &lookAway,
                    error))
      return false;
    if (found)
      out->eye_contact.look_away_enabled = lookAway;

    std::string modelId;
    if (!TryGetString(*ec, "eye_contact", "model_id", &found, &modelId, error))
      return false;
    if (found)
      out->eye_contact.model_id = modelId;
  }

  // Video noise removal.
  if (const auto *dn = GetObj(*obj, "", "video_noise_removal", error)) {
    if (!CheckUnknownKeys(*dn, {"enabled", "strength", "model_id"},
                          "video_noise_removal", options, warnings, error))
      return false;

    bool en = out->video_noise_removal.enabled;
    if (!TryGetBool(*dn, "video_noise_removal", "enabled", &found, &en, error))
      return false;
    if (found)
      out->video_noise_removal.enabled = en;

    int strength = out->video_noise_removal.strength;
    if (!TryGetInt(*dn, "video_noise_removal", "strength", &found, &strength,
                   error))
      return false;
    if (found) {
      if (!RequireRangeInt("video_noise_removal.strength", strength, 0, 100,
                           error))
        return false;
      out->video_noise_removal.strength = strength;
    }

    std::string modelId;
    if (!TryGetString(*dn, "video_noise_removal", "model_id", &found, &modelId,
                      error))
      return false;
    if (found)
      out->video_noise_removal.model_id = modelId;
  }

  // Virtual key light.
  if (const auto *vkl = GetObj(*obj, "", "virtual_key_light", error)) {
    if (!CheckUnknownKeys(*vkl,
                          {"enabled", "intensity", "temperature",
                           "temperature_preset", "direction_pan_degrees",
                           "hdri_path"},
                          "virtual_key_light", options, warnings, error)) {
      return false;
    }

    bool en = out->virtual_key_light.enabled;
    if (!TryGetBool(*vkl, "virtual_key_light", "enabled", &found, &en, error))
      return false;
    if (found)
      out->virtual_key_light.enabled = en;

    int intensity = out->virtual_key_light.intensity;
    if (!TryGetInt(*vkl, "virtual_key_light", "intensity", &found, &intensity,
                   error))
      return false;
    if (found) {
      if (!RequireRangeInt("virtual_key_light.intensity", intensity, 0, 100,
                           error))
        return false;
      out->virtual_key_light.intensity = intensity;
    }

    int tempK = out->virtual_key_light.temperature;
    if (!TryGetInt(*vkl, "virtual_key_light", "temperature", &found, &tempK,
                   error))
      return false;
    if (found) {
      if (!RequireRangeInt("virtual_key_light.temperature", tempK, 1000, 10000,
                           error))
        return false;
      out->virtual_key_light.temperature = tempK;
      if (out->virtual_key_light.temperature == 3200)
        out->virtual_key_light.temperature_preset = 1;
      if (out->virtual_key_light.temperature == 6500)
        out->virtual_key_light.temperature_preset = 2;
      if (out->virtual_key_light.temperature == 4500)
        out->virtual_key_light.temperature_preset = 0;
    }

    // Optional preset (deprecated in favor of temperature, but preserved for
    // contract/IPC).
    if (const Value *tp = Find(*vkl, "temperature_preset")) {
      if (const auto *s = tp->AsString()) {
        if (options.allow_compat_keys) {
          const auto k = KelvinFromPreset(*s);
          if (!k)
            return Fail(error, "virtual_key_light.temperature_preset must be "
                               "'neutral', 'warm', or 'cool'");
          AddWarning(warnings,
                     "virtual_key_light.temperature_preset is deprecated; use "
                     "integer virtual_key_light.temperature (Kelvin)");
          out->virtual_key_light.temperature = *k;
          out->virtual_key_light.temperature_preset = (*s == "warm")   ? 1
                                                      : (*s == "cool") ? 2
                                                                       : 0;
        } else {
          return Fail(
              error,
              "virtual_key_light.temperature_preset must be an integer (0..2)");
        }
      } else if (const double *n = tp->AsNumber()) {
        const int r = static_cast<int>(std::lround(*n));
        out->virtual_key_light.temperature_preset = ClampInt(r, 0, 2);
      } else {
        return Fail(
            error,
            "virtual_key_light.temperature_preset must be a string or number");
      }
    }

    int pan = out->virtual_key_light.direction_pan_degrees;
    if (!TryGetInt(*vkl, "virtual_key_light", "direction_pan_degrees", &found,
                   &pan, error))
      return false;
    if (found) {
      if (!RequireRangeInt("virtual_key_light.direction_pan_degrees", pan,
                           contract::kVirtualKeyLightPanMin,
                           contract::kVirtualKeyLightPanMax, error)) {
        return false;
      }
      out->virtual_key_light.direction_pan_degrees = pan;
    }

    std::string hp;
    if (!TryGetString(*vkl, "virtual_key_light", "hdri_path", &found, &hp,
                      error))
      return false;
    if (found)
      out->virtual_key_light.hdri_path = hp;
  }

  // Vignette.
  if (const auto *vg = GetObj(*obj, "", "vignette", error)) {
    if (!CheckUnknownKeys(*vg,
                          {"enabled", "intensity", "center_on_tracked_face"},
                          "vignette", options, warnings, error))
      return false;

    bool en = out->vignette.enabled;
    if (!TryGetBool(*vg, "vignette", "enabled", &found, &en, error))
      return false;
    if (found)
      out->vignette.enabled = en;

    int intensity = out->vignette.intensity;
    if (!TryGetInt(*vg, "vignette", "intensity", &found, &intensity, error))
      return false;
    if (found) {
      if (!RequireRangeInt("vignette.intensity", intensity, 0, 100, error))
        return false;
      out->vignette.intensity = intensity;
    }

    bool center = out->vignette.center_on_tracked_face;
    if (!TryGetBool(*vg, "vignette", "center_on_tracked_face", &found, &center,
                    error))
      return false;
    if (found)
      out->vignette.center_on_tracked_face = center;
  }

  return true;
}

bool ParseBroadcastCameraEffectsJsonText(
    const std::string &jsonText, BroadcastCameraEffects *out,
    const BroadcastEffectsJsonParseOptions &options,
    std::vector<std::string> *warnings, std::string *error) {
  studiocast::util::json::Value root;
  std::string err;
  if (!studiocast::util::json::Parse(jsonText, &root, &err)) {
    if (error)
      *error = err;
    return false;
  }
  return ParseBroadcastCameraEffectsJson(root, out, options, warnings, error);
}

} // namespace studiocast::video::effects
