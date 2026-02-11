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

#include "core/audio/effects/broadcast_audio_effects_json.h"
#include "core/config/daemon_config.h"
#include "core/ipc/daemon_server.h"
#include "core/ipc/daemon_socket.h"
#include "core/maxine/maxine_manager.h"
#include "core/open_cuda/open_cuda_diagnose.h"
#include "core/open_cuda/model_pack_registry.h"
#include "core/util/json.h"
#include "core/util/ttl_cache.h"
#include "core/util/xdg.h"
#include "core/video/broadcast_camera_effects_json.h"
#include "core/video/camera_effects_json.h"
#include "core/video/effects/broadcast_effect_contract.h"
#include "core/video/effects/broadcast_effect_rules.h"
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
        << "  --capture-mode M         Capture mode: requested|auto (default: requested)\n"
        << "  --width N                Requested width (default: 1280)\n"
        << "  --height N               Requested height (default: 720)\n"
        << "  --fps N                  Requested fps (default: 30)\n"
        << "  --mirror                 Enable mirror (horizontal flip)\n"
        << "  --background MODE         Background effect: none|blur|remove|replace|auto_frame (default: none)\n"
        << "  --background-backend B    Effects engine preference: auto|maxine|open_cuda (default: auto)\n"
        << "  --background-strength N   Intensity knob (default: 8)\n"
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

static double FpsToDouble(int fps, int fps_num, int fps_den) {
    if (fps_den > 0 && fps_num > 0) {
        return static_cast<double>(fps_num) / static_cast<double>(fps_den);
    }
    return static_cast<double>(fps);
}

static std::string CaptureFormatToJson(const studiocast::video::CaptureFormat& f) {
    std::ostringstream oss;
    oss << "{";
    oss << "\"pixfmt\":\"" << JsonEscape(f.pixfmt) << "\",";
    oss << "\"width\":" << f.width << ",";
    oss << "\"height\":" << f.height << ",";
    oss << "\"fps\":" << std::setprecision(6) << FpsToDouble(f.fps, f.fps_num, f.fps_den) << ",";
    oss << "\"fps_num\":" << f.fps_num << ",";
    oss << "\"fps_den\":" << f.fps_den << ",";
    oss << "\"bytesperline\":" << f.bytes_per_line << ",";
    oss << "\"sizeimage\":" << f.size_image;
    oss << "}";
    return oss.str();
}

static std::string ActualFormatToJson(const studiocast::video::ActualFormat& f) {
    std::ostringstream oss;
    oss << "{";
    oss << "\"pixfmt\":\"" << JsonEscape(f.pixfmt) << "\",";
    oss << "\"width\":" << f.width << ",";
    oss << "\"height\":" << f.height << ",";
    oss << "\"fps\":" << std::setprecision(6) << FpsToDouble(f.fps, f.fps_num, f.fps_den) << ",";
    oss << "\"fps_num\":" << f.fps_num << ",";
    oss << "\"fps_den\":" << f.fps_den << ",";
    oss << "\"bytesperline\":" << f.bytes_per_line << ",";
    oss << "\"sizeimage\":" << f.size_image;
    oss << "}";
    return oss.str();
}

std::string StatusToJson(const studiocast::video::VirtualCameraServiceStatus& st,
                         const studiocast::video::VirtualCameraServiceConfig& cfg,
                         const studiocast::audio::VirtualAudioServiceStatus& ast,
                         const studiocast::audio::VirtualAudioServiceConfig& acfg,
                         const std::filesystem::path& socketPath,
                         const std::string& maxineJson,
                         const std::string& openCudaJson) {
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

    // Engine diagnostics (preferred scalable shape). Keep top-level `maxine` for compatibility.
    oss << "\"engines\":{";
    if (!maxineJson.empty()) {
        oss << "\"maxine\":" << maxineJson << ",";
    }
    oss << "\"open_cuda\":" << (openCudaJson.empty() ? std::string("{}") : openCudaJson);
    oss << "},";

    // Convenience top-level alias.
    if (!openCudaJson.empty()) {
        oss << "\"open_cuda\":" << openCudaJson << ",";
    }

    oss << "\"video\":{";
    oss << "\"enabled\":" << BoolJson(cfg.enabled) << ",";
    oss << "\"always_on\":" << BoolJson(cfg.always_on) << ",";
    oss << "\"consumer_present\":" << BoolJson(st.consumer_present) << ",";
    oss << "\"consumer_count\":" << st.consumer_count << ",";

    oss << "\"input_device\":\"" << JsonEscape(st.pipeline.input_device) << "\",";
    oss << "\"output_device\":\"" << JsonEscape(st.pipeline.output_device) << "\",";

    // Negotiated formats (what the driver actually gave us / accepted).
    oss << "\"capture_format\":" << CaptureFormatToJson(st.pipeline.capture) << ",";
    oss << "\"output_format\":" << ActualFormatToJson(st.pipeline.output) << ",";

    // Scaling status (active backend + from/to formats).
    oss << "\"scaling\":{";
    oss << "\"backend_active\":\"" << JsonEscape(st.pipeline.scaling_backend_active) << "\",";
    oss << "\"from\":" << CaptureFormatToJson(st.pipeline.scaling_from) << ",";
    oss << "\"to\":" << ActualFormatToJson(st.pipeline.scaling_to);
    oss << "},";

    const char* capture_mode_label =
        (cfg.pipeline.capture_mode == studiocast::video::CaptureMode::auto_best) ? "auto_best" : "requested";
    oss << "\"capture_mode\":\"" << capture_mode_label << "\",";

    oss << "\"width\":" << cfg.pipeline.width << ",";
    oss << "\"height\":" << cfg.pipeline.height << ",";
    oss << "\"fps\":" << cfg.pipeline.fps << ",";
    oss << "\"mirror\":" << BoolJson(cfg.pipeline.effects.mirror) << ",";
    // Legacy flat fields (kept for compatibility): derived from the canonical Broadcast schema.
    oss << "\"background\":\"" << JsonEscape(studiocast::video::effects::ToString(cfg.pipeline.effects.virtual_background.mode)) << "\",";
    oss << "\"background_backend\":\"" << JsonEscape(studiocast::video::effects::ToString(cfg.pipeline.effects.engine)) << "\",";
    oss << "\"background_strength\":" << cfg.pipeline.effects.virtual_background.strength << ",";
    oss << "\"background_remove_color\":\"" << JsonEscape(cfg.pipeline.effects.virtual_background.remove_color) << "\",";
    oss << "\"background_replace_image\":\"" << JsonEscape(cfg.pipeline.effects.virtual_background.replace_path) << "\",";

    // Canonical effect model (Broadcast schema) for GUI/CLI.
    oss << "\"video_effects\":"
        << studiocast::video::BroadcastCameraEffectsContractToJson(cfg.pipeline.effects)
        << ",";

    const int vkl_intensity = std::max(0, std::min(100, cfg.pipeline.effects.virtual_key_light.intensity));
    oss << "\"virtual_key_light\":" << BoolJson(cfg.pipeline.effects.virtual_key_light.enabled) << ",";
    oss << "\"virtual_key_light_intensity\":" << vkl_intensity << ",";
    oss << "\"virtual_key_light_temperature\":\"" << JsonEscape(FormatKeyLightTemperaturePreset(cfg.pipeline.effects.virtual_key_light.temperature_preset)) << "\",";
    oss << "\"virtual_key_light_pan\":" << cfg.pipeline.effects.virtual_key_light.direction_pan_degrees << ",";
    oss << "\"virtual_key_light_hdri\":\"" << JsonEscape(cfg.pipeline.effects.virtual_key_light.hdri_path) << "\",";

    oss << "\"pipeline\":{";
    oss << "\"running\":" << BoolJson(st.pipeline.running) << ",";
    oss << "\"starting\":" << BoolJson(st.pipeline.starting) << ",";
    oss << "\"frame_index\":" << st.pipeline.frame_index << ",";
    oss << "\"effects_backends\":\"" << JsonEscape(st.pipeline.effects_backends) << "\",";
    oss << "\"effects_note\":\"" << JsonEscape(st.pipeline.effects_note) << "\",";

    // Lightweight rolling perf counters for quick CPU vs GPU scaling comparisons.
    oss << "\"fps_actual\":" << std::setprecision(6) << st.pipeline.fps_actual << ",";
    oss << "\"ms_per_frame\":{";
    oss << "\"capture\":" << std::setprecision(6) << st.pipeline.ms_per_frame.capture << ",";
    oss << "\"scale\":" << std::setprecision(6) << st.pipeline.ms_per_frame.scale << ",";
    oss << "\"effects\":" << std::setprecision(6) << st.pipeline.ms_per_frame.effects << ",";
    oss << "\"write\":" << std::setprecision(6) << st.pipeline.ms_per_frame.write;
    oss << "},";
    oss << "\"perf_sample_frames\":" << st.pipeline.perf_sample_frames << ",";

    // Deterministic effect ordering + rule-based disable reasons.
    const auto plan = studiocast::video::effects::BuildBroadcastEffectsPlan(cfg.pipeline.effects);
    oss << "\"effects_plan\":{";
    oss << "\"ordered\":[";
    for (std::size_t i = 0; i < plan.ordered_effect_ids.size(); ++i) {
        if (i) oss << ",";
        oss << "\"" << JsonEscape(plan.ordered_effect_ids[i]) << "\"";
    }
    oss << "],";

    oss << "\"vignette_attach_to\":\"" << JsonEscape(plan.vignette_attach_to_effect_id) << "\",";

    oss << "\"disabled\":[";
    for (std::size_t i = 0; i < plan.disabled.size(); ++i) {
        if (i) oss << ",";
        oss << "{";
        oss << "\"id\":\"" << JsonEscape(plan.disabled[i].id) << "\",";
        oss << "\"reason\":\"" << JsonEscape(plan.disabled[i].reason) << "\"";
        oss << "}";
    }
    oss << "]";
    oss << "}";
    oss << "},";

    oss << "\"last_error\":\"" << JsonEscape(st.last_error) << "\"";
    oss << "}";  // video

    // Audio status (MVP: microphone processing only).
    oss << ",\"audio\":{";
    oss << "\"enabled\":" << BoolJson(acfg.enabled) << ",";
    oss << "\"create_virtual_mic\":" << BoolJson(acfg.create_virtual_mic) << ",";
    oss << "\"source\":\"" << JsonEscape(acfg.source_name.empty() ? std::string("auto") : acfg.source_name) << "\",";
    oss << "\"mic_present\":" << BoolJson(ast.mic_present) << ",";

    // Canonical effect model for GUI/CLI.
    oss << "\"audio_effects\":"
        << studiocast::audio::effects::BroadcastAudioEffectsToJson(acfg.effects)
        << ",";

    // What effect is currently active (stable-ish summary string).
    std::string mic_mode = "none";
    if (acfg.effects.microphone.studio_voice_enabled) {
        mic_mode = "studio_voice";
    } else if (acfg.effects.microphone.noise_removal_enabled && acfg.effects.microphone.room_echo_removal_enabled) {
        mic_mode = "noise_echo_removal";
    } else if (acfg.effects.microphone.noise_removal_enabled) {
        mic_mode = "noise_removal";
    } else if (acfg.effects.microphone.room_echo_removal_enabled) {
        mic_mode = "room_echo_removal";
    }
    oss << "\"mic_mode\":\"" << JsonEscape(mic_mode) << "\",";

    oss << "\"pipeline\":{";
    oss << "\"running\":" << BoolJson(ast.pipeline_running) << ",";
    oss << "\"starting\":" << BoolJson(ast.pipeline_starting) << ",";
    oss << "\"sink\":\"" << JsonEscape(ast.pipeline_sink) << "\",";
    oss << "\"effect_selector\":\"" << JsonEscape(ast.effect_selector) << "\",";
    oss << "\"feature_id\":\"" << JsonEscape(ast.feature_id) << "\",";
    oss << "\"intensity\":" << ast.intensity << ",";
    oss << "\"gpu\":{";
    oss << "\"index\":" << ast.gpu_index << ",";
    oss << "\"name\":\"" << JsonEscape(ast.gpu_name) << "\",";
    oss << "\"compute_cap\":\"" << JsonEscape(ast.gpu_compute_cap) << "\"";
    oss << "},";
    oss << "\"last_error\":\"" << JsonEscape(ast.last_error) << "\"";
    oss << "}"; // pipeline

    oss << "}"; // audio

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
    // Legacy flat fields (kept for compatibility): derived from the canonical Broadcast schema.
    oss << "\"background\":\"" << JsonEscape(studiocast::video::effects::ToString(cfg.pipeline.effects.virtual_background.mode)) << "\",";
    oss << "\"background_backend\":\"" << JsonEscape(studiocast::video::effects::ToString(cfg.pipeline.effects.engine)) << "\",";
    oss << "\"background_strength\":" << cfg.pipeline.effects.virtual_background.strength << ",";
    oss << "\"background_remove_color\":\"" << JsonEscape(cfg.pipeline.effects.virtual_background.remove_color) << "\",";
    oss << "\"background_replace_image\":\"" << JsonEscape(cfg.pipeline.effects.virtual_background.replace_path) << "\",";

    const int vkl_intensity = std::max(0, std::min(100, cfg.pipeline.effects.virtual_key_light.intensity));
    oss << "\"virtual_key_light\":" << BoolJson(cfg.pipeline.effects.virtual_key_light.enabled) << ",";
    oss << "\"virtual_key_light_intensity\":" << vkl_intensity << ",";
    oss << "\"virtual_key_light_temperature\":\"" << JsonEscape(FormatKeyLightTemperaturePreset(cfg.pipeline.effects.virtual_key_light.temperature_preset)) << "\",";
    oss << "\"virtual_key_light_pan\":" << cfg.pipeline.effects.virtual_key_light.direction_pan_degrees << ",";
    oss << "\"virtual_key_light_hdri\":\"" << JsonEscape(cfg.pipeline.effects.virtual_key_light.hdri_path) << "\",";

    const int vignette_intensity = std::max(0, std::min(100, cfg.pipeline.effects.vignette.intensity));
    oss << "\"vignette\":" << BoolJson(cfg.pipeline.effects.vignette.enabled) << ",";
    oss << "\"vignette_intensity\":" << vignette_intensity << ",";
    oss << "\"vignette_center_on_face\":" << BoolJson(cfg.pipeline.effects.vignette.center_on_tracked_face) << ",";

    // Canonical, nested effects model (safe for file paths with spaces).
    oss << "\"video_effects\":"
        << studiocast::video::BroadcastCameraEffectsContractToJson(cfg.pipeline.effects);
    oss << "}";
    return oss.str();
}

std::string AudioConfigToJson(const studiocast::audio::VirtualAudioServiceConfig& cfg) {
    std::ostringstream oss;
    oss << "{";
    oss << "\"enabled\":" << BoolJson(cfg.enabled) << ",";
    oss << "\"create_virtual_mic\":" << BoolJson(cfg.create_virtual_mic) << ",";
    oss << "\"source\":\"" << JsonEscape(cfg.source_name.empty() ? std::string("auto") : cfg.source_name) << "\",";
    oss << "\"audio_effects\":" << studiocast::audio::effects::BroadcastAudioEffectsToJson(cfg.effects);
    oss << "}";
    return oss.str();
}

bool ApplyAudioConfigPatchJsonText(const std::string& jsonText,
                                  studiocast::audio::VirtualAudioServiceConfig* cfg,
                                  std::vector<std::string>* warnings,
                                  std::string* error) {
    if (!cfg) {
        if (error) *error = "config pointer is null";
        return false;
    }

    studiocast::util::json::Value root;
    if (!studiocast::util::json::Parse(jsonText, &root, error)) return false;
    const auto* obj = root.AsObject();
    if (!obj) {
        if (error) *error = "audio config must be a JSON object";
        return false;
    }

    // enabled
    if (auto it = obj->find("enabled"); it != obj->end()) {
        const bool* b = it->second.AsBool();
        if (!b) {
            if (error) *error = "enabled must be a boolean";
            return false;
        }
        cfg->enabled = *b;
    }

    // create_virtual_mic
    if (auto it = obj->find("create_virtual_mic"); it != obj->end()) {
        const bool* b = it->second.AsBool();
        if (!b) {
            if (error) *error = "create_virtual_mic must be a boolean";
            return false;
        }
        cfg->create_virtual_mic = *b;
    }

    // source
    if (auto it = obj->find("source"); it != obj->end()) {
        const std::string* s = it->second.AsString();
        if (!s) {
            if (error) *error = "source must be a string";
            return false;
        }
        cfg->source_name = (*s == "auto") ? std::string() : *s;
    }

    // effects blob
    const studiocast::util::json::Value* fxVal = nullptr;
    if (auto it = obj->find("audio_effects"); it != obj->end()) {
        fxVal = &it->second;
    } else if (auto it2 = obj->find("effects"); it2 != obj->end()) {
        // Accept alias for convenience.
        fxVal = &it2->second;
        if (warnings) warnings->push_back("effects: alias accepted; please use audio_effects");
    }

    if (fxVal) {
        studiocast::audio::effects::BroadcastAudioEffects parsed;
        studiocast::audio::effects::BroadcastAudioEffectsJsonParseOptions options;
        options.allow_unknown_keys = true;
        std::vector<std::string> parseWarnings;
        std::string parseError;
        if (!studiocast::audio::effects::ParseBroadcastAudioEffectsJson(*fxVal,
                                                                        &parsed,
                                                                        options,
                                                                        &parseWarnings,
                                                                        &parseError)) {
            if (error) *error = parseError;
            return false;
        }
        cfg->effects = parsed;
        if (warnings) warnings->insert(warnings->end(), parseWarnings.begin(), parseWarnings.end());
    }

    return true;
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

std::string AppendWarningsToObjectJson(const std::string& objJson, const std::vector<std::string>& warnings) {
    if (warnings.empty()) return objJson;

    // Find last '}' (skip trailing whitespace).
    std::size_t end = objJson.size();
    while (end > 0 && std::isspace(static_cast<unsigned char>(objJson[end - 1]))) --end;
    if (end == 0 || objJson[end - 1] != '}') return objJson;

    const std::size_t close = end - 1;
    const std::size_t open = objJson.find('{');
    if (open == std::string::npos || open >= close) return objJson;

    bool isEmpty = true;
    for (std::size_t i = open + 1; i < close; ++i) {
        if (!std::isspace(static_cast<unsigned char>(objJson[i]))) {
            isEmpty = false;
            break;
        }
    }

    std::ostringstream w;
    w << "\"warnings\":[";
    for (std::size_t i = 0; i < warnings.size(); ++i) {
        if (i) w << ',';
        w << "\"" << JsonEscape(warnings[i]) << "\"";
    }
    w << ']';

    std::string out;
    out.reserve(objJson.size() + warnings.size() * 32);
    out.append(objJson.substr(0, close));
    out.append(isEmpty ? "" : ",");
    out.append(w.str());
    out.push_back('}');
    out.append(objJson.substr(end));
    return out;
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
    studiocast::audio::VirtualAudioServiceConfig acfg = studiocast::config::ToAudioServiceConfig(daemonCfg);

    if (const auto v = GetArgValue(argc, argv, "--input"); !v.empty()) cfg.pipeline.input_device = v;
    if (const auto v = GetArgValue(argc, argv, "--output"); !v.empty()) cfg.pipeline.output_device = v;

    bool capture_mode_explicit = false;
    if (const auto v = GetArgValue(argc, argv, "--capture-mode"); !v.empty()) {
        const auto t = ToLowerAscii(v);
        if (t == "auto" || t == "auto_best" || t == "autobest") {
            cfg.pipeline.capture_mode = studiocast::video::CaptureMode::auto_best;
            capture_mode_explicit = true;
        } else if (t == "requested") {
            cfg.pipeline.capture_mode = studiocast::video::CaptureMode::requested;
            capture_mode_explicit = true;
        } else {
            std::cerr << "WARN: unknown --capture-mode value: " << v << " (expected requested|auto)\n";
        }
    }

    cfg.pipeline.width = GetArgInt(argc, argv, "--width", cfg.pipeline.width);
    cfg.pipeline.height = GetArgInt(argc, argv, "--height", cfg.pipeline.height);
    cfg.pipeline.fps = GetArgInt(argc, argv, "--fps", cfg.pipeline.fps);

    // Convenience: if the user sets a sentinel width/height and didn't explicitly set a capture mode,
    // treat it as capture auto.
    if (!capture_mode_explicit && (cfg.pipeline.width <= 0 || cfg.pipeline.height <= 0)) {
        cfg.pipeline.capture_mode = studiocast::video::CaptureMode::auto_best;
    }

    if (HasArg(argc, argv, "--mirror")) cfg.pipeline.effects.mirror = true;

    // Legacy CLI flags: map to canonical Broadcast schema.
    if (const auto v = GetArgValue(argc, argv, "--background"); !v.empty()) {
        studiocast::video::effects::VirtualBackgroundMode mode{};
        if (studiocast::video::effects::ParseVirtualBackgroundMode(v, &mode)) {
            cfg.pipeline.effects.virtual_background.mode = mode;
            if (mode != studiocast::video::effects::VirtualBackgroundMode::none) {
                cfg.pipeline.effects.auto_frame.enabled = false;
            }
        } else if (v == "auto_frame" || v == "autoframe") {
            cfg.pipeline.effects.auto_frame.enabled = true;
            cfg.pipeline.effects.virtual_background.mode = studiocast::video::effects::VirtualBackgroundMode::none;
        } else {
            std::cerr << "WARN: unknown --background value: " << v << "\n";
        }
    }
    if (const auto v = GetArgValue(argc, argv, "--background-backend"); !v.empty()) {
        studiocast::video::effects::EffectsEnginePreference eng{};
        if (studiocast::video::effects::ParseEffectsEnginePreference(v, &eng)) {
            cfg.pipeline.effects.engine = eng;
        } else {
            std::cerr << "WARN: unknown --background-backend value: " << v
                      << " (expected auto|maxine|open_cuda)\n";
        }
    }
    if (const int v = GetArgInt(argc, argv, "--background-strength", -1); v > 0) {
        cfg.pipeline.effects.virtual_background.strength = std::max(1, std::min(64, v));
    }

    if (const auto v = GetArgValue(argc, argv, "--background-remove-color"); !v.empty()) {
        // Canonical form is "#RRGGBB". Accept legacy formats too.
        std::uint32_t rgb = 0;
        if (ParseRgbHex(v, &rgb)) {
            cfg.pipeline.effects.virtual_background.remove_color = FormatRgbHex(rgb);
        } else {
            std::cerr << "WARN: invalid --background-remove-color (expected #RRGGBB): " << v << "\n";
        }
    }
    if (const auto v = GetArgValue(argc, argv, "--background-replace-image"); !v.empty()) {
        cfg.pipeline.effects.virtual_background.replace_path = v;
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

    studiocast::audio::VirtualAudioService audioSvc;
    std::string aerr;
    if (!audioSvc.Start(acfg, &aerr)) {
        std::cerr << "ERROR: " << aerr << "\n";
        svc.Stop();
        return 1;
    }

    // Start IPC server for GUI / studiocastctl.
    studiocast::ipc::DaemonServer server;
    std::string sockErr;
    const auto socketPath = studiocast::ipc::DaemonSocketPath(&sockErr);
    if (socketPath.empty()) {
        std::cerr << "ERROR: failed to compute socket path: " << sockErr << "\n";
        svc.Stop();
        audioSvc.Stop();
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

                              const auto ast = audioSvc.Status();
                              const auto acurrent = audioSvc.Config();

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

                              // Cache Open CUDA diagnostics to avoid heavy probing on every GUI poll.
                              static studiocast::util::TtlCache<std::string> openCudaDiagCache;
                              constexpr auto kOpenCudaDiagTtl = std::chrono::seconds(2);

                              const std::string openCudaJson = openCudaDiagCache.GetOrCompute(
                                      std::chrono::steady_clock::now(), kOpenCudaDiagTtl, []() {
                                          return studiocast::open_cuda::DiagnoseOpenCudaDefault().ToJson();
                                      });

                              return std::string("OK ") +
                                     StatusToJson(st, current, ast, acurrent, socketPath, diagJson, openCudaJson);
                          }

                          if (pc.cmd == "GET_CONFIG") {
                              const auto current = svc.Config();
                              return std::string("OK ") +
                                     studiocast::video::BroadcastCameraEffectsContractToJson(current.pipeline.effects);
                          }

                          if (pc.cmd == "GET_AUDIO_CONFIG") {
                              const auto current = audioSvc.Config();
                              return std::string("OK ") + AudioConfigToJson(current);
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

                          if (pc.cmd == "AUDIO_START") {
                              std::lock_guard<std::mutex> lock(controlMu);
                              auto newCfg = audioSvc.Config();
                              newCfg.enabled = true;
                              audioSvc.UpdateConfig(newCfg);

                              studiocast::config::ApplyAudioServiceConfigToDaemonConfig(newCfg, &daemonCfg);
                              std::string perr;
                              (void)studiocast::config::SaveDaemonConfig(daemonCfg, &perr);

                              return std::string("OK {\"enabled\":true}");
                          }

                          if (pc.cmd == "AUDIO_STOP") {
                              std::lock_guard<std::mutex> lock(controlMu);
                              auto newCfg = audioSvc.Config();
                              newCfg.enabled = false;
                              audioSvc.UpdateConfig(newCfg);

                              studiocast::config::ApplyAudioServiceConfigToDaemonConfig(newCfg, &daemonCfg);
                              std::string perr;
                              (void)studiocast::config::SaveDaemonConfig(daemonCfg, &perr);

                              return std::string("OK {\"enabled\":false}");
                          }

                          if (pc.cmd == "SET_AUDIO_CONFIG") {
                              const std::string jsonText = ExtractRawTailAfterCmd(line, pc.cmd);
                              if (jsonText.empty()) {
                                  return std::string("ERR ") + ErrorJson("SET_AUDIO_CONFIG requires a JSON object argument");
                              }

                              std::lock_guard<std::mutex> lock(controlMu);
                              auto newCfg = audioSvc.Config();

                              std::vector<std::string> warnings;
                              std::string jerr;
                              if (!ApplyAudioConfigPatchJsonText(jsonText, &newCfg, &warnings, &jerr)) {
                                  return std::string("ERR ") + ErrorJson(jerr.empty() ? "invalid audio config JSON" : jerr);
                              }

                              audioSvc.UpdateConfig(newCfg);

                              studiocast::config::ApplyAudioServiceConfigToDaemonConfig(newCfg, &daemonCfg);
                              std::string perr;
                              (void)studiocast::config::SaveDaemonConfig(daemonCfg, &perr);

                              return std::string("OK ") + AppendWarningsToObjectJson(AudioConfigToJson(newCfg), warnings);
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

                              std::vector<std::string> warnings;

                              std::string jerr;
                              auto bfx = newCfg.pipeline.effects;

                              if (!studiocast::video::ApplyBroadcastCameraEffectsPatchJsonText(jsonText, &bfx, &jerr)) {
                                  // Legacy compatibility: accept old `CameraEffects` JSON patches, but warn.
                                  // Note: explicit requests for backend `cpu` are rejected by the legacy parser.
                                  std::string lerr;
                                  auto legacy = studiocast::video::ToLegacyCameraEffects(bfx);
                                  if (!studiocast::video::ApplyCameraEffectsPatchJsonText(jsonText, &legacy, &lerr)) {
                                      std::string msg = jerr.empty() ? "invalid effects JSON" : jerr;
                                      if (!lerr.empty()) {
                                          msg += "; legacy parse: " + lerr;
                                      }
                                      return std::string("ERR ") + ErrorJson(msg);
                                  }
                                  warnings.emplace_back(
                                      "Legacy effects JSON accepted; please migrate to the Broadcast effects schema (video_effects)."
                                  );
                                  bfx = studiocast::video::ToBroadcastCameraEffects(legacy);
                              }
                              newCfg.pipeline.effects = bfx;

                              svc.UpdateConfig(newCfg);

                              studiocast::config::ApplyVideoServiceConfigToDaemonConfig(newCfg, &daemonCfg);
                              std::string perr;
                              (void)studiocast::config::SaveDaemonConfig(daemonCfg, &perr);

                              return std::string("OK ") + AppendWarningsToObjectJson(ConfigToJson(newCfg), warnings);
                          }

                          if (pc.cmd == "SET_VIDEO_EFFECTS") {
                              std::lock_guard<std::mutex> lock(controlMu);
                              auto newCfg = svc.Config();

                              std::vector<std::string> warnings;
                              warnings.emplace_back("SET_VIDEO_EFFECTS is deprecated; use SET_VIDEO_EFFECTS_JSON");

                              auto bfx = newCfg.pipeline.effects;

                              if (auto it = pc.kv.find("mirror"); it != pc.kv.end()) {
                                  bool mirror = false;
                                  if (!ParseBoolArg(it->second, &mirror)) {
                                      return std::string("ERR ") + ErrorJson("mirror must be 0|1");
                                  }
                                  bfx.mirror = mirror;
                              }

                              if (auto it = pc.kv.find("background"); it != pc.kv.end()) {
                                  const auto v = it->second;
                                  studiocast::video::effects::VirtualBackgroundMode mode{};
                                  if (studiocast::video::effects::ParseVirtualBackgroundMode(v, &mode)) {
                                      bfx.virtual_background.mode = mode;
                                      if (mode != studiocast::video::effects::VirtualBackgroundMode::none) {
                                          bfx.auto_frame.enabled = false;
                                      }
                                  } else if (v == "auto_frame" || v == "autoframe") {
                                      bfx.auto_frame.enabled = true;
                                      bfx.virtual_background.mode = studiocast::video::effects::VirtualBackgroundMode::none;
                                  } else {
                                      return std::string("ERR ") + ErrorJson("background must be none|blur|remove|replace|auto_frame");
                                  }
                              }

                              if (auto it = pc.kv.find("background_backend"); it != pc.kv.end()) {
                                  // Deprecated flat field: map to canonical engine preference.
                                  warnings.emplace_back("background_backend is deprecated; use engine");

                                  {
                                      std::string v = it->second;
                                      std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) {
                                          return static_cast<char>(std::tolower(c));
                                      });
                                      if (v == "cpu") {
                                          return std::string("ERR ") + ErrorJson("backend 'cpu' is not supported");
                                      }
                                  }

                                  studiocast::video::effects::EffectsEnginePreference eng{};
                                  if (!studiocast::video::effects::ParseEffectsEnginePreference(it->second, &eng)) {
                                      return std::string("ERR ") + ErrorJson("background_backend must be auto|maxine|open_cuda");
                                  }
                                  bfx.engine = eng;
                              }

                              if (auto it = pc.kv.find("background_strength"); it != pc.kv.end()) {
                                  const int v = std::atoi(it->second.c_str());
                                  if (v <= 0) {
                                      return std::string("ERR ") + ErrorJson("background_strength must be a positive integer");
                                  }
                                  bfx.virtual_background.strength = std::max(1, std::min(64, v));
                              }

                              if (auto it = pc.kv.find("background_remove_color"); it != pc.kv.end()) {
                                  std::uint32_t rgb = 0;
                                  if (!ParseRgbHex(it->second, &rgb)) {
                                      return std::string("ERR ") + ErrorJson("background_remove_color must be #RRGGBB");
                                  }
                                  bfx.virtual_background.remove_color = FormatRgbHex(rgb);
                              }

                              if (auto it = pc.kv.find("background_replace_image"); it != pc.kv.end()) {
                                  bfx.virtual_background.replace_path = it->second;
                              }

                              if (auto it = pc.kv.find("virtual_key_light"); it != pc.kv.end()) {
                                  bool en = false;
                                  if (!ParseBoolArg(it->second, &en)) {
                                      return std::string("ERR ") + ErrorJson("virtual_key_light must be 0|1");
                                  }
                                  bfx.virtual_key_light.enabled = en;
                              }

                              if (auto it = pc.kv.find("virtual_key_light_intensity"); it != pc.kv.end()) {
                                  const int v = std::atoi(it->second.c_str());
                                  if (v < 0 || v > 100) {
                                      return std::string("ERR ") + ErrorJson("virtual_key_light_intensity must be 0..100");
                                  }
                                  bfx.virtual_key_light.intensity = v;
                              }

                              if (auto it = pc.kv.find("virtual_key_light_temperature"); it != pc.kv.end()) {
                                  const int preset = ParseKeyLightTemperaturePreset(it->second, -1);
                                  if (preset < 0) {
                                      return std::string("ERR ") + ErrorJson("virtual_key_light_temperature must be neutral|warm|cool");
                                  }
                                  bfx.virtual_key_light.temperature_preset = preset;
                                  // Match KelvinFromPreset() in broadcast_effects_json.cpp.
                                  switch (preset) {
                                      case 1: bfx.virtual_key_light.temperature = 3200; break;
                                      case 2: bfx.virtual_key_light.temperature = 6500; break;
                                      default: bfx.virtual_key_light.temperature = 4500; break;
                                  }
                              }

                              if (auto it = pc.kv.find("virtual_key_light_pan"); it != pc.kv.end()) {
                                  const int v = std::atoi(it->second.c_str());
                                  bfx.virtual_key_light.direction_pan_degrees = std::max(-180, std::min(180, v));
                              }

                              if (auto it = pc.kv.find("virtual_key_light_hdri"); it != pc.kv.end()) {
                                  bfx.virtual_key_light.hdri_path = it->second;
                              }

                              if (auto it = pc.kv.find("vignette"); it != pc.kv.end()) {
                                  bool en = false;
                                  if (!ParseBoolArg(it->second, &en)) {
                                      return std::string("ERR ") + ErrorJson("vignette must be 0|1");
                                  }
                                  bfx.vignette.enabled = en;
                              }

                              if (auto it = pc.kv.find("vignette_intensity"); it != pc.kv.end()) {
                                  const int v = std::atoi(it->second.c_str());
                                  if (v < 0 || v > 100) {
                                      return std::string("ERR ") + ErrorJson("vignette_intensity must be 0..100");
                                  }
                                  bfx.vignette.intensity = v;
                              }

                              if (auto it = pc.kv.find("vignette_center_on_face"); it != pc.kv.end()) {
                                  bool en = false;
                                  if (!ParseBoolArg(it->second, &en)) {
                                      return std::string("ERR ") + ErrorJson("vignette_center_on_face must be 0|1");
                                  }
                                  bfx.vignette.center_on_tracked_face = en;
                              }

                              // Enforce mutually exclusive background modes.
                              if (bfx.auto_frame.enabled) {
                                  bfx.virtual_background.mode = studiocast::video::effects::VirtualBackgroundMode::none;
                              }

                              // Persist via canonical schema.
                              newCfg.pipeline.effects = bfx;

                              svc.UpdateConfig(newCfg);

                              studiocast::config::ApplyVideoServiceConfigToDaemonConfig(newCfg, &daemonCfg);
                              std::string perr;
                              (void)studiocast::config::SaveDaemonConfig(daemonCfg, &perr);

                              return std::string("OK ") + AppendWarningsToObjectJson(ConfigToJson(newCfg), warnings);
                          }

                          return std::string("ERR ") + ErrorJson("unknown_command");
                      },
                      &serverErr)) {
        std::cerr << "ERROR: failed to start IPC server: " << serverErr << "\n";
        svc.Stop();
        audioSvc.Stop();
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
    audioSvc.Stop();
    return 0;
}
