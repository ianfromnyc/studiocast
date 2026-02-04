#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "core/config/daemon_config.h"
#include "core/ipc/daemon_server.h"
#include "core/ipc/daemon_socket.h"
#include "core/maxine/maxine_manager.h"
#include "core/video/broadcast_camera_effects_json.h"
#include "core/video/effects/effect_types.h"
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
        << "  --background MODE         Background effect: none|blur|remove|replace|auto_frame (default: none)\n"
        << "  --background-backend B    Background backend: auto|cpu|maxine (default: auto)\n"
        << "  --background-strength N   Intensity knob (CPU blur radius; default: 8)\n"
        << "  --background-remove-color #RRGGBB  Remove-mode background color (default: #000000)\n"
        << "  --background-replace-image PATH    Replace-mode background image path\n"
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

bool ParseRgbHex(const std::string& raw, std::uint32_t* out) {
    if (!out) return false;
    std::string s = raw;
    // trim (minimal; enough for our tokenized IPC)
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\n' || s.back() == '\r')) s.pop_back();
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\n' || s.front() == '\r')) s.erase(s.begin());

    if (s.empty()) return false;
    if (!s.empty() && s[0] == '#') s.erase(0, 1);
    if (s.size() != 6) return false;

    std::uint32_t v = 0;
    for (const char c : s) {
        v <<= 4u;
        if (c >= '0' && c <= '9') {
            v |= static_cast<std::uint32_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            v |= static_cast<std::uint32_t>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            v |= static_cast<std::uint32_t>(c - 'A' + 10);
        } else {
            return false;
        }
    }
    *out = v;
    return true;
}

std::string FormatRgbHex(std::uint32_t rgb) {
    std::ostringstream oss;
    oss << "#" << std::hex << std::nouppercase << std::setfill('0') << std::setw(6) << (rgb & 0xFFFFFFu);
    return oss.str();
}

std::string ToLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

int ParseKeyLightTemperaturePreset(const std::string& raw, int fallback) {
    const auto v = ToLowerAscii(raw);
    if (v == "0" || v == "neutral") return 0;
    if (v == "1" || v == "warm") return 1;
    if (v == "2" || v == "cool") return 2;
    return fallback;
}

std::string FormatKeyLightTemperaturePreset(int preset) {
    switch (preset) {
        case 1: return "warm";
        case 2: return "cool";
        default: return "neutral";
    }
}

std::string StatusToJson(const studiocast::video::VirtualCameraServiceStatus& st,
                         const studiocast::video::VirtualCameraServiceConfig& cfg,
                         const std::filesystem::path& socketPath,
                         const std::string& maxineJson) {
    std::ostringstream oss;
    oss << "{";
    oss << "\"version\":\"" << JsonEscape(STUDIOCAST_VERSION) << "\",";
    oss << "\"git_sha\":\"" << JsonEscape(STUDIOCAST_GIT_SHA) << "\",";
    oss << "\"socket\":\"" << JsonEscape(socketPath.string()) << "\",";
    oss << "\"service_running\":" << BoolJson(st.service_running) << ",";

    // Global Maxine diagnostics payload (used by GUI/CLI to disable unsupported effects).
    if (!maxineJson.empty()) {
        oss << "\"maxine\":" << maxineJson << ",";
    }

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
    oss << "\"background\":\"" << JsonEscape(studiocast::video::effects::ToString(cfg.pipeline.effects.background)) << "\",";
    oss << "\"background_backend\":\"" << JsonEscape(studiocast::video::effects::ToString(cfg.pipeline.effects.background_backend)) << "\",";
    oss << "\"background_strength\":" << cfg.pipeline.effects.background_strength << ",";
    oss << "\"background_remove_color\":\"" << JsonEscape(FormatRgbHex(cfg.pipeline.effects.background_remove_color_rgb)) << "\",";
    oss << "\"background_replace_image\":\"" << JsonEscape(cfg.pipeline.effects.background_replace_image.string()) << "\",";

    const int vkl_intensity = std::max(0, std::min(100, static_cast<int>(cfg.pipeline.effects.virtual_key_light.intensity * 100.0f)));
    oss << "\"virtual_key_light\":" << BoolJson(cfg.pipeline.effects.virtual_key_light.enabled) << ",";
    oss << "\"virtual_key_light_intensity\":" << vkl_intensity << ",";
    oss << "\"virtual_key_light_temperature\":\"" << JsonEscape(FormatKeyLightTemperaturePreset(cfg.pipeline.effects.virtual_key_light.temperature_preset)) << "\",";
    oss << "\"virtual_key_light_pan\":" << static_cast<int>(cfg.pipeline.effects.virtual_key_light.direction_pan_degrees) << ",";
    oss << "\"virtual_key_light_hdri\":\"" << JsonEscape(cfg.pipeline.effects.virtual_key_light.hdri_path.string()) << "\",";

    oss << "\"pipeline\":{";
    oss << "\"running\":" << BoolJson(st.pipeline.running) << ",";
    oss << "\"starting\":" << BoolJson(st.pipeline.starting) << ",";
    oss << "\"frame_index\":" << st.pipeline.frame_index << ",";
    oss << "\"effects_backends\":\"" << JsonEscape(st.pipeline.effects_backends) << "\",";
    oss << "\"effects_note\":\"" << JsonEscape(st.pipeline.effects_note) << "\"";
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
    oss << "\"mirror\":" << BoolJson(cfg.pipeline.effects.mirror) << ",";
    oss << "\"background\":\"" << JsonEscape(studiocast::video::effects::ToString(cfg.pipeline.effects.background)) << "\",";
    oss << "\"background_backend\":\"" << JsonEscape(studiocast::video::effects::ToString(cfg.pipeline.effects.background_backend)) << "\",";
    oss << "\"background_strength\":" << cfg.pipeline.effects.background_strength << ",";
    oss << "\"background_remove_color\":\"" << JsonEscape(FormatRgbHex(cfg.pipeline.effects.background_remove_color_rgb)) << "\",";
    oss << "\"background_replace_image\":\"" << JsonEscape(cfg.pipeline.effects.background_replace_image.string()) << "\",";

    const int vkl_intensity = std::max(0, std::min(100, static_cast<int>(cfg.pipeline.effects.virtual_key_light.intensity * 100.0f)));
    oss << "\"virtual_key_light\":" << BoolJson(cfg.pipeline.effects.virtual_key_light.enabled) << ",";
    oss << "\"virtual_key_light_intensity\":" << vkl_intensity << ",";
    oss << "\"virtual_key_light_temperature\":\"" << JsonEscape(FormatKeyLightTemperaturePreset(cfg.pipeline.effects.virtual_key_light.temperature_preset)) << "\",";
    oss << "\"virtual_key_light_pan\":" << static_cast<int>(cfg.pipeline.effects.virtual_key_light.direction_pan_degrees) << ",";
    oss << "\"virtual_key_light_hdri\":\"" << JsonEscape(cfg.pipeline.effects.virtual_key_light.hdri_path.string()) << "\",";

    const int vignette_intensity =
        std::max(0, std::min(100, static_cast<int>(cfg.pipeline.effects.vignette.intensity * 100.0f)));
    oss << "\"vignette\":" << BoolJson(cfg.pipeline.effects.vignette.enabled) << ",";
    oss << "\"vignette_intensity\":" << vignette_intensity << ",";
    oss << "\"vignette_center_on_face\":" << BoolJson(cfg.pipeline.effects.vignette.center_on_tracked_face) << ",";

    // Canonical, nested effects model (safe for file paths with spaces).
    oss << "\"video_effects\":"
        << studiocast::video::BroadcastCameraEffectsContractToJson(
               studiocast::video::ToBroadcastCameraEffects(cfg.pipeline.effects));
    oss << "}";
    return oss.str();
}

std::string ExtractRawTailAfterCmd(const std::string& line, const std::string& cmd) {
    std::size_t pos = 0;
    // The cmd is the first token.
    if (line.rfind(cmd, 0) == 0) {
        pos = cmd.size();
    } else {
        // Fallback: find the first occurrence.
        pos = line.find(cmd);
        if (pos == std::string::npos) return {};
        pos += cmd.size();
    }
    while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) ++pos;
    return (pos < line.size()) ? line.substr(pos) : std::string();
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

    if (const auto v = GetArgValue(argc, argv, "--background"); !v.empty()) {
        studiocast::video::effects::BackgroundEffect bg{};
        if (studiocast::video::effects::ParseBackgroundEffect(v, &bg)) {
            cfg.pipeline.effects.background = bg;
        } else {
            std::cerr << "WARN: unknown --background value: " << v << "\n";
        }
    }
    if (const auto v = GetArgValue(argc, argv, "--background-backend"); !v.empty()) {
        studiocast::video::effects::EffectBackend be{};
        if (studiocast::video::effects::ParseEffectBackend(v, &be)) {
            cfg.pipeline.effects.background_backend = be;
        } else {
            std::cerr << "WARN: unknown --background-backend value: " << v << "\n";
        }
    }
    if (const int v = GetArgInt(argc, argv, "--background-strength", -1); v > 0) {
        cfg.pipeline.effects.background_strength = std::max(1, std::min(64, v));
    }

    if (const auto v = GetArgValue(argc, argv, "--background-remove-color"); !v.empty()) {
        std::uint32_t rgb = 0;
        if (ParseRgbHex(v, &rgb)) {
            cfg.pipeline.effects.background_remove_color_rgb = rgb;
        } else {
            std::cerr << "WARN: invalid --background-remove-color (expected #RRGGBB): " << v << "\n";
        }
    }
    if (const auto v = GetArgValue(argc, argv, "--background-replace-image"); !v.empty()) {
        cfg.pipeline.effects.background_replace_image = v;
    }

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

                              // Cache diagnostics to avoid heavy probing on every GUI poll.
                              static std::mutex diagMu;
                              static std::chrono::steady_clock::time_point lastDiag;
                              static std::string lastDiagJson;

                              std::string diagJson;
                              {
                                  std::lock_guard<std::mutex> lock(diagMu);
                                  const auto now = std::chrono::steady_clock::now();
                                  if (lastDiagJson.empty() ||
                                      (now - lastDiag) > std::chrono::seconds(2)) {
                                      studiocast::maxine::MaxineManager mm;
                                      const auto d = mm.Diagnose(/*verbose_probe=*/false);
                                      lastDiagJson = d.ToJson();
                                      lastDiag = now;
                                  }
                                  diagJson = lastDiagJson;
                              }

                              return std::string("OK ") + StatusToJson(st, current, socketPath, diagJson);
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

                          if (pc.cmd == "SET_VIDEO_EFFECTS_JSON") {
                              const std::string jsonText = ExtractRawTailAfterCmd(line, pc.cmd);
                              if (jsonText.empty()) {
                                  return std::string("ERR ") + ErrorJson("SET_VIDEO_EFFECTS_JSON requires a JSON object argument");
                              }

                              std::lock_guard<std::mutex> lock(controlMu);
                              auto newCfg = svc.Config();

                              std::string jerr;
                              auto bfx = studiocast::video::ToBroadcastCameraEffects(newCfg.pipeline.effects);
                              if (!studiocast::video::ApplyBroadcastCameraEffectsPatchJsonText(jsonText, &bfx, &jerr)) {
                                  return std::string("ERR ") + ErrorJson(jerr.empty() ? "invalid effects JSON" : jerr);
                              }
                              newCfg.pipeline.effects = studiocast::video::ToLegacyCameraEffects(bfx);

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

                              if (auto it = pc.kv.find("background"); it != pc.kv.end()) {
                                  studiocast::video::effects::BackgroundEffect bg{};
                                  if (!studiocast::video::effects::ParseBackgroundEffect(it->second, &bg)) {
                                      return std::string("ERR ") + ErrorJson("background must be none|blur|remove|replace|auto_frame");
                                  }
                                  newCfg.pipeline.effects.background = bg;
                              }

                              if (auto it = pc.kv.find("background_backend"); it != pc.kv.end()) {
                                  studiocast::video::effects::EffectBackend be{};
                                  if (!studiocast::video::effects::ParseEffectBackend(it->second, &be)) {
                                      return std::string("ERR ") + ErrorJson("background_backend must be auto|cpu|maxine");
                                  }
                                  newCfg.pipeline.effects.background_backend = be;
                              }

                              if (auto it = pc.kv.find("background_strength"); it != pc.kv.end()) {
                                  const int v = std::atoi(it->second.c_str());
                                  if (v <= 0) {
                                      return std::string("ERR ") + ErrorJson("background_strength must be a positive integer");
                                  }
                                  newCfg.pipeline.effects.background_strength = std::max(1, std::min(64, v));
                              }

                              if (auto it = pc.kv.find("background_remove_color"); it != pc.kv.end()) {
                                  std::uint32_t rgb = 0;
                                  if (!ParseRgbHex(it->second, &rgb)) {
                                      return std::string("ERR ") + ErrorJson("background_remove_color must be #RRGGBB");
                                  }
                                  newCfg.pipeline.effects.background_remove_color_rgb = rgb;
                              }

                              if (auto it = pc.kv.find("background_replace_image"); it != pc.kv.end()) {
                                  newCfg.pipeline.effects.background_replace_image = it->second;
                              }

                              if (auto it = pc.kv.find("virtual_key_light"); it != pc.kv.end()) {
                                  bool en = false;
                                  if (!ParseBoolArg(it->second, &en)) {
                                      return std::string("ERR ") + ErrorJson("virtual_key_light must be 0|1");
                                  }
                                  newCfg.pipeline.effects.virtual_key_light.enabled = en;
                              }

                              if (auto it = pc.kv.find("virtual_key_light_intensity"); it != pc.kv.end()) {
                                  const int v = std::atoi(it->second.c_str());
                                  if (v < 0 || v > 100) {
                                      return std::string("ERR ") + ErrorJson("virtual_key_light_intensity must be 0..100");
                                  }
                                  newCfg.pipeline.effects.virtual_key_light.intensity = static_cast<float>(v) / 100.0f;
                              }

                              if (auto it = pc.kv.find("virtual_key_light_temperature"); it != pc.kv.end()) {
                                  const int preset = ParseKeyLightTemperaturePreset(it->second, -1);
                                  if (preset < 0) {
                                      return std::string("ERR ") + ErrorJson("virtual_key_light_temperature must be neutral|warm|cool");
                                  }
                                  newCfg.pipeline.effects.virtual_key_light.temperature_preset = preset;
                              }

                              if (auto it = pc.kv.find("virtual_key_light_pan"); it != pc.kv.end()) {
                                  const int v = std::atoi(it->second.c_str());
                                  newCfg.pipeline.effects.virtual_key_light.direction_pan_degrees =
                                      static_cast<float>(std::max(-180, std::min(180, v)));
                              }

                              if (auto it = pc.kv.find("virtual_key_light_hdri"); it != pc.kv.end()) {
                                  newCfg.pipeline.effects.virtual_key_light.hdri_path = it->second;
                              }

                              if (auto it = pc.kv.find("vignette"); it != pc.kv.end()) {
                                  bool en = false;
                                  if (!ParseBoolArg(it->second, &en)) {
                                      return std::string("ERR ") + ErrorJson("vignette must be 0|1");
                                  }
                                  newCfg.pipeline.effects.vignette.enabled = en;
                              }

                              if (auto it = pc.kv.find("vignette_intensity"); it != pc.kv.end()) {
                                  const int v = std::atoi(it->second.c_str());
                                  if (v < 0 || v > 100) {
                                      return std::string("ERR ") + ErrorJson("vignette_intensity must be 0..100");
                                  }
                                  newCfg.pipeline.effects.vignette.intensity = static_cast<float>(v) / 100.0f;
                              }

                              if (auto it = pc.kv.find("vignette_center_on_face"); it != pc.kv.end()) {
                                  bool en = false;
                                  if (!ParseBoolArg(it->second, &en)) {
                                      return std::string("ERR ") + ErrorJson("vignette_center_on_face must be 0|1");
                                  }
                                  newCfg.pipeline.effects.vignette.center_on_tracked_face = en;
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
