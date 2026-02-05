#include "core/maxine/afx/afx_effect.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>

namespace studiocast::maxine::afx {

namespace {

float StrengthToIntensity(int strength) {
    if (strength < 0) strength = 0;
    if (strength > 100) strength = 100;
    return static_cast<float>(strength) / 100.0f;
}

std::optional<std::string> SmFolderFromComputeCap(const std::optional<std::pair<int, int>>& cap,
                                                  std::string* error_out) {
    if (!cap) {
        if (error_out) {
            *error_out = "GPU compute capability is required to resolve AFX model paths.";
        }
        return std::nullopt;
    }

    const int major = cap->first;
    const int minor = cap->second;

    // Supported folders (per issue requirements):
    // sm_75, sm_80, sm_86, sm_89, sm_90, sm_100, sm_120.
    if (major == 7) {
        if (minor >= 5) return std::string("sm_75");
    }
    if (major == 8) {
        if (minor >= 9) return std::string("sm_89");
        if (minor >= 6) return std::string("sm_86");
        if (minor >= 0) return std::string("sm_80");
    }
    if (major == 9) {
        if (minor >= 0) return std::string("sm_90");
    }
    if (major == 10) {
        if (minor >= 0) return std::string("sm_100");
    }
    if (major == 12) {
        if (minor >= 0) return std::string("sm_120");
    }

    if (error_out) {
        std::ostringstream oss;
        oss << "Unsupported GPU compute capability " << major << "." << minor
            << " for AFX models. Supported SM folders: sm_75, sm_80, sm_86, sm_89, sm_90, sm_100, sm_120.";
        *error_out = oss.str();
    }
    return std::nullopt;
}

std::optional<fs::path> FindFirstExistingFile(std::initializer_list<fs::path> candidates) {
    std::error_code ec;
    for (const auto& p : candidates) {
        if (p.empty()) continue;
        if (fs::exists(p, ec) && fs::is_regular_file(p, ec)) {
            return p;
        }
    }
    return std::nullopt;
}

bool PrependEnvPath(const char* var, const fs::path& dir, std::string* error_out) {
    if (!var || std::strlen(var) == 0) {
        if (error_out) *error_out = "Internal error: env var name is empty.";
        return false;
    }
    const std::string add = dir.string();
    if (add.empty()) {
        if (error_out) *error_out = "Internal error: attempted to prepend an empty directory to env.";
        return false;
    }

    const char* old = std::getenv(var);
    const std::string oldStr = old ? std::string(old) : std::string();

    auto contains_exact = [&](const std::string& list, const std::string& item) -> bool {
        if (list.empty()) return false;
        size_t start = 0;
        while (start <= list.size()) {
            const size_t end = list.find(':', start);
            const std::string token = (end == std::string::npos)
                                          ? list.substr(start)
                                          : list.substr(start, end - start);
            if (token == item) return true;
            if (end == std::string::npos) break;
            start = end + 1;
        }
        return false;
    };

    if (contains_exact(oldStr, add)) {
        return true;  // idempotent
    }

    std::string next;
    if (oldStr.empty()) {
        next = add;
    } else {
        next = add + ":" + oldStr;
    }

    if (::setenv(var, next.c_str(), 1) != 0) {
        if (error_out) {
            std::ostringstream oss;
            oss << "Failed to set " << var << ": " << std::strerror(errno);
            *error_out = oss.str();
        }
        return false;
    }
    return true;
}

std::optional<fs::path> ResolveModelPath(const AfxEffectConfig& cfg, std::string* error_out) {
    std::error_code ec;

    if (!cfg.model_path.empty()) {
        if (!fs::exists(cfg.model_path, ec)) {
            if (error_out) {
                *error_out = "AFX model file not found: " + cfg.model_path.string();
            }
            return std::nullopt;
        }
        return cfg.model_path;
    }

    if (cfg.sample_rate != 48000) {
        if (error_out) {
            *error_out = "AFX model resolution currently supports only 48000 Hz (requested: " +
                         std::to_string(cfg.sample_rate) + ").";
        }
        return std::nullopt;
    }

    std::string sm_err;
    const auto sm = SmFolderFromComputeCap(cfg.compute_capability, &sm_err);
    if (!sm) {
        if (error_out) *error_out = sm_err;
        return std::nullopt;
    }

    std::string model_name;
    if (cfg.feature_id == "denoiser") {
        model_name = cfg.use_denoiser_v2_model ? "denoiser_v2_48k.trtpkg" : "denoiser_48k.trtpkg";
    } else if (cfg.effect_selector == "studio_voice_low_latency") {
        model_name = "studio_voice_low_latency_48k.trtpkg";
    } else {
        model_name = cfg.feature_id + "_48k.trtpkg";
    }

    const fs::path expected = cfg.features_dir / cfg.feature_id / "models" / *sm / model_name;
    if (!fs::exists(expected, ec)) {
        if (error_out) {
            std::ostringstream oss;
            oss << "AFX model file not found: " << expected.string() << ". "
                << "Expected models under `" << (cfg.features_dir / cfg.feature_id / "models").string() << "` "
                << "(SM folder " << *sm << ").";
            *error_out = oss.str();
        }
        return std::nullopt;
    }

    return expected;
}

}  // namespace

PlannedAfxMicrophoneEffect PlanBroadcastMicrophoneEffect(bool studio_voice_enabled,
                                                        bool noise_removal_enabled,
                                                        bool room_echo_removal_enabled,
                                                        int strength) {
    PlannedAfxMicrophoneEffect out;
    out.intensity = StrengthToIntensity(strength);

    if (studio_voice_enabled) {
        out.enabled = true;
        out.effect_selector = "studio_voice_low_latency";
        out.feature_id = "studio_voice";
        return out;
    }

    if (noise_removal_enabled && room_echo_removal_enabled) {
        out.enabled = true;
        out.effect_selector = "dereverb_denoiser";
        out.feature_id = "dereverb_denoiser";
        return out;
    }
    if (noise_removal_enabled) {
        out.enabled = true;
        out.effect_selector = "denoiser";
        out.feature_id = "denoiser";
        return out;
    }
    if (room_echo_removal_enabled) {
        out.enabled = true;
        out.effect_selector = "dereverb";
        out.feature_id = "dereverb";
        return out;
    }

    return out;
}

AfxEffect::AfxEffect(AfxApi* api) : api_(api) {}

AfxEffect::~AfxEffect() { Destroy(); }

bool AfxEffect::Configure(const AfxEffectConfig& cfg, std::string* error_out) {
    configured_ = false;
    loaded_ = false;
    cfg_ = cfg;
    resolved_model_path_.clear();
    resolved_feature_lib_dir_.clear();
    resolved_feature_lib_path_.clear();

    if (cfg_.effect_selector.empty()) {
        if (error_out) *error_out = "AFX effect selector is empty.";
        return false;
    }
    if (cfg_.feature_id.empty()) {
        if (error_out) *error_out = "AFX feature_id is empty.";
        return false;
    }
    if (cfg_.features_dir.empty()) {
        if (error_out) *error_out = "AFX features_dir is empty (expected `<AFX_ROOT>/features`).";
        return false;
    }
    if (cfg_.channels == 0) {
        if (error_out) *error_out = "AFX channels must be >= 1.";
        return false;
    }
    if (cfg_.frame_samples == 0) {
        if (error_out) *error_out = "AFX frame_samples must be >= 1.";
        return false;
    }

    std::string model_err;
    const auto mp = ResolveModelPath(cfg_, &model_err);
    if (!mp) {
        if (error_out) *error_out = model_err;
        return false;
    }
    resolved_model_path_ = *mp;

    resolved_feature_lib_dir_ = cfg_.features_dir / cfg_.feature_id / "lib";
    const auto lib = FindFirstExistingFile({
        resolved_feature_lib_dir_ / ("libnv_audiofx_" + cfg_.feature_id + ".so"),
        resolved_feature_lib_dir_ / ("libnv_audiofx_" + cfg_.feature_id + ".so.1"),
    });
    if (!lib) {
        if (error_out) {
            std::ostringstream oss;
            oss << "AFX feature library not found for `" << cfg_.feature_id << "`. "
                << "Expected `libnv_audiofx_" << cfg_.feature_id << ".so` under `"
                << resolved_feature_lib_dir_.string() << "`. "
                << "Ensure you ran the AFX `install_feature.sh` for this feature.";
            *error_out = oss.str();
        }
        return false;
    }
    resolved_feature_lib_path_ = *lib;

    configured_ = true;
    return true;
}

bool AfxEffect::SetU32Any(NvAFX_Handle handle,
                         const char* what,
                         std::initializer_list<const char*> candidates,
                         std::uint32_t v,
                         std::string* error_out) {
    if (!api_ || !api_->IsInitialized() || !api_->f().NvAFX_SetU32) {
        if (error_out) *error_out = "AFX API not initialized (missing NvAFX_SetU32).";
        return false;
    }
    std::ostringstream tried;
    bool first = true;
    for (const auto* sel : candidates) {
        if (!sel || std::strlen(sel) == 0) continue;
        if (!first) tried << ", ";
        first = false;
        tried << sel;
        const NvAFX_Status st = api_->f().NvAFX_SetU32(handle, sel, v);
        if (st == NVAFX_SUCCESS) {
            return true;
        }
    }
    if (error_out) {
        std::ostringstream oss;
        oss << "NvAFX_SetU32 failed for " << what << " (tried: " << tried.str() << ").";
        *error_out = oss.str();
    }
    return false;
}

bool AfxEffect::SetFloatAny(NvAFX_Handle handle,
                           const char* what,
                           std::initializer_list<const char*> candidates,
                           float v,
                           std::string* error_out) {
    if (!api_ || !api_->IsInitialized() || !api_->f().NvAFX_SetFloat) {
        if (error_out) *error_out = "AFX API not initialized (missing NvAFX_SetFloat).";
        return false;
    }
    std::ostringstream tried;
    bool first = true;
    for (const auto* sel : candidates) {
        if (!sel || std::strlen(sel) == 0) continue;
        if (!first) tried << ", ";
        first = false;
        tried << sel;
        const NvAFX_Status st = api_->f().NvAFX_SetFloat(handle, sel, v);
        if (st == NVAFX_SUCCESS) {
            return true;
        }
    }
    if (error_out) {
        std::ostringstream oss;
        oss << "NvAFX_SetFloat failed for " << what << " (tried: " << tried.str() << ").";
        *error_out = oss.str();
    }
    return false;
}

bool AfxEffect::SetStringAny(NvAFX_Handle handle,
                            const char* what,
                            std::initializer_list<const char*> candidates,
                            const std::string& v,
                            std::string* error_out) {
    if (!api_ || !api_->IsInitialized() || !api_->f().NvAFX_SetString) {
        if (error_out) *error_out = "AFX API not initialized (missing NvAFX_SetString).";
        return false;
    }
    std::ostringstream tried;
    bool first = true;
    for (const auto* sel : candidates) {
        if (!sel || std::strlen(sel) == 0) continue;
        if (!first) tried << ", ";
        first = false;
        tried << sel;
        const NvAFX_Status st = api_->f().NvAFX_SetString(handle, sel, v.c_str());
        if (st == NVAFX_SUCCESS) {
            return true;
        }
    }
    if (error_out) {
        std::ostringstream oss;
        oss << "NvAFX_SetString failed for " << what << " (tried: " << tried.str() << ").";
        *error_out = oss.str();
    }
    return false;
}

bool AfxEffect::Load(std::string* error_out) {
    if (!configured_) {
        if (error_out) *error_out = "AFX effect not configured.";
        return false;
    }
    if (!api_ || !api_->IsInitialized()) {
        if (error_out) *error_out = "AFX API is not initialized.";
        return false;
    }
    if (!api_->f().NvAFX_CreateEffect || !api_->f().NvAFX_Load || !api_->f().NvAFX_DestroyEffect) {
        if (error_out) *error_out = "AFX API missing required symbols (CreateEffect/Load/DestroyEffect).";
        return false;
    }

    Destroy();

    // Ensure the feature library can be discovered by the AFX SDK loader.
    std::string env_err;
    if (!PrependEnvPath("LD_LIBRARY_PATH", resolved_feature_lib_dir_, &env_err)) {
        if (error_out) *error_out = env_err;
        return false;
    }

    NvAFX_Handle h = nullptr;
    const NvAFX_Status stCreate = api_->f().NvAFX_CreateEffect(cfg_.effect_selector.c_str(), &h);
    if (stCreate != NVAFX_SUCCESS || !h) {
        if (error_out) {
            std::ostringstream oss;
            oss << "NvAFX_CreateEffect failed for effect `" << cfg_.effect_selector << "` (status=" << stCreate
                << "). Ensure the AFX feature library is installed and discoverable (" << resolved_feature_lib_path_.string() << ").";
            *error_out = oss.str();
        }
        return false;
    }
    handle_ = h;

    // Parameter setup. We intentionally try a small set of plausible selector names
    // to stay resilient across SDK revisions without proprietary headers.
    std::string set_err;
    if (!SetStringAny(handle_, "model_path",
                      {"modelPath", "model_path", "model", "trtModelPath", "trt_model_path"},
                      resolved_model_path_.string(), &set_err)) {
        if (error_out) *error_out = set_err;
        Destroy();
        return false;
    }
    if (!SetU32Any(handle_, "sample_rate",
                   {"sampleRate", "sample_rate", "samplerate"},
                   static_cast<std::uint32_t>(cfg_.sample_rate), &set_err)) {
        if (error_out) *error_out = set_err;
        Destroy();
        return false;
    }
    if (!SetU32Any(handle_, "channels",
                   {"numChannels", "num_channels", "channels"},
                   cfg_.channels, &set_err)) {
        if (error_out) *error_out = set_err;
        Destroy();
        return false;
    }
    if (!SetU32Any(handle_, "frame_samples",
                   {"numSamplesPerFrame", "num_samples_per_frame", "frameSize", "frame_size", "frameSamples", "frame_samples"},
                   cfg_.frame_samples, &set_err)) {
        if (error_out) *error_out = set_err;
        Destroy();
        return false;
    }
    if (!SetFloatAny(handle_, "intensity",
                     {"intensityRatio", "intensity_ratio", "intensity", "strength"},
                     cfg_.intensity, &set_err)) {
        if (error_out) *error_out = set_err;
        Destroy();
        return false;
    }
    if (cfg_.effect_version) {
        if (!SetU32Any(handle_, "effect_version",
                       {"effectVersion", "effect_version", "version"},
                       *cfg_.effect_version, &set_err)) {
            if (error_out) *error_out = set_err;
            Destroy();
            return false;
        }
    }
    if (cfg_.vad_enabled) {
        if (!SetU32Any(handle_, "vad_enabled",
                       {"useVAD", "use_vad", "vad", "enableVAD"},
                       1u, &set_err)) {
            if (error_out) *error_out = set_err;
            Destroy();
            return false;
        }
    }

    const NvAFX_Status stLoad = api_->f().NvAFX_Load(handle_);
    if (stLoad != NVAFX_SUCCESS) {
        if (error_out) {
            std::ostringstream oss;
            oss << "NvAFX_Load failed (status=" << stLoad << ") for effect `" << cfg_.effect_selector << "`.";
            *error_out = oss.str();
        }
        Destroy();
        return false;
    }

    loaded_ = true;
    return true;
}

bool AfxEffect::Run(const float* input, float* output, std::uint32_t num_samples, std::string* error_out) {
    if (!loaded_ || !handle_) {
        if (error_out) *error_out = "AFX effect not loaded.";
        return false;
    }
    if (!api_ || !api_->IsInitialized() || !api_->f().NvAFX_Run) {
        if (error_out) *error_out = "AFX API not initialized (missing NvAFX_Run).";
        return false;
    }
    if (!input || !output || num_samples == 0) {
        if (error_out) *error_out = "AFX Run received invalid buffers.";
        return false;
    }

    const NvAFX_Status st = api_->f().NvAFX_Run(handle_, input, output, num_samples);
    if (st != NVAFX_SUCCESS) {
        if (error_out) {
            std::ostringstream oss;
            oss << "NvAFX_Run failed (status=" << st << ") for effect `" << cfg_.effect_selector << "`.";
            *error_out = oss.str();
        }
        return false;
    }
    return true;
}

void AfxEffect::Destroy() {
    loaded_ = false;
    if (!handle_) return;
    if (api_ && api_->IsInitialized() && api_->f().NvAFX_DestroyEffect) {
        (void)api_->f().NvAFX_DestroyEffect(handle_);
    }
    handle_ = nullptr;
}

}  // namespace studiocast::maxine::afx
