#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstdio>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "core/config/daemon_config.h"
#include "core/ipc/daemon_server.h"
#include "core/ipc/daemon_socket.h"
#include "core/video/virtual_camera_service.h"
#include "core/video/v4l2loopback.h"
#include "studiocast/version.h"

namespace {

std::atomic_bool g_running{true};

void HandleSignal(int) {
    g_running.store(false);
}

bool HasArg(int argc, char** argv, const std::string& flag) {
    for (int i = 1; i < argc; ++i) {
        if (argv[i] && flag == argv[i]) return true;
    }
    return false;
}

std::string GetArgValue(int argc, char** argv, const std::string& key) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (argv[i] && key == argv[i]) {
            return argv[i + 1] ? std::string(argv[i + 1]) : std::string();
        }
    }
    return {};
}

int GetArgInt(int argc, char** argv, const std::string& key, int fallback) {
    const auto v = GetArgValue(argc, argv, key);
    if (v.empty()) return fallback;
    return std::atoi(v.c_str());
}

void Usage(const char* argv0) {
    std::cout
        << "StudioCast background service (studiocastd)\n\n"
        << "Usage:\n"
        << "  " << argv0 << " [options]\n\n"
        << "Options:\n"
        << "  --input /dev/videoX      Input camera (default: auto)\n"
        << "  --output /dev/videoY     Output v4l2loopback (default: auto)\n"
        << "  --width N                Requested width (default: 1280)\n"
        << "  --height N               Requested height (default: 720)\n"
        << "  --fps N                  Requested fps (default: 30)\n"
        << "  --mirror                 Enable mirror (horizontal flip)\n"
        << "  --poll-ms N              Consumer poll interval (default: 250)\n"
        << "  --stop-grace-ms N        Stop after N ms without consumers (default: 1000)\n"
        << "  --always-on              Run pipeline even with no consumers\n"
        << "  --version                Print version and exit\n"
        << "  -h, --help               Show this help\n\n"
        << "Notes:\n"
        << "  - This daemon does NOT run modprobe for you.\n"
        << "  - Consumer-driven start/stop is based on scanning /proc/*/fd for open handles\n"
        << "    to the v4l2loopback device (best-effort; typically works when OBS/Zoom run\n"
        << "    under the same user).\n";
}

std::string ChooseWritableLoopbackDevice() {
    const auto rep = studiocast::video::ProbeLoopback();
    for (const auto& d : rep.devices) {
        if (d.is_loopback && d.can_write) return d.dev_node;
    }
    return {};
}

// -----------------------------
// IPC helpers
// -----------------------------

std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 16);
    for (char c : s) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '"': out += "\\\""; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (static_cast<unsigned char>(c) < 0x20) {
                    // Control chars -> \u00XX
                    const char* hex = "0123456789abcdef";
                    out += "\\u00";
                    out.push_back(hex[(c >> 4) & 0xF]);
                    out.push_back(hex[c & 0xF]);
                } else {
                    out.push_back(c);
                }
        }
    }
    return out;
}

std::string BoolJson(bool v) { return v ? "true" : "false"; }

struct ParsedCommand {
    std::string cmd;
    std::map<std::string, std::string> kv;
    std::vector<std::string> args;
};

ParsedCommand ParseLine(const std::string& line) {
    std::istringstream iss(line);
    ParsedCommand pc;
    iss >> pc.cmd;
    std::string tok;
    while (iss >> tok) {
        const auto eq = tok.find('=');
        if (eq != std::string::npos) {
            pc.kv[tok.substr(0, eq)] = tok.substr(eq + 1);
        } else {
            pc.args.push_back(tok);
        }
    }
    return pc;
}

bool ParseBoolArg(const std::string& raw, bool* out) {
    if (!out) return false;
    std::string s = raw;
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    if (s == "1" || s == "true" || s == "yes" || s == "on" || s == "enable" || s == "enabled") {
        *out = true;
        return true;
    }
    if (s == "0" || s == "false" || s == "no" || s == "off" || s == "disable" || s == "disabled") {
        *out = false;
        return true;
    }
    return false;
}

std::string StatusToJson(const studiocast::video::VirtualCameraServiceStatus& st,
                         const studiocast::video::VirtualCameraServiceConfig& cfg,
                         const std::filesystem::path& socketPath) {
    std::ostringstream oss;
    oss << "{";
    oss << "\"version\":\"" << JsonEscape(STUDIOCAST_VERSION) << "\",";
    oss << "\"git_sha\":\"" << JsonEscape(STUDIOCAST_GIT_SHA) << "\",";
    oss << "\"socket\":\"" << JsonEscape(socketPath.string()) << "\",";
    oss << "\"service_running\":" << BoolJson(st.service_running) << ",";

    oss << "\"video\":{";
    oss << "\"enabled\":" << BoolJson(cfg.enabled) << ",";
    oss << "\"always_on\":" << BoolJson(cfg.always_on) << ",";
    oss << "\"consumer_present\":" << BoolJson(st.consumer_present) << ",";
    oss << "\"consumer_count\":" << st.consumer_count << ",";

    oss << "\"input_device\":\"" << JsonEscape(st.pipeline.input_device) << "\",";
    oss << "\"output_device\":\"" << JsonEscape(st.pipeline.output_device) << "\",";

    oss << "\"width\":" << cfg.pipeline.width << ",";
    oss << "\"height\":" << cfg.pipeline.height << ",";
    oss << "\"fps\":" << cfg.pipeline.fps << ",";
    oss << "\"mirror\":" << BoolJson(cfg.pipeline.effects.mirror) << ",";

    oss << "\"pipeline\":{";
    oss << "\"running\":" << BoolJson(st.pipeline.running) << ",";
    oss << "\"starting\":" << BoolJson(st.pipeline.starting) << ",";
    oss << "\"frame_index\":" << st.pipeline.frame_index;
    oss << "},";

    oss << "\"last_error\":\"" << JsonEscape(st.last_error) << "\"";
    oss << "}";  // video

    oss << "}";
    return oss.str();
}

std::string ConfigToJson(const studiocast::video::VirtualCameraServiceConfig& cfg) {
    std::ostringstream oss;
    oss << "{";
    oss << "\"enabled\":" << BoolJson(cfg.enabled) << ",";
    oss << "\"always_on\":" << BoolJson(cfg.always_on) << ",";
    oss << "\"consumer_poll_ms\":" << cfg.consumer_poll_ms << ",";
    oss << "\"stop_grace_ms\":" << cfg.stop_grace_ms << ",";
    oss << "\"input_device\":\"" << JsonEscape(cfg.pipeline.input_device) << "\",";
    oss << "\"output_device\":\"" << JsonEscape(cfg.pipeline.output_device) << "\",";
    oss << "\"width\":" << cfg.pipeline.width << ",";
    oss << "\"height\":" << cfg.pipeline.height << ",";
    oss << "\"fps\":" << cfg.pipeline.fps << ",";
    oss << "\"mirror\":" << BoolJson(cfg.pipeline.effects.mirror);
    oss << "}";
    return oss.str();
}

std::string ErrorJson(const std::string& msg) {
    return std::string("{\"error\":\"") + JsonEscape(msg) + "\"}";
}

}  // namespace

int main(int argc, char** argv) {
    std::signal(SIGINT, HandleSignal);
    std::signal(SIGTERM, HandleSignal);

    if (HasArg(argc, argv, "--version") || HasArg(argc, argv, "-v")) {
        std::printf("studiocastd %s (%s)\n", STUDIOCAST_VERSION, STUDIOCAST_GIT_SHA);
        return 0;
    }

    if (HasArg(argc, argv, "--help") || HasArg(argc, argv, "-h")) {
        Usage(argv[0]);
        return 0;
    }

    // Load persisted config and then apply CLI overrides for this run.
    auto daemonCfg = studiocast::config::LoadDaemonConfig();
    studiocast::video::VirtualCameraServiceConfig cfg = studiocast::config::ToVideoServiceConfig(daemonCfg);

    if (const auto v = GetArgValue(argc, argv, "--input"); !v.empty()) cfg.pipeline.input_device = v;
    if (const auto v = GetArgValue(argc, argv, "--output"); !v.empty()) cfg.pipeline.output_device = v;
    cfg.pipeline.width = GetArgInt(argc, argv, "--width", cfg.pipeline.width);
    cfg.pipeline.height = GetArgInt(argc, argv, "--height", cfg.pipeline.height);
    cfg.pipeline.fps = GetArgInt(argc, argv, "--fps", cfg.pipeline.fps);

    if (HasArg(argc, argv, "--mirror")) cfg.pipeline.effects.mirror = true;

    if (const int v = GetArgInt(argc, argv, "--poll-ms", -1); v > 0) cfg.consumer_poll_ms = v;
    if (const int v = GetArgInt(argc, argv, "--stop-grace-ms", -1); v > 0) cfg.stop_grace_ms = v;
    if (HasArg(argc, argv, "--always-on")) cfg.always_on = true;

    if (cfg.pipeline.output_device.empty()) {
        // If possible, pre-fill output for a nicer startup experience.
        cfg.pipeline.output_device = ChooseWritableLoopbackDevice();
    }

    const auto rep = studiocast::video::ProbeLoopback();
    if (!rep.ReadyForVirtualCamera()) {
        std::cout << rep.ToText() << "\n\n";
    }

    std::cout << "studiocastd " << STUDIOCAST_VERSION << " (" << STUDIOCAST_GIT_SHA << ")\n";
    std::cout << "Virtual camera supervisor started. Press Ctrl+C to stop.\n\n";

    studiocast::video::VirtualCameraService svc;
    std::string err;
    if (!svc.Start(cfg, &err)) {
        std::cerr << "ERROR: " << err << "\n";
        return 1;
    }

    // Start IPC server for GUI / studiocastctl.
    studiocast::ipc::DaemonServer server;
    std::string sockErr;
    const auto socketPath = studiocast::ipc::DaemonSocketPath(&sockErr);
    if (socketPath.empty()) {
        std::cerr << "ERROR: failed to compute socket path: " << sockErr << "\n";
        svc.Stop();
        return 2;
    }

    std::mutex controlMu;

    std::string serverErr;
    if (!server.Start(socketPath,
                      [&](const std::string& line) -> std::string {
                          const auto pc = ParseLine(line);

                          if (pc.cmd == "PING") {
                              return std::string("OK {\"pong\":true}");
                          }

                          if (pc.cmd == "GET_STATUS") {
                              const auto st = svc.Status();
                              const auto current = svc.Config();
                              return std::string("OK ") + StatusToJson(st, current, socketPath);
                          }

                          if (pc.cmd == "GET_CONFIG") {
                              const auto current = svc.Config();
                              return std::string("OK ") + ConfigToJson(current);
                          }

                          if (pc.cmd == "SET_ENABLED") {
                              bool enabled = true;
                              bool ok = false;
                              if (auto it = pc.kv.find("enabled"); it != pc.kv.end()) {
                                  ok = ParseBoolArg(it->second, &enabled);
                              } else if (!pc.args.empty()) {
                                  ok = ParseBoolArg(pc.args[0], &enabled);
                              }

                              if (!ok) {
                                  return std::string("ERR ") + ErrorJson("SET_ENABLED requires enabled=0|1");
                              }

                              std::lock_guard<std::mutex> lock(controlMu);
                              auto newCfg = svc.Config();
                              newCfg.enabled = enabled;
                              svc.UpdateConfig(newCfg);

                              studiocast::config::ApplyVideoServiceConfigToDaemonConfig(newCfg, &daemonCfg);
                              std::string perr;
                              (void)studiocast::config::SaveDaemonConfig(daemonCfg, &perr);

                              return std::string("OK {\"enabled\":") + BoolJson(enabled) + "}";
                          }

                          if (pc.cmd == "SET_VIDEO_CONFIG") {
                              std::lock_guard<std::mutex> lock(controlMu);
                              auto newCfg = svc.Config();

                              if (auto it = pc.kv.find("input"); it != pc.kv.end()) {
                                  newCfg.pipeline.input_device = (it->second == "auto") ? std::string() : it->second;
                              }
                              if (auto it = pc.kv.find("output"); it != pc.kv.end()) {
                                  newCfg.pipeline.output_device = (it->second == "auto") ? std::string() : it->second;
                              }
                              if (auto it = pc.kv.find("width"); it != pc.kv.end()) {
                                  newCfg.pipeline.width = std::atoi(it->second.c_str());
                              }
                              if (auto it = pc.kv.find("height"); it != pc.kv.end()) {
                                  newCfg.pipeline.height = std::atoi(it->second.c_str());
                              }
                              if (auto it = pc.kv.find("fps"); it != pc.kv.end()) {
                                  newCfg.pipeline.fps = std::atoi(it->second.c_str());
                              }

                              svc.UpdateConfig(newCfg);

                              studiocast::config::ApplyVideoServiceConfigToDaemonConfig(newCfg, &daemonCfg);
                              std::string perr;
                              (void)studiocast::config::SaveDaemonConfig(daemonCfg, &perr);

                              return std::string("OK ") + ConfigToJson(newCfg);
                          }

                          if (pc.cmd == "SET_VIDEO_EFFECTS") {
                              std::lock_guard<std::mutex> lock(controlMu);
                              auto newCfg = svc.Config();

                              if (auto it = pc.kv.find("mirror"); it != pc.kv.end()) {
                                  bool mirror = false;
                                  if (!ParseBoolArg(it->second, &mirror)) {
                                      return std::string("ERR ") + ErrorJson("mirror must be 0|1");
                                  }
                                  newCfg.pipeline.effects.mirror = mirror;
                              }

                              svc.UpdateConfig(newCfg);

                              studiocast::config::ApplyVideoServiceConfigToDaemonConfig(newCfg, &daemonCfg);
                              std::string perr;
                              (void)studiocast::config::SaveDaemonConfig(daemonCfg, &perr);

                              return std::string("OK ") + ConfigToJson(newCfg);
                          }

                          return std::string("ERR ") + ErrorJson("unknown_command");
                      },
                      &serverErr)) {
        std::cerr << "ERROR: failed to start IPC server: " << serverErr << "\n";
        svc.Stop();
        return 3;
    }

    studiocast::video::VirtualCameraServiceStatus prev;

    // Print initial status once so users immediately see which devices were selected
    // (especially when running in auto mode).
    {
        const auto st = svc.Status();
        std::cout << "[status] consumers=" << st.consumer_count
                  << " running=" << (st.pipeline.running ? "yes" : (st.pipeline.starting ? "starting" : "no"))
                  << " in=" << (st.pipeline.input_device.empty() ? "(auto)" : st.pipeline.input_device)
                  << " out=" << (st.pipeline.output_device.empty() ? "(auto)" : st.pipeline.output_device)
                  << "\n";
        if (!st.last_error.empty()) {
            std::cout << "[last_error] " << st.last_error << "\n";
        }
        std::cout.flush();
        prev = st;
    }

    while (g_running.load()) {
        const auto st = svc.Status();

        // Print state transitions.
        if (st.consumer_present != prev.consumer_present ||
            st.pipeline.running != prev.pipeline.running ||
            st.pipeline.starting != prev.pipeline.starting ||
            st.pipeline.input_device != prev.pipeline.input_device ||
            st.pipeline.output_device != prev.pipeline.output_device ||
            st.last_error != prev.last_error) {

            std::cout << "[status] consumers=" << st.consumer_count
                      << " running=" << (st.pipeline.running ? "yes" : (st.pipeline.starting ? "starting" : "no"))
                      << " in=" << (st.pipeline.input_device.empty() ? "(auto)" : st.pipeline.input_device)
                      << " out=" << (st.pipeline.output_device.empty() ? "(auto)" : st.pipeline.output_device)
                      << "\n";

            if (!st.last_error.empty()) {
                std::cout << "[last_error] " << st.last_error << "\n";
            }

            std::cout.flush();
            prev = st;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }

    std::cout << "\nStopping...\n";
    server.Stop();
    svc.Stop();
    return 0;
}
