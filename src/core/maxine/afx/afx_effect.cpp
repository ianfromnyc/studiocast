#include "core/maxine/afx/afx_effect.h"

#include "core/maxine/afx/afx_loader_path.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

namespace studiocast::maxine::afx {

namespace {

float StrengthToIntensity(int strength) {
  if (strength < 0)
    strength = 0;
  if (strength > 100)
    strength = 100;
  return static_cast<float>(strength) / 100.0f;
}

std::optional<std::string>
SmFolderFromComputeCap(const std::optional<std::pair<int, int>> &cap,
                       std::string *error_out) {
  if (!cap) {
    if (error_out) {
      *error_out =
          "GPU compute capability is required to resolve AFX model paths.";
    }
    return std::nullopt;
  }

  const int major = cap->first;
  const int minor = cap->second;

  // Supported folders (per issue requirements):
  // sm_75, sm_80, sm_86, sm_89, sm_90, sm_100, sm_120.
  if (major == 7) {
    if (minor >= 5)
      return std::string("sm_75");
  }
  if (major == 8) {
    if (minor >= 9)
      return std::string("sm_89");
    if (minor >= 6)
      return std::string("sm_86");
    if (minor >= 0)
      return std::string("sm_80");
  }
  if (major == 9) {
    if (minor >= 0)
      return std::string("sm_90");
  }
  if (major == 10) {
    if (minor >= 0)
      return std::string("sm_100");
  }
  if (major == 12) {
    if (minor >= 0)
      return std::string("sm_120");
  }

  if (error_out) {
    std::ostringstream oss;
    oss << "Unsupported GPU compute capability " << major << "." << minor
        << " for AFX models. Supported SM folders: sm_75, sm_80, sm_86, sm_89, "
           "sm_90, sm_100, sm_120.";
    *error_out = oss.str();
  }
  return std::nullopt;
}

std::optional<fs::path>
FindFirstExistingFile(const std::vector<fs::path> &candidates) {
  std::error_code ec;
  for (const auto &p : candidates) {
    if (p.empty())
      continue;
    if (fs::exists(p, ec) && fs::is_regular_file(p, ec)) {
      return p;
    }
  }
  return std::nullopt;
}

// The version numbers of `<stem>.so.<digits>[.<digits>...]`, or nothing when
// `filename` does not have that shape. The parts order the versions: `.so.2`
// gives {2}, `.so.2.1.0` gives {2, 1, 0}, and {2} sorts below {2, 1, 0}.
std::optional<std::vector<unsigned long long>>
SoVersionParts(const std::string &filename, const std::string &stem) {
  const std::string prefix = stem + ".so.";
  if (filename.size() <= prefix.size() ||
      filename.compare(0, prefix.size(), prefix) != 0) {
    return std::nullopt;
  }

  std::vector<unsigned long long> parts;
  unsigned long long value = 0;
  bool have_digits = false;
  for (std::size_t i = prefix.size(); i < filename.size(); ++i) {
    const char c = filename[i];
    if (c >= '0' && c <= '9') {
      value = value * 10ull + static_cast<unsigned long long>(c - '0');
      have_digits = true;
      continue;
    }
    if (c == '.' && have_digits) {
      parts.push_back(value);
      value = 0;
      have_digits = false;
      continue;
    }
    return std::nullopt;
  }

  if (!have_digits)
    return std::nullopt;
  parts.push_back(value);
  return parts;
}

// The highest `<stem>.so.<version>` file in `lib_dir`, or an empty path when
// there is none.
fs::path HighestVersionedLibrary(const fs::path &lib_dir,
                                 const std::string &stem) {
  std::error_code ec;
  fs::directory_iterator it(lib_dir, ec);
  const fs::directory_iterator end;

  fs::path best;
  std::optional<std::vector<unsigned long long>> best_version;
  for (; !ec && it != end; it.increment(ec)) {
    const auto parts = SoVersionParts(it->path().filename().string(), stem);
    if (!parts)
      continue;

    std::error_code file_ec;
    if (!fs::is_regular_file(it->path(), file_ec) || file_ec)
      continue;

    if (!best_version || *best_version < *parts) {
      best_version = parts;
      best = it->path();
    }
  }

  return best;
}

// The libraries that can hold one effect, in the order to try them. A feature
// that holds more than one effect, such as studio voice, names its libraries
// after the effect selector; the other features name theirs after the feature.
// AFX 2.1.0 installs `.so`, `.so.2` and `.so.2.1.0`; `.so.1` is for the older
// SDK.
std::vector<fs::path> FeatureLibraryCandidates(const fs::path &lib_dir,
                                               const std::string &selector,
                                               const std::string &feature_id) {
  std::vector<std::string> stems;
  if (!selector.empty())
    stems.push_back("libnv_audiofx_" + selector);
  if (!feature_id.empty() && feature_id != selector)
    stems.push_back("libnv_audiofx_" + feature_id);

  std::vector<fs::path> out;
  out.reserve(stems.size() * 4);
  for (const auto &stem : stems) {
    out.push_back(lib_dir / (stem + ".so"));
    out.push_back(lib_dir / (stem + ".so.2"));
    out.push_back(lib_dir / (stem + ".so.1"));
  }

  // The names above are the links the SDK installs beside the real file. A
  // tree that lost the links still holds `<stem>.so.2.1.0`, so try the highest
  // version of each stem after the names we know.
  for (const auto &stem : stems) {
    const fs::path versioned = HighestVersionedLibrary(lib_dir, stem);
    if (!versioned.empty())
      out.push_back(versioned);
  }
  return out;
}

// True when `dir` is on LD_LIBRARY_PATH of this process.
bool LoaderPathHasDir(const fs::path &dir) {
  if (dir.empty())
    return false;
  const char *current = std::getenv("LD_LIBRARY_PATH");
  return !LdLibraryPathWithDirs(current ? std::string(current) : std::string(),
                                {dir})
              .has_value();
}

std::optional<fs::path> ResolveModelPath(const AfxEffectConfig &cfg,
                                         std::string *error_out) {
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
      *error_out =
          "AFX model resolution currently supports only 48000 Hz (requested: " +
          std::to_string(cfg.sample_rate) + ").";
    }
    return std::nullopt;
  }

  std::string sm_err;
  const auto sm = SmFolderFromComputeCap(cfg.compute_capability, &sm_err);
  if (!sm) {
    if (error_out)
      *error_out = sm_err;
    return std::nullopt;
  }

  // The model is named after the effect selector, which for most features is
  // the feature id itself. The denoiser has a second model.
  std::vector<std::string> model_names;
  if (cfg.feature_id == "denoiser" && cfg.use_denoiser_v2_model) {
    model_names.push_back("denoiser_v2_48k.trtpkg");
  }
  if (!cfg.effect_selector.empty()) {
    model_names.push_back(cfg.effect_selector + "_48k.trtpkg");
  }
  if (!cfg.feature_id.empty() && cfg.feature_id != cfg.effect_selector) {
    model_names.push_back(cfg.feature_id + "_48k.trtpkg");
  }

  const fs::path model_dir = cfg.features_dir / cfg.feature_id / "models" / *sm;
  std::vector<fs::path> candidates;
  candidates.reserve(model_names.size());
  for (const auto &name : model_names) {
    candidates.push_back(model_dir / name);
  }

  const auto found = FindFirstExistingFile(candidates);
  if (!found) {
    if (error_out) {
      std::ostringstream oss;
      oss << "AFX model file not found: "
          << (candidates.empty() ? model_dir.string()
                                 : candidates.front().string())
          << ". Expected models under `"
          << (cfg.features_dir / cfg.feature_id / "models").string() << "` "
          << "(SM folder " << *sm << ").";
      *error_out = oss.str();
    }
    return std::nullopt;
  }

  return *found;
}

} // namespace

PlannedAfxMicrophoneEffect
PlanBroadcastMicrophoneEffect(bool studio_voice_enabled,
                              bool noise_removal_enabled,
                              bool room_echo_removal_enabled, int strength) {
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

AfxEffect::AfxEffect(AfxApi *api) : api_(api) {}

AfxEffect::~AfxEffect() { Destroy(); }

bool AfxEffect::Configure(const AfxEffectConfig &cfg, std::string *error_out) {
  configured_ = false;
  loaded_ = false;
  cfg_ = cfg;
  resolved_model_path_.clear();
  resolved_feature_lib_dir_.clear();
  resolved_feature_lib_path_.clear();

  if (cfg_.effect_selector.empty()) {
    if (error_out)
      *error_out = "AFX effect selector is empty.";
    return false;
  }
  if (cfg_.feature_id.empty()) {
    if (error_out)
      *error_out = "AFX feature_id is empty.";
    return false;
  }
  if (cfg_.features_dir.empty()) {
    if (error_out)
      *error_out =
          "AFX features_dir is empty (expected `<AFX_ROOT>/features`).";
    return false;
  }
  // The AFX 2.1.0 effects run one channel, and Run builds a one-element array
  // of channel pointers for them. Anything else would make the SDK read past
  // that array, so refuse it here. A stereo caller splits the channels itself
  // and calls Run once per channel.
  if (cfg_.channels != 1) {
    if (error_out) {
      std::ostringstream oss;
      oss << "AFX effects run 1 channel; StudioCast asked for " << cfg_.channels
          << ".";
      *error_out = oss.str();
    }
    return false;
  }
  if (cfg_.frame_samples == 0) {
    if (error_out)
      *error_out = "AFX frame_samples must be >= 1.";
    return false;
  }

  std::string model_err;
  const auto mp = ResolveModelPath(cfg_, &model_err);
  if (!mp) {
    if (error_out)
      *error_out = model_err;
    return false;
  }
  resolved_model_path_ = *mp;

  resolved_feature_lib_dir_ = cfg_.features_dir / cfg_.feature_id / "lib";
  const auto lib_candidates = FeatureLibraryCandidates(
      resolved_feature_lib_dir_, cfg_.effect_selector, cfg_.feature_id);
  const auto lib = FindFirstExistingFile(lib_candidates);
  if (!lib) {
    if (error_out) {
      std::ostringstream tried;
      bool first = true;
      for (const auto &c : lib_candidates) {
        if (!first)
          tried << ", ";
        first = false;
        tried << c.filename().string();
      }
      std::ostringstream oss;
      oss << "AFX feature library not found for `" << cfg_.feature_id << "`. "
          << "Tried " << tried.str() << " under `"
          << resolved_feature_lib_dir_.string() << "`. "
          << "Ensure you downloaded the AFX feature `" << cfg_.feature_id
          << "`.";
      *error_out = oss.str();
    }
    return false;
  }
  resolved_feature_lib_path_ = *lib;

  configured_ = true;
  return true;
}

bool AfxEffect::SetU32Any(NvAFX_Handle handle, const char *what,
                          std::initializer_list<const char *> candidates,
                          std::uint32_t v, std::string *error_out,
                          bool *unsupported_out) {
  if (unsupported_out)
    *unsupported_out = false;
  if (!api_ || !api_->IsInitialized() || !api_->f().NvAFX_SetU32) {
    if (error_out)
      *error_out = "AFX API not initialized (missing NvAFX_SetU32).";
    return false;
  }
  std::ostringstream tried;
  bool first = true;
  bool any_tried = false;
  bool all_invalid_param = true;
  for (const auto *sel : candidates) {
    if (!sel || std::strlen(sel) == 0)
      continue;
    if (!first)
      tried << ", ";
    first = false;
    tried << sel;
    const NvAFX_Status st = api_->f().NvAFX_SetU32(handle, sel, v);
    if (st == NVAFX_SUCCESS) {
      return true;
    }
    any_tried = true;
    if (st != NVAFX_ERR_INVALID_PARAM)
      all_invalid_param = false;
  }
  if (unsupported_out)
    *unsupported_out = any_tried && all_invalid_param;
  if (error_out) {
    std::ostringstream oss;
    oss << "NvAFX_SetU32 failed for " << what << " (tried: " << tried.str()
        << ").";
    *error_out = oss.str();
  }
  return false;
}

bool AfxEffect::SetFloatAny(NvAFX_Handle handle, const char *what,
                            std::initializer_list<const char *> candidates,
                            float v, std::string *error_out,
                            bool *unsupported_out) {
  if (unsupported_out)
    *unsupported_out = false;
  if (!api_ || !api_->IsInitialized() || !api_->f().NvAFX_SetFloat) {
    if (error_out)
      *error_out = "AFX API not initialized (missing NvAFX_SetFloat).";
    return false;
  }
  std::ostringstream tried;
  bool first = true;
  bool any_tried = false;
  bool all_invalid_param = true;
  for (const auto *sel : candidates) {
    if (!sel || std::strlen(sel) == 0)
      continue;
    if (!first)
      tried << ", ";
    first = false;
    tried << sel;
    const NvAFX_Status st = api_->f().NvAFX_SetFloat(handle, sel, v);
    if (st == NVAFX_SUCCESS) {
      return true;
    }
    any_tried = true;
    if (st != NVAFX_ERR_INVALID_PARAM)
      all_invalid_param = false;
  }
  if (unsupported_out)
    *unsupported_out = any_tried && all_invalid_param;
  if (error_out) {
    std::ostringstream oss;
    oss << "NvAFX_SetFloat failed for " << what << " (tried: " << tried.str()
        << ").";
    *error_out = oss.str();
  }
  return false;
}

bool AfxEffect::SetStringAny(NvAFX_Handle handle, const char *what,
                             std::initializer_list<const char *> candidates,
                             const std::string &v, std::string *error_out) {
  if (!api_ || !api_->IsInitialized() || !api_->f().NvAFX_SetString) {
    if (error_out)
      *error_out = "AFX API not initialized (missing NvAFX_SetString).";
    return false;
  }
  std::ostringstream tried;
  bool first = true;
  for (const auto *sel : candidates) {
    if (!sel || std::strlen(sel) == 0)
      continue;
    if (!first)
      tried << ", ";
    first = false;
    tried << sel;
    const NvAFX_Status st = api_->f().NvAFX_SetString(handle, sel, v.c_str());
    if (st == NVAFX_SUCCESS) {
      return true;
    }
  }
  if (error_out) {
    std::ostringstream oss;
    oss << "NvAFX_SetString failed for " << what << " (tried: " << tried.str()
        << ").";
    *error_out = oss.str();
  }
  return false;
}

// The same parameter can come up again, because the GUI can update the
// intensity as often as it likes. Keep one note for each message, so that the
// warnings stay a short list and not a log.
void AfxEffect::NoteOptionalParam(const char *what, bool unsupported,
                                  const std::string &set_err) {
  std::string note;
  if (unsupported) {
    note = "Effect `" + cfg_.effect_selector + "` takes no " + what + ".";
  } else {
    note = "Effect `" + cfg_.effect_selector + "` did not take the " + what +
           ": " + set_err;
  }
  if (std::find(warnings_.begin(), warnings_.end(), note) != warnings_.end())
    return;
  warnings_.push_back(std::move(note));
}

bool AfxEffect::GetU32Any(NvAFX_Handle handle,
                          std::initializer_list<const char *> candidates,
                          std::uint32_t *out) const {
  if (!out || !api_ || !api_->IsInitialized() || !api_->f().NvAFX_GetU32) {
    return false;
  }
  for (const auto *sel : candidates) {
    if (!sel || std::strlen(sel) == 0)
      continue;
    std::uint32_t v = 0;
    if (api_->f().NvAFX_GetU32(handle, sel, &v) == NVAFX_SUCCESS) {
      *out = v;
      return true;
    }
  }
  return false;
}

bool AfxEffect::Load(std::string *error_out) {
  if (!configured_) {
    if (error_out)
      *error_out = "AFX effect not configured.";
    return false;
  }
  if (!api_ || !api_->IsInitialized()) {
    if (error_out)
      *error_out = "AFX API is not initialized.";
    return false;
  }
  if (!api_->f().NvAFX_CreateEffect || !api_->f().NvAFX_Load ||
      !api_->f().NvAFX_DestroyEffect) {
    if (error_out)
      *error_out =
          "AFX API missing required symbols (CreateEffect/Load/DestroyEffect).";
    return false;
  }

  Destroy();
  warnings_.clear();

  // The AFX core loads the feature library by its bare name, and glibc reads
  // LD_LIBRARY_PATH only when the process starts. A process that did not get
  // the directory at start cannot add it, so say that plainly when the effect
  // does not come up.
  const bool lib_dir_on_loader_path =
      LoaderPathHasDir(resolved_feature_lib_dir_);

  NvAFX_Handle h = nullptr;
  const NvAFX_Status stCreate =
      api_->f().NvAFX_CreateEffect(cfg_.effect_selector.c_str(), &h);
  if (stCreate != NVAFX_SUCCESS || !h) {
    if (error_out) {
      std::ostringstream oss;
      oss << "NvAFX_CreateEffect failed for effect `" << cfg_.effect_selector
          << "` (status=" << stCreate << "). The feature library is "
          << resolved_feature_lib_path_.string() << ".";
      if (!lib_dir_on_loader_path) {
        oss << " Its directory " << resolved_feature_lib_dir_.string()
            << " is not on LD_LIBRARY_PATH, which is how the AFX core finds "
               "it. StudioCast programs put it there when they start, so run "
               "this program again, or export LD_LIBRARY_PATH with that "
               "directory before you start it.";
      }
      *error_out = oss.str();
    }
    return false;
  }
  handle_ = h;

  // Parameter setup. The names come from `nvAudioEffects.h` of AFX 2.1.0,
  // with the v1.0 name kept as a second choice for an older SDK. The model
  // goes first, then the stream format.
  //
  // Every effect needs the model, the sample rate and the frame size. The
  // other parameters belong to some effects alone: the AFX 2.1.0 microphone
  // effects all run with one channel and reject a channel count, and studio
  // voice also rejects the intensity, the effect version and the VAD flag. A
  // rejected parameter of that kind gives a warning, not a failure.
  std::string set_err;
  if (!SetStringAny(handle_, "model_path", {"model_path", "modelPath"},
                    resolved_model_path_.string(), &set_err)) {
    if (error_out)
      *error_out = set_err;
    Destroy();
    return false;
  }
  if (!SetU32Any(handle_, "sample_rate", {"input_sample_rate", "sample_rate"},
                 static_cast<std::uint32_t>(cfg_.sample_rate), &set_err)) {
    if (error_out)
      *error_out = set_err;
    Destroy();
    return false;
  }
  if (!SetU32Any(handle_, "frame_samples",
                 {"num_samples_per_input_frame", "num_samples_per_frame"},
                 cfg_.frame_samples, &set_err)) {
    if (error_out)
      *error_out = set_err;
    Destroy();
    return false;
  }

  // The channel count is part of the effect. An effect that does not take one
  // says so with an invalid parameter status; then read the count it reports
  // and stop only when that count is not the one the caller needs. Any other
  // status is a real SDK error, so stop at once and keep its message.
  bool channels_unsupported = false;
  if (!SetU32Any(handle_, "channels", {"num_input_channels", "num_channels"},
                 cfg_.channels, &set_err, &channels_unsupported)) {
    if (!channels_unsupported) {
      if (error_out)
        *error_out = set_err;
      Destroy();
      return false;
    }
    std::uint32_t actual = 0;
    const bool known =
        GetU32Any(handle_, {"num_input_channels", "num_channels"}, &actual);
    if (known && actual != cfg_.channels) {
      if (error_out) {
        std::ostringstream oss;
        oss << "AFX effect `" << cfg_.effect_selector << "` runs with "
            << actual << " channel(s); StudioCast asked for " << cfg_.channels
            << ".";
        *error_out = oss.str();
      }
      Destroy();
      return false;
    }
    std::ostringstream oss;
    oss << "Effect `" << cfg_.effect_selector
        << "` takes no channel count; it runs with ";
    if (known) {
      oss << actual << " channel(s).";
    } else {
      oss << "its own channel count.";
    }
    warnings_.push_back(oss.str());
  }

  bool unsupported = false;
  if (!SetFloatAny(handle_, "intensity", {"intensity_ratio"}, cfg_.intensity,
                   &set_err, &unsupported)) {
    NoteOptionalParam("intensity", unsupported, set_err);
  }
  if (cfg_.effect_version) {
    if (!SetU32Any(handle_, "effect_version", {"effect_version"},
                   *cfg_.effect_version, &set_err, &unsupported)) {
      NoteOptionalParam("effect version", unsupported, set_err);
    }
  }
  if (cfg_.vad_enabled) {
    if (!SetU32Any(handle_, "vad_enabled", {"enable_vad"}, 1u, &set_err,
                   &unsupported)) {
      NoteOptionalParam("VAD flag", unsupported, set_err);
    }
  }

  const NvAFX_Status stLoad = api_->f().NvAFX_Load(handle_);
  if (stLoad != NVAFX_SUCCESS) {
    if (error_out) {
      std::ostringstream oss;
      oss << "NvAFX_Load failed (status=" << stLoad << ") for effect `"
          << cfg_.effect_selector << "`.";
      *error_out = oss.str();
    }
    Destroy();
    return false;
  }

  loaded_ = true;
  return true;
}

bool AfxEffect::UpdateIntensity(float intensity, std::string *error_out) {
  if (!loaded_ || !handle_) {
    if (error_out)
      *error_out = "AFX effect not loaded.";
    return false;
  }
  if (!api_ || !api_->IsInitialized()) {
    if (error_out)
      *error_out = "AFX API is not initialized.";
    return false;
  }

  // Studio voice takes no intensity. Keep that a note and a no-op, and report
  // every other status as the failure it is.
  std::string set_err;
  bool unsupported = false;
  if (!SetFloatAny(handle_, "intensity", {"intensity_ratio"}, intensity,
                   &set_err, &unsupported)) {
    if (!unsupported) {
      if (error_out)
        *error_out = set_err;
      return false;
    }
    NoteOptionalParam("intensity", true, set_err);
    return true;
  }

  cfg_.intensity = intensity;
  return true;
}

bool AfxEffect::Run(const float *input, float *output,
                    std::uint32_t num_samples, std::string *error_out) {
  if (!loaded_ || !handle_) {
    if (error_out)
      *error_out = "AFX effect not loaded.";
    return false;
  }
  if (!api_ || !api_->IsInitialized() || !api_->f().NvAFX_Run) {
    if (error_out)
      *error_out = "AFX API not initialized (missing NvAFX_Run).";
    return false;
  }
  if (!input || !output || num_samples == 0) {
    if (error_out)
      *error_out = "AFX Run received invalid buffers.";
    return false;
  }

  // Configure refuses anything but one channel. Check again, because the
  // arrays below hold one pointer each and a larger count would make the SDK
  // read past them.
  if (cfg_.channels != 1) {
    if (error_out) {
      std::ostringstream oss;
      oss << "AFX effects run 1 channel; StudioCast asked for " << cfg_.channels
          << ".";
      *error_out = oss.str();
    }
    return false;
  }

  // The SDK takes an array of channel pointers. StudioCast runs the AFX
  // effects with one channel; a stereo caller splits the channels itself.
  const float *in_channels[1] = {input};
  float *out_channels[1] = {output};
  const NvAFX_Status st =
      api_->f().NvAFX_Run(handle_, in_channels, out_channels, num_samples, 1u);
  if (st != NVAFX_SUCCESS) {
    if (error_out) {
      std::ostringstream oss;
      oss << "NvAFX_Run failed (status=" << st << ") for effect `"
          << cfg_.effect_selector << "`.";
      *error_out = oss.str();
    }
    return false;
  }
  return true;
}

void AfxEffect::Destroy() {
  loaded_ = false;
  if (!handle_)
    return;
  if (api_ && api_->IsInitialized() && api_->f().NvAFX_DestroyEffect) {
    (void)api_->f().NvAFX_DestroyEffect(handle_);
  }
  handle_ = nullptr;
}

} // namespace studiocast::maxine::afx
