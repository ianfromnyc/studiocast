#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cctype>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "core/ipc/daemon_client.h"
#include "core/util/fs.h"
#include "core/util/json.h"
#include "core/video/effects/broadcast_effect_contract.h"
#include "studiocast/version.h"

namespace {

using studiocast::util::json::Value;

std::optional<double> ParseDouble(std::string_view s) {
  if (s.empty()) return std::nullopt;
  std::string tmp(s);
  char* end = nullptr;
  const double v = std::strtod(tmp.c_str(), &end);
  if (!end || *end != '\0') return std::nullopt;
  return v;
}

bool ParseBoolArg(std::string_view s, bool* out) {
  if (!out) return false;
  if (s == "1" || s == "true" || s == "TRUE" || s == "True") {
    *out = true;
    return true;
  }
  if (s == "0" || s == "false" || s == "FALSE" || s == "False") {
    *out = false;
    return true;
  }
  return false;
}

std::string ToLower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool IsKnownEffectId(const std::string& id) {
  using namespace studiocast::video::effects::contract;
  return id == kEffectIdMirror ||
         id == kEffectIdVirtualBackgroundBlur ||
         id == kEffectIdVirtualBackgroundRemove ||
         id == kEffectIdVirtualBackgroundReplace ||
         id == kEffectIdAutoFrame ||
         id == kEffectIdEyeContact ||
         id == kEffectIdVideoNoiseRemoval ||
         id == kEffectIdVirtualKeyLight ||
         id == kEffectIdVignette;
}

std::string NormalizeEffectId(std::string id) {
  // Allow a few ergonomic aliases; keep contract IDs canonical.
  if (id == "background_blur" || id == "vb_blur") return std::string(studiocast::video::effects::contract::kEffectIdVirtualBackgroundBlur);
  if (id == "background_remove" || id == "vb_remove") return std::string(studiocast::video::effects::contract::kEffectIdVirtualBackgroundRemove);
  if (id == "background_replace" || id == "vb_replace") return std::string(studiocast::video::effects::contract::kEffectIdVirtualBackgroundReplace);
  return id;
}

const Value::Object* RootObjForEffectsPatch(const Value& root) {
  const auto* o0 = root.AsObject();
  if (!o0) return nullptr;
  if (auto it = o0->find("video_effects"); it != o0->end()) {
    if (const auto* o = it->second.AsObject()) return o;
  }
  if (auto it = o0->find("broadcast_effects"); it != o0->end()) {
    if (const auto* o = it->second.AsObject()) return o;
  }
  return o0;
}

bool ValidateNoCpuOptions(const Value& root, std::vector<std::string>* warnings, std::string* error) {
  const Value::Object* obj = RootObjForEffectsPatch(root);
  if (!obj) return true;

  auto warn = [&](const std::string& w) {
    if (warnings) warnings->push_back(w);
  };

  // Canonical engine selector.
  if (auto it = obj->find("engine"); it != obj->end()) {
    if (const auto* s = it->second.AsString()) {
      const std::string v = ToLower(*s);
      if (v == "cpu") {
        warn("CPU effects are not supported; use engine=auto or engine=maxine");
        if (error) *error = "engine must be 'auto' or 'maxine'";
        return false;
      }
      if (v != "auto" && v != "maxine") {
        if (error) *error = "engine must be 'auto' or 'maxine'";
        return false;
      }
    }
  }

  // Legacy/unknown keys that users might try.
  for (const auto& [k, v] : *obj) {
    const std::string lk = ToLower(k);
    if (lk == "background_backend" || lk == "background-backend" || lk == "backend") {
      if (const auto* s = v.AsString()) {
        const std::string vv = ToLower(*s);
        if (vv == "cpu") {
          warn("CPU backends are not supported (and legacy backend keys are ignored); use engine=auto|maxine");
        } else {
          warn("legacy backend key '" + k + "' is ignored; use engine=auto|maxine");
        }
      }
    }
  }

  return true;
}

bool ReadTextFileOrStdin(const std::string& path, std::string* out) {
  if (!out) return false;
  out->clear();
  if (path == "-" || path.empty()) {
    std::ostringstream ss;
    ss << std::cin.rdbuf();
    *out = ss.str();
    return !out->empty();
  }
  const auto jsonTextOpt = studiocast::util::ReadTextFile(path);
  if (!jsonTextOpt) return false;
  *out = *jsonTextOpt;
  return true;
}

std::optional<int> ParseStrengthForEffectId(const std::string& effectId, std::string_view s, std::string* error) {
  const auto dOpt = ParseDouble(s);
  if (!dOpt) {
    if (error) *error = "invalid number for strength";
    return std::nullopt;
  }
  const double d = *dOpt;

  const bool isVb = effectId.rfind("virtual_background.", 0) == 0;
  if (isVb) {
    // VB strength uses the canonical 1..64 range.
    if (d < 0.0) {
      if (error) *error = "strength must be >= 0";
      return std::nullopt;
    }
    int v = 0;
    if (d <= 1.0) {
      v = static_cast<int>(std::lround(1.0 + d * (studiocast::video::effects::contract::kVbStrengthMax - 1)));
    } else {
      v = static_cast<int>(std::lround(d));
    }
    v = std::max(studiocast::video::effects::contract::kVbStrengthMin,
                 std::min(studiocast::video::effects::contract::kVbStrengthMax, v));
    return v;
  }

  // Most other effects use 0..100-ish percent.
  if (d < 0.0) {
    if (error) *error = "strength must be >= 0";
    return std::nullopt;
  }
  int v = 0;
  if (d <= 1.0) v = static_cast<int>(std::lround(d * 100.0));
  else v = static_cast<int>(std::lround(d));
  v = std::max(0, std::min(100, v));
  return v;
}

std::optional<int> ParsePercent01Or100(std::string_view s, const char* what, std::string* error) {
  const auto dOpt = ParseDouble(s);
  if (!dOpt) {
    if (error) *error = std::string("invalid number for ") + what;
    return std::nullopt;
  }
  double d = *dOpt;
  if (d < 0.0) {
    if (error) *error = std::string(what) + " must be >= 0";
    return std::nullopt;
  }
  int v = 0;
  if (d <= 1.0) v = static_cast<int>(std::lround(d * 100.0));
  else v = static_cast<int>(std::lround(d));
  v = std::max(0, std::min(100, v));
  return v;
}

std::string BuildEnablePatchJson(const std::string& effectId,
                                 bool enabled,
                                 const std::optional<std::string>& engine,
                                 const std::optional<int>& strength,
                                 const std::optional<int>& intensity,
                                 const std::optional<int>& smoothing,
                                 const std::optional<double>& headroom,
                                 const std::optional<bool>& lookAway,
                                 const std::optional<std::string>& removeColor,
                                 const std::optional<std::string>& replacePath,
                                 const std::optional<int>& greenscreenMode,
                                 const std::optional<bool>& greenscreenTemporal,
                                 const std::optional<std::string>& temperaturePreset,
                                 const std::optional<int>& directionPanDegrees,
                                 const std::optional<std::string>& hdriPath,
                                 const std::optional<bool>& centerOnTrackedFace) {
  using studiocast::util::json::EscapeString;
  using namespace studiocast::video::effects::contract;
  std::ostringstream oss;
  oss << '{';
  bool first = true;

  if (engine && !engine->empty()) {
    oss << "\"engine\":\"" << EscapeString(*engine) << "\"";
    first = false;
  }

  if (!first) oss << ',';
  oss << "\"" << EscapeString(effectId) << "\":{";
  oss << "\"" << param::kEnabled << "\":" << (enabled ? "true" : "false");

  if (strength) oss << ",\"" << param::kStrength << "\":" << *strength;
  if (intensity) oss << ",\"" << param::kIntensity << "\":" << *intensity;
  if (smoothing) oss << ",\"" << param::kSmoothing << "\":" << *smoothing;
  if (headroom) oss << ",\"" << param::kHeadroom << "\":" << *headroom;
  if (lookAway) oss << ",\"" << param::kLookAwayEnabled << "\":" << (*lookAway ? "true" : "false");

  if (removeColor) oss << ",\"" << param::kVbRemoveColor << "\":\"" << EscapeString(*removeColor) << "\"";
  if (replacePath) oss << ",\"" << param::kVbReplacePath << "\":\"" << EscapeString(*replacePath) << "\"";
  if (greenscreenMode) oss << ",\"" << param::kGreenscreenMode << "\":" << *greenscreenMode;
  if (greenscreenTemporal) oss << ",\"" << param::kGreenscreenTemporal << "\":" << (*greenscreenTemporal ? "true" : "false");

  if (temperaturePreset) oss << ",\"" << param::kTemperaturePreset << "\":\"" << EscapeString(*temperaturePreset) << "\"";
  if (directionPanDegrees) oss << ",\"" << param::kDirectionPanDegrees << "\":" << *directionPanDegrees;
  if (hdriPath) oss << ",\"" << param::kHdriPath << "\":\"" << EscapeString(*hdriPath) << "\"";

  if (centerOnTrackedFace) oss << ",\"" << param::kCenterOnTrackedFace << "\":" << (*centerOnTrackedFace ? "true" : "false");

  oss << "}}";
  return oss.str();
}

void Usage(const char* argv0) {
  std::cout
      << "studiocastctl - control StudioCast daemon (studiocastd)\n\n"
      << "Usage:\n"
      << "  " << argv0 << " status\n"
      << "  " << argv0 << " config\n"
      << "  " << argv0 << " effects get\n"
      << "  " << argv0 << " effects set --file <effects.json|->\n"
      << "  " << argv0 << " effects enable <effect_id> [--engine auto|maxine] [--strength N|0..1] [--intensity N|0..1] ...\n"
      << "  " << argv0 << " effects disable <effect_id>\n"
      << "  " << argv0 << " enable <0|1>\n"
      << "  " << argv0 << " video set [input=/dev/videoX|auto] [output=/dev/videoY|auto] [width=N] [height=N] [fps=N]\n"
      << "  " << argv0 << " video effects [mirror=0|1] [background=none|blur|remove|replace|auto_frame] "
      << "[background_backend=auto|maxine] [background_strength=N] [background_remove_color=#RRGGBB] [background_replace_image=/path]\n"
      << "  " << argv0 << " video effects --from <effects.json>\n\n"
      << "Examples:\n"
      << "  " << argv0 << " status\n"
      << "  " << argv0 << " enable 1\n"
      << "  " << argv0 << " video set input=/dev/video0 output=/dev/video10 width=1280 height=720 fps=30\n"
      << "  " << argv0 << " video effects mirror=1 background=blur background_strength=10\n"
      << "  " << argv0 << " video effects --from effects.json\n\n"
      << "Notes:\n"
      << "- 'effects get' prints the canonical Broadcast effects JSON (GET_CONFIG).\n"
      << "- 'config' is an alias for 'effects get'.\n"
      << "- 'effects set' expects a JSON patch (file-based, avoids shell quoting issues).\n"
      << "- 'video effects' key=value flags are deprecated and mapped server-side; expect a warning.\n\n"
      << "The canonical effects JSON schema uses effect IDs as keys, e.g.:\n"
      << "  {\n"
      << "    \"mirror\":{\"enabled\":true},\n"
      << "    \"virtual_background.blur\":{\"enabled\":true,\"strength\":8},\n"
      << "    \"auto_frame\":{\"enabled\":false},\n"
      << "    \"eye_contact\":{\"enabled\":false}\n"
      << "  }\n";
}

bool CallOrDie(const std::string& req) {
  studiocast::ipc::DaemonCallResult res;
  std::string err;
  if (!studiocast::ipc::DaemonCall(req, &res, &err)) {
    std::cerr << "ERROR: " << err << "\n";
    return false;
  }

  if (res.ok) {
    if (!res.json.empty()) {
      // Print warnings cleanly to stderr, while keeping stdout machine-readable.
      Value v;
      std::string jerr;
      if (studiocast::util::json::Parse(res.json, &v, &jerr)) {
        if (const auto* o = v.AsObject()) {
          if (auto it = o->find("warnings"); it != o->end()) {
            if (const auto* a = it->second.AsArray()) {
              for (const auto& wv : *a) {
                if (const auto* ws = wv.AsString()) {
                  std::cerr << "WARNING: " << *ws << "\n";
                }
              }
            }
          }
        }
      }
      std::cout << res.json << "\n";
    }
    return true;
  }

  std::cerr << (res.error_json.empty() ? std::string("{\"error\":\"unknown\"}") : res.error_json) << "\n";
  return false;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    Usage(argv[0]);
    return 2;
  }

  const std::string cmd = argv[1];
  if (cmd == "--version" || cmd == "-v") {
    std::printf("studiocastctl %s (%s)\n", STUDIOCAST_VERSION, STUDIOCAST_GIT_SHA);
    return 0;
  }
  if (cmd == "--help" || cmd == "-h" || cmd == "help") {
    Usage(argv[0]);
    return 0;
  }

  if (cmd == "status") {
    return CallOrDie("GET_STATUS") ? 0 : 1;
  }
  if (cmd == "config") {
    // Back-compat alias.
    return CallOrDie("GET_CONFIG") ? 0 : 1;
  }
  if (cmd == "effects") {
    if (argc < 3) {
      Usage(argv[0]);
      return 2;
    }
    const std::string sub = argv[2] ? std::string(argv[2]) : std::string();
    if (sub == "get") {
      return CallOrDie("GET_CONFIG") ? 0 : 1;
    }
    if (sub == "set") {
      std::string file;
      for (int i = 3; i < argc; ++i) {
        const std::string_view a = argv[i] ? std::string_view(argv[i]) : std::string_view();
        if (a == "--file" || a == "--from") {
          if (i + 1 >= argc) {
            std::cerr << "ERROR: --file requires a path (or '-' for stdin)\n";
            return 2;
          }
          file = argv[i + 1] ? std::string(argv[i + 1]) : std::string();
          break;
        }
      }
      if (file.empty()) {
        std::cerr << "ERROR: effects set requires --file <effects.json|->\n";
        return 2;
      }

      std::string jsonText;
      if (!ReadTextFileOrStdin(file, &jsonText)) {
        std::cerr << "ERROR: failed to read effects JSON from: " << file << "\n";
        return 2;
      }

      Value root;
      std::string jerr;
      if (!studiocast::util::json::Parse(jsonText, &root, &jerr)) {
        std::cerr << "ERROR: invalid JSON: " << jerr << "\n";
        return 2;
      }

      std::vector<std::string> warnings;
      std::string verr;
      if (!ValidateNoCpuOptions(root, &warnings, &verr)) {
        for (const auto& w : warnings) std::cerr << "WARNING: " << w << "\n";
        std::cerr << "ERROR: " << verr << "\n";
        return 2;
      }
      for (const auto& w : warnings) std::cerr << "WARNING: " << w << "\n";

      const std::string minified = studiocast::util::json::Minify(jsonText);
      const std::string req = std::string("SET_VIDEO_EFFECTS_JSON ") + minified;
      return CallOrDie(req) ? 0 : 1;
    }

    if (sub == "enable" || sub == "disable") {
      if (argc < 4) {
        std::cerr << "ERROR: effects " << sub << " requires an effect_id\n";
        return 2;
      }
      const bool enabled = (sub == "enable");
      std::string effectId = NormalizeEffectId(argv[3] ? std::string(argv[3]) : std::string());
      if (!IsKnownEffectId(effectId)) {
        std::cerr << "ERROR: unknown effect_id: " << effectId << "\n";
        return 2;
      }

      std::optional<std::string> engine;
      std::optional<int> strength;
      std::optional<int> intensity;
      std::optional<int> smoothing;
      std::optional<double> headroom;
      std::optional<bool> lookAway;
      std::optional<std::string> removeColor;
      std::optional<std::string> replacePath;
      std::optional<int> greenscreenMode;
      std::optional<bool> greenscreenTemporal;
      std::optional<std::string> temperaturePreset;
      std::optional<int> directionPanDegrees;
      std::optional<std::string> hdriPath;
      std::optional<bool> centerOnTrackedFace;

      for (int i = 4; i < argc; ++i) {
        const std::string_view a = argv[i] ? std::string_view(argv[i]) : std::string_view();
        auto needValue = [&](const char* flag) -> std::optional<std::string_view> {
          if (a != flag) return std::nullopt;
          if (i + 1 >= argc || !argv[i + 1]) {
            std::cerr << "ERROR: " << flag << " requires a value\n";
            return std::nullopt;
          }
          ++i;
          return std::string_view(argv[i]);
        };

        if (auto v = needValue("--engine")) {
          const std::string vv = ToLower(std::string(*v));
          if (vv == "cpu") {
            std::cerr << "WARNING: CPU effects are not supported; use engine=auto|maxine\n";
            std::cerr << "ERROR: engine must be auto|maxine\n";
            return 2;
          }
          if (vv != "auto" && vv != "maxine") {
            std::cerr << "ERROR: engine must be auto|maxine\n";
            return 2;
          }
          engine = vv;
          continue;
        }
        if (auto v = needValue("--strength")) {
          std::string perr;
          auto s2 = ParseStrengthForEffectId(effectId, *v, &perr);
          if (!s2) {
            std::cerr << "ERROR: " << perr << "\n";
            return 2;
          }
          strength = *s2;
          continue;
        }
        if (auto v = needValue("--intensity")) {
          std::string perr;
          auto p = ParsePercent01Or100(*v, "intensity", &perr);
          if (!p) {
            std::cerr << "ERROR: " << perr << "\n";
            return 2;
          }
          intensity = *p;
          continue;
        }
        if (auto v = needValue("--smoothing")) {
          auto d = ParseDouble(*v);
          if (!d) {
            std::cerr << "ERROR: invalid number for smoothing\n";
            return 2;
          }
          smoothing = std::max(0, std::min(100, static_cast<int>(std::lround(*d))));
          continue;
        }
        if (auto v = needValue("--headroom")) {
          auto d = ParseDouble(*v);
          if (!d) {
            std::cerr << "ERROR: invalid number for headroom\n";
            return 2;
          }
          headroom = std::max(0.0, std::min(1.0, *d));
          continue;
        }
        if (auto v = needValue("--look-away-enabled")) {
          bool b = false;
          if (!ParseBoolArg(*v, &b)) {
            std::cerr << "ERROR: --look-away-enabled must be 0|1|true|false\n";
            return 2;
          }
          lookAway = b;
          continue;
        }
        if (auto v = needValue("--remove-color")) {
          removeColor = std::string(*v);
          continue;
        }
        if (auto v = needValue("--replace-path")) {
          replacePath = std::string(*v);
          continue;
        }
        if (auto v = needValue("--greenscreen-mode")) {
          auto d = ParseDouble(*v);
          if (!d) {
            std::cerr << "ERROR: invalid number for greenscreen-mode\n";
            return 2;
          }
          greenscreenMode = std::max(0, static_cast<int>(std::lround(*d)));
          continue;
        }
        if (auto v = needValue("--greenscreen-temporal")) {
          bool b = false;
          if (!ParseBoolArg(*v, &b)) {
            std::cerr << "ERROR: --greenscreen-temporal must be 0|1|true|false\n";
            return 2;
          }
          greenscreenTemporal = b;
          continue;
        }
        if (auto v = needValue("--temperature-preset")) {
          temperaturePreset = ToLower(std::string(*v));
          continue;
        }
        if (auto v = needValue("--direction-pan-degrees")) {
          auto d = ParseDouble(*v);
          if (!d) {
            std::cerr << "ERROR: invalid number for direction-pan-degrees\n";
            return 2;
          }
          directionPanDegrees = static_cast<int>(std::lround(*d));
          continue;
        }
        if (auto v = needValue("--hdri-path")) {
          hdriPath = std::string(*v);
          continue;
        }
        if (auto v = needValue("--center-on-tracked-face")) {
          bool b = false;
          if (!ParseBoolArg(*v, &b)) {
            std::cerr << "ERROR: --center-on-tracked-face must be 0|1|true|false\n";
            return 2;
          }
          centerOnTrackedFace = b;
          continue;
        }
      }

      if (enabled && effectId == std::string(studiocast::video::effects::contract::kEffectIdVirtualBackgroundReplace) &&
          !replacePath) {
        std::cerr << "WARNING: virtual_background.replace requires --replace-path; daemon will reject if empty\n";
      }

      const std::string patch = BuildEnablePatchJson(effectId, enabled, engine, strength, intensity, smoothing, headroom,
                                                     lookAway, removeColor, replacePath, greenscreenMode, greenscreenTemporal,
                                                     temperaturePreset, directionPanDegrees, hdriPath, centerOnTrackedFace);
      const std::string req = std::string("SET_VIDEO_EFFECTS_JSON ") + patch;
      return CallOrDie(req) ? 0 : 1;
    }

    std::cerr << "Unknown effects subcommand: " << sub << "\n";
    return 2;
  }
  if (cmd == "enable") {
    if (argc < 3) {
      std::cerr << "enable requires 0|1\n";
      return 2;
    }
    return CallOrDie(std::string("SET_ENABLED ") + argv[2]) ? 0 : 1;
  }

  if (cmd == "video") {
    if (argc < 3) {
      Usage(argv[0]);
      return 2;
    }
    const std::string sub = argv[2];
    if (sub == "set") {
      std::string req = "SET_VIDEO_CONFIG";
      for (int i = 3; i < argc; ++i) {
        req.push_back(' ');
        req.append(argv[i]);
      }
      return CallOrDie(req) ? 0 : 1;
    }
    if (sub == "effects") {
      std::string fromPath;
      for (int i = 3; i < argc; ++i) {
        const std::string_view a = argv[i] ? std::string_view(argv[i]) : std::string_view();
        if (a == "--from") {
          if (i + 1 >= argc) {
            std::cerr << "--from requires a file path\n";
            return 2;
          }
          fromPath = argv[i + 1] ? std::string(argv[i + 1]) : std::string();
          break;
        }
      }

      if (!fromPath.empty()) {
        const auto jsonTextOpt = studiocast::util::ReadTextFile(fromPath);
        if (!jsonTextOpt) {
          std::cerr << "ERROR: failed to read file: " << fromPath << "\n";
          return 2;
        }
        const std::string minified = studiocast::util::json::Minify(*jsonTextOpt);
        const std::string req = std::string("SET_VIDEO_EFFECTS_JSON ") + minified;
        return CallOrDie(req) ? 0 : 1;
      }

      // Legacy key=value flags (server maps into canonical effects and returns a warning).
      std::string req = "SET_VIDEO_EFFECTS";
      for (int i = 3; i < argc; ++i) {
        if (!argv[i]) continue;
        req.push_back(' ');
        req.append(argv[i]);
      }
      return CallOrDie(req) ? 0 : 1;
    }

    std::cerr << "Unknown video subcommand: " << sub << "\n";
    return 2;
  }

  std::cerr << "Unknown command: " << cmd << "\n";
  Usage(argv[0]);
  return 2;
}
