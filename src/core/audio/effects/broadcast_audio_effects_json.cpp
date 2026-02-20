#include "core/audio/effects/broadcast_audio_effects_json.h"

#include <set>
#include <sstream>
#include <string_view>

namespace studiocast::audio::effects {

using studiocast::util::json::Value;

namespace {

constexpr int kStrengthMin = 0;
constexpr int kStrengthMax = 100;

const Value *Find(const Value::Object &obj, const std::string &key) {
  const auto it = obj.find(key);
  if (it == obj.end())
    return nullptr;
  return &it->second;
}

std::string JoinPath(std::string_view parent, std::string_view key) {
  if (parent.empty())
    return std::string(key);
  return std::string(parent) + "." + std::string(key);
}

void AddWarning(std::vector<std::string> *warnings, const std::string &s) {
  if (warnings)
    warnings->push_back(s);
}

bool Fail(std::string *error, const std::string &msg) {
  if (error)
    *error = msg;
  return false;
}

bool CheckUnknownKeys(const Value::Object &obj,
                      const std::set<std::string_view> &allowed,
                      std::string_view path,
                      const BroadcastAudioEffectsJsonParseOptions &options,
                      std::vector<std::string> *warnings, std::string *error) {
  for (const auto &[k, _] : obj) {
    if (allowed.contains(k))
      continue;
    const std::string msg = JoinPath(path, k) + ": unknown key";
    if (options.allow_unknown_keys) {
      AddWarning(warnings, msg);
      continue;
    }
    return Fail(error, msg);
  }
  return true;
}

bool TryGetBool(const Value::Object &obj, std::string_view path,
                std::string_view key, bool *found, bool *out,
                std::string *error) {
  *found = false;
  const Value *v = Find(obj, std::string(key));
  if (!v)
    return true;
  const bool *b = v->AsBool();
  if (!b)
    return Fail(error, JoinPath(path, key) + " must be a boolean");
  *found = true;
  *out = *b;
  return true;
}

bool TryGetInt(const Value::Object &obj, std::string_view path,
               std::string_view key, bool *found, int *out,
               std::string *error) {
  *found = false;
  const Value *v = Find(obj, std::string(key));
  if (!v)
    return true;
  const double *n = v->AsNumber();
  if (!n)
    return Fail(error, JoinPath(path, key) + " must be a number");
  *found = true;
  *out = static_cast<int>(*n);
  return true;
}

bool TryGetString(const Value::Object &obj, std::string_view path,
                  std::string_view key, bool *found, std::string *out,
                  std::string *error) {
  *found = false;
  const Value *v = Find(obj, std::string(key));
  if (!v)
    return true;
  const std::string *s = v->AsString();
  if (!s)
    return Fail(error, JoinPath(path, key) + " must be a string");
  *found = true;
  *out = *s;
  return true;
}

const Value::Object *GetObj(const Value::Object &obj, std::string_view path,
                            std::string_view key, std::string *error) {
  const Value *v = Find(obj, std::string(key));
  if (!v)
    return nullptr;
  const Value::Object *o = v->AsObject();
  if (!o) {
    Fail(error, JoinPath(path, key) + " must be an object");
    return nullptr;
  }
  return o;
}

bool RequireRangeInt(std::string_view path, int v, int lo, int hi,
                     std::string *error) {
  if (v < lo || v > hi) {
    return Fail(error, std::string(path) + " must be in [" +
                           std::to_string(lo) + ", " + std::to_string(hi) +
                           "]");
  }
  return true;
}

bool ParseRootObject(const Value &root, const Value::Object **out,
                     std::vector<std::string> *warnings, std::string *error) {
  const Value::Object *obj = root.AsObject();
  if (!obj)
    return Fail(error, "root must be a JSON object");
  *out = obj;
  (void)warnings;
  return true;
}

bool ValidateMicExclusivity(const BroadcastAudioEffects &fx,
                            std::string *error) {
  if (fx.microphone.studio_voice_enabled &&
      (fx.microphone.noise_removal_enabled ||
       fx.microphone.room_echo_removal_enabled)) {
    return Fail(error, "microphone.studio_voice_enabled is mutually exclusive "
                       "with microphone.noise_removal_enabled and "
                       "microphone.room_echo_removal_enabled");
  }
  return true;
}

} // namespace

std::string BroadcastAudioEffectsToJson(const BroadcastAudioEffects &effects) {
  std::ostringstream oss;
  oss << "{";
  oss << "\"schema_version\":" << effects.schema_version << ",";

  oss << "\"engine\":\"" << ToString(effects.engine) << "\",";

  oss << "\"microphone\":{";
  oss << "\"model_id\":\""
      << studiocast::util::json::EscapeString(effects.microphone.model_id)
      << "\",";
  oss << "\"model_path\":\""
      << studiocast::util::json::EscapeString(effects.microphone.model_path)
      << "\",";
  oss << "\"noise_removal_enabled\":"
      << (effects.microphone.noise_removal_enabled ? "true" : "false") << ",";
  oss << "\"room_echo_removal_enabled\":"
      << (effects.microphone.room_echo_removal_enabled ? "true" : "false")
      << ",";
  oss << "\"strength\":" << effects.microphone.strength << ",";
  oss << "\"studio_voice_enabled\":"
      << (effects.microphone.studio_voice_enabled ? "true" : "false") << ",";

  oss << "\"aec\":{";
  oss << "\"enabled\":" << (effects.microphone.aec.enabled ? "true" : "false")
      << ",";
  oss << "\"reference_source\":\""
      << studiocast::util::json::EscapeString(
             effects.microphone.aec.reference_source)
      << "\"";
  oss << "},";

  oss << "\"superres\":{";
  oss << "\"enabled\":"
      << (effects.microphone.superres.enabled ? "true" : "false") << ",";
  oss << "\"mode\":\"" << ToString(effects.microphone.superres.mode) << "\"";
  oss << "}";
  oss << "},";

  oss << "\"speaker\":{";
  oss << "\"model_id\":\""
      << studiocast::util::json::EscapeString(effects.speaker.model_id)
      << "\",";
  oss << "\"model_path\":\""
      << studiocast::util::json::EscapeString(effects.speaker.model_path)
      << "\",";
  oss << "\"noise_removal_enabled\":"
      << (effects.speaker.noise_removal_enabled ? "true" : "false") << ",";
  oss << "\"room_echo_removal_enabled\":"
      << (effects.speaker.room_echo_removal_enabled ? "true" : "false") << ",";
  oss << "\"strength\":" << effects.speaker.strength << ",";

  oss << "\"superres\":{";
  oss << "\"enabled\":" << (effects.speaker.superres.enabled ? "true" : "false")
      << ",";
  oss << "\"mode\":\"" << ToString(effects.speaker.superres.mode) << "\"";
  oss << "}";
  oss << "}";

  oss << "}";
  return oss.str();
}

bool ParseBroadcastAudioEffectsJson(
    const studiocast::util::json::Value &root, BroadcastAudioEffects *out,
    const BroadcastAudioEffectsJsonParseOptions &options,
    std::vector<std::string> *warnings, std::string *error) {
  if (!out)
    return Fail(error, "output pointer is null");
  *out = BroadcastAudioEffects{};

  const Value::Object *obj = nullptr;
  if (!ParseRootObject(root, &obj, warnings, error))
    return false;

  if (!CheckUnknownKeys(*obj,
                        {"schema_version", "engine", "microphone", "speaker"},
                        "", options, warnings, error)) {
    return false;
  }

  bool found = false;

  int schema = out->schema_version;
  if (!TryGetInt(*obj, "", "schema_version", &found, &schema, error))
    return false;
  if (found) {
    if (schema != 1 && schema != 2 && schema != 3 &&
        schema != kBroadcastAudioEffectsSchemaVersion) {
      return Fail(
          error, "unsupported schema_version " + std::to_string(schema) +
                     " (expected 1, 2, 3, or " +
                     std::to_string(kBroadcastAudioEffectsSchemaVersion) + ")");
    }
    if (schema != kBroadcastAudioEffectsSchemaVersion) {
      AddWarning(warnings,
                 "schema_version " + std::to_string(schema) +
                     " detected; upgrading to " +
                     std::to_string(kBroadcastAudioEffectsSchemaVersion));
      out->schema_version = kBroadcastAudioEffectsSchemaVersion;
    } else {
      out->schema_version = schema;
    }
  } else {
    AddWarning(warnings,
               "schema_version missing; assuming " +
                   std::to_string(kBroadcastAudioEffectsSchemaVersion));
    out->schema_version = kBroadcastAudioEffectsSchemaVersion;
  }

  {
    std::string engineStr;
    if (!TryGetString(*obj, "", "engine", &found, &engineStr, error))
      return false;
    if (found) {
      AudioEffectsEnginePreference e = out->engine;
      if (!TryParseAudioEffectsEnginePreference(engineStr, &e)) {
        return Fail(error, "engine must be one of \"auto\", \"maxine\", "
                           "\"open_source\", or \"off\"");
      }
      out->engine = e;
    }
  }

  if (const auto *mic = GetObj(*obj, "", "microphone", error)) {
    if (!CheckUnknownKeys(*mic,
                          {"model_id", "model_path", "noise_removal_enabled",
                           "room_echo_removal_enabled", "strength",
                           "studio_voice_enabled", "aec", "superres"},
                          "microphone", options, warnings, error)) {
      return false;
    }

    std::string modelId = out->microphone.model_id;
    if (!TryGetString(*mic, "microphone", "model_id", &found, &modelId, error))
      return false;
    if (found)
      out->microphone.model_id = modelId;

    std::string modelPath = out->microphone.model_path;
    if (!TryGetString(*mic, "microphone", "model_path", &found, &modelPath,
                      error))
      return false;
    if (found)
      out->microphone.model_path = modelPath;

    bool en = out->microphone.noise_removal_enabled;
    if (!TryGetBool(*mic, "microphone", "noise_removal_enabled", &found, &en,
                    error))
      return false;
    if (found)
      out->microphone.noise_removal_enabled = en;

    en = out->microphone.room_echo_removal_enabled;
    if (!TryGetBool(*mic, "microphone", "room_echo_removal_enabled", &found,
                    &en, error))
      return false;
    if (found)
      out->microphone.room_echo_removal_enabled = en;

    int strength = out->microphone.strength;
    if (!TryGetInt(*mic, "microphone", "strength", &found, &strength, error))
      return false;
    if (found) {
      if (!RequireRangeInt("microphone.strength", strength, kStrengthMin,
                           kStrengthMax, error))
        return false;
      out->microphone.strength = strength;
    }

    en = out->microphone.studio_voice_enabled;
    if (!TryGetBool(*mic, "microphone", "studio_voice_enabled", &found, &en,
                    error))
      return false;
    if (found)
      out->microphone.studio_voice_enabled = en;

    if (const auto *aec = GetObj(*mic, "microphone", "aec", error)) {
      if (!CheckUnknownKeys(*aec, {"enabled", "reference_source"},
                            "microphone.aec", options, warnings, error)) {
        return false;
      }

      en = out->microphone.aec.enabled;
      if (!TryGetBool(*aec, "microphone.aec", "enabled", &found, &en, error))
        return false;
      if (found)
        out->microphone.aec.enabled = en;

      std::string ref = out->microphone.aec.reference_source;
      if (!TryGetString(*aec, "microphone.aec", "reference_source", &found,
                        &ref, error))
        return false;
      if (found)
        out->microphone.aec.reference_source = ref;
    }

    if (const auto *sr = GetObj(*mic, "microphone", "superres", error)) {
      if (!CheckUnknownKeys(*sr, {"enabled", "mode"}, "microphone.superres",
                            options, warnings, error))
        return false;

      en = out->microphone.superres.enabled;
      if (!TryGetBool(*sr, "microphone.superres", "enabled", &found, &en,
                      error))
        return false;
      if (found)
        out->microphone.superres.enabled = en;

      std::string mode;
      if (!TryGetString(*sr, "microphone.superres", "mode", &found, &mode,
                        error))
        return false;
      if (found) {
        SuperresMode m = out->microphone.superres.mode;
        if (!TryParseSuperresMode(mode, &m)) {
          return Fail(error, "microphone.superres.mode must be one of "
                             "\"8k_to_16k\" or \"16k_to_48k\"");
        }
        out->microphone.superres.mode = m;
      }
    }
  }

  if (const auto *spk = GetObj(*obj, "", "speaker", error)) {
    if (!CheckUnknownKeys(*spk,
                          {"model_id", "model_path", "noise_removal_enabled",
                           "room_echo_removal_enabled", "strength", "superres"},
                          "speaker", options, warnings, error)) {
      return false;
    }

    std::string modelId = out->speaker.model_id;
    if (!TryGetString(*spk, "speaker", "model_id", &found, &modelId, error))
      return false;
    if (found)
      out->speaker.model_id = modelId;

    std::string modelPath = out->speaker.model_path;
    if (!TryGetString(*spk, "speaker", "model_path", &found, &modelPath, error))
      return false;
    if (found)
      out->speaker.model_path = modelPath;

    bool en = out->speaker.noise_removal_enabled;
    if (!TryGetBool(*spk, "speaker", "noise_removal_enabled", &found, &en,
                    error))
      return false;
    if (found)
      out->speaker.noise_removal_enabled = en;

    bool enEcho = out->speaker.room_echo_removal_enabled;
    if (!TryGetBool(*spk, "speaker", "room_echo_removal_enabled", &found,
                    &enEcho, error))
      return false;
    if (found)
      out->speaker.room_echo_removal_enabled = enEcho;

    int strength = out->speaker.strength;
    if (!TryGetInt(*spk, "speaker", "strength", &found, &strength, error))
      return false;
    if (found) {
      if (!RequireRangeInt("speaker.strength", strength, kStrengthMin,
                           kStrengthMax, error))
        return false;
      out->speaker.strength = strength;
    }

    if (const auto *sr = GetObj(*spk, "speaker", "superres", error)) {
      if (!CheckUnknownKeys(*sr, {"enabled", "mode"}, "speaker.superres",
                            options, warnings, error))
        return false;

      bool en2 = out->speaker.superres.enabled;
      if (!TryGetBool(*sr, "speaker.superres", "enabled", &found, &en2, error))
        return false;
      if (found)
        out->speaker.superres.enabled = en2;

      std::string mode;
      if (!TryGetString(*sr, "speaker.superres", "mode", &found, &mode, error))
        return false;
      if (found) {
        SuperresMode m = out->speaker.superres.mode;
        if (!TryParseSuperresMode(mode, &m)) {
          return Fail(error, "speaker.superres.mode must be one of "
                             "\"8k_to_16k\" or \"16k_to_48k\"");
        }
        out->speaker.superres.mode = m;
      }
    }
  }

  return ValidateMicExclusivity(*out, error);
}

bool ParseBroadcastAudioEffectsJsonText(
    const std::string &jsonText, BroadcastAudioEffects *out,
    const BroadcastAudioEffectsJsonParseOptions &options,
    std::vector<std::string> *warnings, std::string *error) {
  Value root;
  if (!studiocast::util::json::Parse(jsonText, &root, error))
    return false;
  return ParseBroadcastAudioEffectsJson(root, out, options, warnings, error);
}

} // namespace studiocast::audio::effects
