#include <cstdio>
#include <iostream>
#include <string>

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
      << "  " << argv0 << " video effects [mirror=0|1] [background=none|blur|remove|auto_frame] "
      << "[background_backend=auto|cpu|maxine] [background_strength=N]\n"
      << "  " << argv0 << " video effects --from <effects.json>\n\n"
      << "Examples:\n"
      << "  " << argv0 << " status\n"
      << "  " << argv0 << " enable 1\n"
      << "  " << argv0 << " video set input=/dev/video0 output=/dev/video10 width=1280 height=720 fps=30\n"
      << "  " << argv0 << " video effects mirror=1 background=blur background_strength=10\n"
      << "  " << argv0 << " video effects --from effects.json\n";
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

      std::string req = "SET_VIDEO_EFFECTS";
      for (int i = 3; i < argc; ++i) {
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
