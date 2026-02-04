#include <cstdio>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#include "core/ipc/daemon_client.h"
#include "core/util/fs.h"
#include "core/util/json.h"
#include "core/video/effects/broadcast_effect_contract.h"
#include "studiocast/version.h"

namespace {

void Usage(const char* argv0) {
  std::cout
      << "studiocastctl - control StudioCast daemon (studiocastd)\n\n"
      << "Usage:\n"
      << "  " << argv0 << " status\n"
      << "  " << argv0 << " config\n"
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
    if (!res.json.empty()) std::cout << res.json << "\n";
    return true;
  }

  std::cerr << (res.error_json.empty() ? std::string("{\"error\":\"unknown\"}") : res.error_json) << "\n";
  return false;
}

std::optional<int> ParseInt(const std::string& s) {
  try {
    size_t idx = 0;
    int v = std::stoi(s, &idx, 10);
    if (idx != s.size()) return std::nullopt;
    return v;
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<bool> ParseBool01(const std::string& s) {
  if (s == "1" || s == "true") return true;
  if (s == "0" || s == "false") return false;
  return std::nullopt;
}

// Builds the canonical effect-ID keyed patch JSON from legacy cli args.
// This keeps `studiocastctl` user-friendly while ensuring the IPC surface is canonical.
std::string BuildCanonicalEffectsPatchJsonFromArgs(const std::vector<std::string>& args, std::string* error) {
  using studiocast::util::json::EscapeString;
  namespace c = studiocast::video::effects::contract;

  std::optional<bool> mirror;
  std::optional<std::string> background;
  std::optional<std::string> engine;
  std::optional<int> bgStrength;
  std::optional<std::string> removeColor;
  std::optional<std::string> replacePath;

  for (const auto& a : args) {
    const auto eq = a.find('=');
    if (eq == std::string::npos) {
      if (error) *error = "expected key=value argument, got: " + a;
      return {};
    }
    const std::string k = a.substr(0, eq);
    const std::string v = a.substr(eq + 1);

    if (k == "mirror") {
      mirror = ParseBool01(v);
      if (!mirror) {
        if (error) *error = "mirror must be 0|1";
        return {};
      }
    } else if (k == "background") {
      background = v;
    } else if (k == "background_backend") {
      // Canonical key is `engine` with values `auto` or `maxine`.
      engine = (v == "maxine") ? "maxine" : "auto";
    } else if (k == "background_strength") {
      bgStrength = ParseInt(v);
      if (!bgStrength) {
        if (error) *error = "background_strength must be an integer";
        return {};
      }
    } else if (k == "background_remove_color") {
      removeColor = v;
    } else if (k == "background_replace_image") {
      replacePath = v;
    } else {
      // Unknown keys are ignored for forward compatibility.
    }
  }

  std::ostringstream oss;
  bool first = true;
  auto add_raw = [&](const std::string& k, const std::string& rawJson) {
    if (!first) oss << ',';
    first = false;
    oss << '"' << EscapeString(k) << "\":" << rawJson;
  };
  auto add_string = [&](const std::string& k, const std::string& v) {
    add_raw(k, std::string("\"") + EscapeString(v) + "\"");
  };

  oss << '{';

  if (engine) add_string("engine", *engine);

  if (mirror) {
    std::ostringstream m;
    m << '{' << "\"enabled\":" << (*mirror ? "true" : "false") << '}';
    add_raw(std::string(c::kEffectIdMirror), m.str());
  }

  if (background || bgStrength || removeColor || replacePath) {
    const std::string bg = background.value_or("none");
    const bool vbBlur = (bg == "blur");
    const bool vbRemove = (bg == "remove");
    const bool vbReplace = (bg == "replace");
    const bool autoFrame = (bg == "auto_frame");

    auto vb_obj = [&](bool en, bool includeReplace) {
      std::ostringstream o;
      o << '{';
      o << "\"enabled\":" << (en ? "true" : "false");
      if (bgStrength) o << ",\"strength\":" << *bgStrength;
      if (removeColor) o << ",\"remove_color\":\"" << EscapeString(*removeColor) << "\"";
      if (includeReplace && replacePath) o << ",\"replace_path\":\"" << EscapeString(*replacePath) << "\"";
      o << '}';
      return o.str();
    };

    add_raw(std::string(c::kEffectIdVirtualBackgroundBlur), vb_obj(vbBlur && !autoFrame, false));
    add_raw(std::string(c::kEffectIdVirtualBackgroundRemove), vb_obj(vbRemove && !autoFrame, false));
    add_raw(std::string(c::kEffectIdVirtualBackgroundReplace), vb_obj(vbReplace && !autoFrame, true));

    {
      std::ostringstream af;
      af << '{' << "\"enabled\":" << (autoFrame ? "true" : "false") << '}';
      add_raw(std::string(c::kEffectIdAutoFrame), af.str());
    }
  }

  oss << '}';
  return oss.str();
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
    return CallOrDie("GET_CONFIG") ? 0 : 1;
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

      // Convert legacy kv args into the canonical effect-ID keyed JSON patch.
      std::vector<std::string> args;
      for (int i = 3; i < argc; ++i) {
        if (argv[i]) args.emplace_back(argv[i]);
      }
      std::string buildErr;
      const std::string json = BuildCanonicalEffectsPatchJsonFromArgs(args, &buildErr);
      if (json.empty() && !buildErr.empty()) {
        std::cerr << "ERROR: " << buildErr << "\n";
        return 2;
      }
      const std::string req = std::string("SET_VIDEO_EFFECTS_JSON ") + studiocast::util::json::Minify(json);
      return CallOrDie(req) ? 0 : 1;
    }

    std::cerr << "Unknown video subcommand: " << sub << "\n";
    return 2;
  }

  std::cerr << "Unknown command: " << cmd << "\n";
  Usage(argv[0]);
  return 2;
}
