#include <cstdio>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

#include "core/ipc/daemon_client.h"
#include "core/util/fs.h"
#include "core/util/json.h"
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
      << "Notes:\n"
      << "- 'config' prints the canonical Broadcast-style effects JSON (GET_CONFIG).\n"
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
    if (res.json.find("\"warnings\"") != std::string::npos) {
      std::cerr << "WARNING: daemon returned warnings (see JSON output)\n";
    }
    if (!res.json.empty()) std::cout << res.json << "\n";
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
