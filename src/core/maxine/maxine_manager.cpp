#include "core/maxine/maxine_manager.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "core/maxine/ar_api.h"
#include "core/maxine/availability.h"
#include "core/maxine/paths.h"
#include "core/maxine/reason_codes.h"
#include "core/maxine/vfx_api.h"
#include "core/probe/probe.h"
#include "core/util/dynlib.h"
#include "core/video/effects/broadcast_effect_contract.h"
#include "core/video/effects/effect_descriptors.h"

namespace studiocast::maxine {
namespace {

namespace fs = std::filesystem;

std::string ToLowerCopy(std::string s) {
  for (char &c : s) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return s;
}

std::string JsonEscape(const std::string &s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (char c : s) {
    switch (c) {
    case '\\':
      out += "\\\\";
      break;
    case '"':
      out += "\\\"";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (static_cast<unsigned char>(c) < 0x20) {
        out += "?";
      } else {
        out += c;
      }
    }
  }
  return out;
}

// The directory names that install each StudioCast feature id, newest first.
// SDK Core 1.x names each VFX or AR feature directory after its NGC model,
// for example `nvvfxdenoising`; AFX names each directory after the effect.
// The older names stay in the list so an SDK 0.7 or 0.8 tree keeps working.
std::vector<std::string> MarkerNamesFor(const std::string &feature_id) {
  // VFX.
  if (feature_id == "greenscreen")
    return {"nvvfxgreenscreen", "greenscreen"};
  if (feature_id == "bgblur")
    return {"nvvfxbackgroundblur", "backgroundblur", "bgblur"};
  if (feature_id == "denoise")
    return {"nvvfxdenoising", "denoising", "denoise"};
  if (feature_id == "relighting")
    return {"nvvfxrelighting", "nvvfxaigsrelighting", "aigsrelighting",
            "relighting"};

  // AR.
  if (feature_id == "gaze_redirection")
    return {"nvargazeredirection", "gazeredirection", "eyecontact",
            "gaze_redirection"};
  if (feature_id == "face_detection")
    return {"nvarfaceboxdetection", "faceboxdetection", "facebox",
            "face_detection"};
  if (feature_id == "body_detection")
    return {"nvarbodydetection", "bodydetection", "bodyboxdetection",
            "bodybox", "body_detection"};

  // AFX. The directory names are the ids that StudioCast uses, except for
  // the older name of the dereverb effect.
  if (feature_id == "dereverb")
    return {"dereverb", "room_echo_removal"};
  return {feature_id};
}

// True when `features_dir` holds an entry that one of `names` names. The
// comparison takes the whole name, without case: a name that only holds
// another one, such as `dereverb_denoiser` and `dereverb`, must not count,
// and neither must the SDK helpers (`install_feature.sh`, `README.md`,
// `compute_capability`), which no feature names.
bool HasAnyFeatureMarker(const fs::path &features_dir,
                         const std::vector<std::string> &names) {
  if (features_dir.empty()) {
    return false;
  }
  std::error_code ec;
  if (!fs::is_directory(features_dir, ec)) {
    return false;
  }

  std::set<std::string> wanted;
  for (const auto &name : names) {
    wanted.insert(ToLowerCopy(name));
  }

  for (const auto &entry : fs::directory_iterator(features_dir, ec)) {
    if (ec) {
      break;
    }
    if (wanted.count(ToLowerCopy(entry.path().filename().string())) != 0) {
      return true;
    }
  }

  return false;
}

bool MeetsMinDriverVersion(const studiocast::probe::Version &v) {
  // Developer note (docs/maxine_install.md): Maxine Linux requires
  // 570.26+.
  if (v.major > 570)
    return true;
  if (v.major < 570)
    return false;
  return v.minor >= 26;
}

void DedupPreserveOrder(std::vector<std::string> *v) {
  if (!v)
    return;
  std::set<std::string> seen;
  std::vector<std::string> out;
  out.reserve(v->size());
  for (const auto &s : *v) {
    if (s.empty())
      continue;
    if (seen.insert(s).second)
      out.push_back(s);
  }
  *v = std::move(out);
}

ComponentDiagnostics ConvertComponent(const ComponentPaths &c) {
  ComponentDiagnostics out;
  out.component = c.component;
  out.root_env_var = c.root_env_var;
  out.root = c.root;
  out.root_source = c.root_source;
  out.candidate_roots = c.candidate_roots;
  out.library_names = c.library_names;
  out.searched_lib_dirs = c.searched_lib_dirs;
  out.library = c.library;
  out.models_dir = c.models_dir;
  out.models_dir_source = c.models_dir_source;
  out.candidate_models_dirs = c.candidate_models_dirs;
  out.require_models_dir = c.require_models_dir;
  out.features_dir = c.features_dir;
  out.root_exists = c.root_exists;
  out.models_dir_exists = c.models_dir_exists;
  out.features_dir_exists = c.features_dir_exists;
  out.library_exists = c.library_exists;
  out.ok = c.ok;
  out.problems = c.problems;
  return out;
}

std::optional<studiocast::probe::GpuInfo>
FindSelectedGpu(const studiocast::probe::Report &rep) {
  if (!rep.selected_gpu_index.has_value()) {
    return std::nullopt;
  }
  for (const auto &g : rep.gpus) {
    if (g.index == *rep.selected_gpu_index) {
      return g;
    }
  }
  return std::nullopt;
}

std::string
DriverBlockedReason(const studiocast::maxine::MaxineDiagnostics &d) {
  using namespace studiocast::maxine::reasons;
  return d.driver.version.empty() ? std::string(kDriverMissing)
                                  : std::string(kDriverTooOld);
}

std::string GpuBlockedReason(const studiocast::maxine::MaxineDiagnostics &d) {
  using namespace studiocast::maxine::reasons;
  if (!d.gpus.empty()) {
    if (!d.gpu.selected_index.has_value()) {
      return std::string(kGpuNotSelected);
    }
    if (!d.gpu.ok && !d.gpu.selected_name.empty()) {
      return std::string(kGpuUnsupported);
    }
    return std::string(kGpuSelectionFailed);
  }
  return std::string(kGpuMissing);
}

std::string
ComponentBlockedReason(const studiocast::maxine::ComponentDiagnostics &c,
                       const char *component_code) {
  using namespace studiocast::maxine::reasons;
  const std::string comp =
      component_code ? std::string(component_code) : std::string();

  if (c.component == "VFX") {
    if (!c.root_exists || !c.library_exists) {
      return std::string(kMissingVfxSdk);
    }
  }
  if (c.component == "AR") {
    if (!c.root_exists || !c.library_exists) {
      return std::string(kMissingArSdk);
    }
  }
  if (c.component == "AFX") {
    if (!c.root_exists || !c.library_exists) {
      return std::string(kMissingAfxSdk);
    }
  }

  if (c.library_exists && !c.library_loadable) {
    const std::string err = ToLowerCopy(c.library_dlopen_error);

    bool looks_like_symbol = (err.find("symbol") != std::string::npos);
    if (!looks_like_symbol) {
      for (const auto &p : c.problems) {
        const std::string lp = ToLowerCopy(p);
        if (lp.find("symbol") != std::string::npos) {
          looks_like_symbol = true;
          break;
        }
      }
    }

    const std::string base = looks_like_symbol ? std::string(kSymbolMissing)
                                               : std::string(kDlopenFailed);
    return comp.empty() ? base : (base + ":" + comp);
  }

  return std::string(kUnknown);
}

int RankReasonCode(const std::string &code) {
  using namespace studiocast::maxine::reasons;
  auto starts_with = [&](std::string_view prefix) {
    return code.rfind(std::string(prefix), 0) == 0;
  };

  if (starts_with(kGpuMissing) || starts_with(kGpuNotSelected) ||
      starts_with(kGpuUnsupported) || starts_with(kGpuSelectionFailed)) {
    return 10;
  }
  if (starts_with(kDriverMissing) || starts_with(kDriverTooOld)) {
    return 20;
  }
  if (code == kMissingVfxSdk || code == kMissingArSdk) {
    return 30;
  }
  if (starts_with(kSymbolMissing)) {
    return 40;
  }
  if (starts_with(kDlopenFailed)) {
    return 50;
  }
  if (starts_with(kMissingVfxFeaturePrefix) ||
      starts_with(kMissingArFeaturePrefix)) {
    return 60;
  }
  if (code == kUnknown) {
    return 90;
  }
  return 80;
}

std::string PickTopReasonFromMissingEffects(
    const std::map<std::string, std::vector<std::string>> &missing_effects) {
  std::string best;
  int best_rank = 999;
  for (const auto &kv : missing_effects) {
    for (const auto &r : kv.second) {
      const int rank = RankReasonCode(r);
      if (best.empty() || rank < best_rank || (rank == best_rank && r < best)) {
        best = r;
        best_rank = rank;
      }
    }
  }
  return best;
}

} // namespace

bool FeatureMarkerInstalled(const fs::path &features_dir,
                            const std::string &feature_id) {
  return HasAnyFeatureMarker(features_dir, MarkerNamesFor(feature_id));
}

MaxineDiagnostics MaxineManager::Diagnose(bool verbose_probe) const {
  MaxineDiagnostics d;

  const auto probe_rep = studiocast::probe::Run(verbose_probe);

  // Detected GPUs (stable order: nvidia-smi index).
  d.gpus.reserve(probe_rep.gpus.size());
  for (const auto &g : probe_rep.gpus) {
    GpuSummary s;
    s.index = g.index;
    s.uuid = g.uuid;
    s.name = g.name;
    s.compute_cap = g.compute_cap.value_or("");
    s.likely_supported = g.likely_supported;
    s.maxine_gpu_arg = g.maxine_gpu_arg.value_or("");
    d.gpus.push_back(std::move(s));
  }
  std::sort(d.gpus.begin(), d.gpus.end(),
            [](const GpuSummary &a, const GpuSummary &b) {
              return a.index < b.index;
            });

  // Driver
  d.driver.min_version = "570.26";
  if (probe_rep.nvidia_driver.has_value()) {
    d.driver.version = probe_rep.nvidia_driver->original;
    d.nvidia_driver = d.driver.version;
    d.driver.meets_min_version =
        MeetsMinDriverVersion(*probe_rep.nvidia_driver);
    d.driver.ok = d.driver.meets_min_version;
    if (!d.driver.ok) {
      d.driver.details = "NVIDIA driver too old for Maxine (requires 570.26+).";
      d.problems.push_back(d.driver.details);
      d.hints.push_back("Update the NVIDIA driver to 570.26+ (then reboot).");
    }
  } else {
    d.driver.ok = false;
    d.driver.meets_min_version = false;
    d.driver.details = "NVIDIA driver not detected.";
    d.problems.push_back(d.driver.details);
    d.hints.push_back(
        "Install the NVIDIA driver and verify with 'nvidia-smi'.");
  }

  // GPU selection (already applies Settings + probe discovery)
  d.gpu.selection_mode = probe_rep.gpu_selection_mode;
  d.gpu.selected_index = probe_rep.selected_gpu_index;
  d.gpu.selected_uuid = probe_rep.selected_gpu_uuid;

  if (auto sel = FindSelectedGpu(probe_rep)) {
    d.gpu.selected_name = sel->name;
    d.gpu.compute_cap = sel->compute_cap;
    d.gpu.maxine_gpu_arg = sel->maxine_gpu_arg;
    d.gpu.ok = sel->likely_supported;
    if (!d.gpu.ok) {
      d.gpu.error = "Selected GPU appears unsupported for Maxine (requires "
                    "RTX-class/Turing+).";
    }
  } else {
    d.gpu.ok = false;
    if (probe_rep.gpus.empty()) {
      d.gpu.error = "No NVIDIA GPUs detected (nvidia-smi returned no GPUs).";
    } else if (!probe_rep.selected_gpu_index.has_value()) {
      d.gpu.error = "No GPU selected (check settings.conf gpu.* keys).";
    } else {
      d.gpu.error = "Selected GPU index not found in probe GPU list.";
    }
  }

  // SDK paths (filesystem-only)
  const auto paths = ResolveMaxinePaths();
  d.vfx = ConvertComponent(paths.vfx);
  d.ar = ConvertComponent(paths.ar);
  d.afx = ConvertComponent(paths.afx);

  // dlopen checks are best-effort: they validate that the resolved .so is a
  // real, loadable ELF.
  if (d.vfx.library_exists) {
    std::string err;
    studiocast::maxine::vfx::VfxApi api;
    d.vfx.library_loadable = api.InitializeFromLibraryPath(d.vfx.library, &err);
    d.vfx.library_dlopen_error = d.vfx.library_loadable ? "" : err;
    if (!d.vfx.library_loadable) {
      d.vfx.problems.push_back(
          "VFX library exists but required symbols could not be loaded: " +
          d.vfx.library.string() + (err.empty() ? "" : " (" + err + ")"));
    }
  }
  if (d.ar.library_exists) {
    std::string err;
    studiocast::maxine::ar::ArApi api;
    d.ar.library_loadable = api.InitializeFromLibraryPath(d.ar.library, &err);
    d.ar.library_dlopen_error = d.ar.library_loadable ? "" : err;
    if (!d.ar.library_loadable) {
      d.ar.problems.push_back(
          "AR library exists but required symbols could not be loaded: " +
          d.ar.library.string() + (err.empty() ? "" : " (" + err + ")"));
    }
  }
  if (d.afx.library_exists) {
    std::string err;
    studiocast::util::DynLib lib;
    d.afx.library_loadable =
        lib.Open(d.afx.library, studiocast::util::DynLib::Scope::Local, &err);
    d.afx.library_dlopen_error = d.afx.library_loadable ? "" : err;
    if (!d.afx.library_loadable) {
      d.afx.problems.push_back("AFX library exists but could not be loaded: " +
                               d.afx.library.string() +
                               (err.empty() ? "" : " (" + err + ")"));
    }
  }

  // Feature install markers (see MarkerNamesFor).
  auto add_feature = [&](ComponentDiagnostics *comp, const std::string &id,
                         const std::string &hint) {
    FeatureInstallStatus f;
    f.id = id;
    f.installed = FeatureMarkerInstalled(comp->features_dir, id);
    f.details = f.installed ? "installed" : hint;
    comp->features.push_back(std::move(f));
  };

  add_feature(&d.vfx, "greenscreen",
              "missing (run VFX install_feature.sh for greenscreen)");
  add_feature(&d.vfx, "bgblur",
              "missing (run VFX install_feature.sh for bgblur)");
  add_feature(&d.vfx, "denoise",
              "missing (run VFX install_feature.sh for denoise)");
  add_feature(&d.vfx, "relighting",
              "missing (run VFX install_feature.sh for relighting)");
  add_feature(&d.ar, "gaze_redirection",
              "missing (run AR install_feature.sh for gaze_redirection)");
  add_feature(&d.ar, "face_detection",
              "missing (run AR install_feature.sh for face_detection)");
  add_feature(&d.ar, "body_detection",
              "optional (install AR body detection to improve tracking)");

  add_feature(&d.afx, "denoiser",
              "missing (run AFX install_feature.sh for denoiser)");
  add_feature(&d.afx, "dereverb",
              "missing (run AFX install_feature.sh for dereverb)");
  add_feature(&d.afx, "dereverb_denoiser",
              "missing (run AFX install_feature.sh for dereverb_denoiser)");
  add_feature(&d.afx, "studio_voice",
              "missing (run AFX install_feature.sh for studio_voice)");

  // Derive effect availability using stable effect IDs.
  const bool gpu_ok = d.gpu.ok && d.driver.ok;
  const bool vfx_ready = gpu_ok && d.vfx.ok && d.vfx.library_loadable;
  const bool ar_ready = gpu_ok && d.ar.ok && d.ar.library_loadable;
  const bool afx_ready = gpu_ok && d.afx.ok && d.afx.library_loadable;

  auto vfx_has = [&](const char *id) {
    for (const auto &f : d.vfx.features) {
      if (f.id == id)
        return f.installed;
    }
    return false;
  };
  auto ar_has = [&](const char *id) {
    for (const auto &f : d.ar.features) {
      if (f.id == id)
        return f.installed;
    }
    return false;
  };

  auto afx_has = [&](const char *id) {
    for (const auto &f : d.afx.features) {
      if (f.id == id)
        return f.installed;
    }
    return false;
  };

  if (afx_ready) {
    if (afx_has("denoiser")) {
      d.available_audio_effects.push_back("denoiser");
    }
    if (afx_has("dereverb")) {
      d.available_audio_effects.push_back("dereverb");
    }
    if (afx_has("dereverb_denoiser")) {
      d.available_audio_effects.push_back("dereverb_denoiser");
    }
    if (afx_has("studio_voice")) {
      d.available_audio_effects.push_back("studio_voice");
    }
  }

  if (vfx_ready) {
    if (vfx_has("greenscreen")) {
      d.available_effects.push_back("virtual_background.remove");
      d.available_effects.push_back("virtual_background.replace");
    }
    // Background blur in Broadcast-style UX generally implies segmentation.
    if (vfx_has("greenscreen") && vfx_has("bgblur")) {
      d.available_effects.push_back("virtual_background.blur");
    }
    if (vfx_has("denoise")) {
      d.available_effects.push_back("video_noise_removal");
    }
    if (vfx_has("relighting")) {
      d.available_effects.push_back("virtual_key_light");
    }
  }

  if (ar_ready) {
    if (ar_has("face_detection") || ar_has("body_detection")) {
      d.available_effects.push_back("auto_frame");
    }
    if ((ar_has("face_detection") || ar_has("body_detection")) &&
        ar_has("gaze_redirection")) {
      d.available_effects.push_back("eye_contact");
    }
  }

  // Deterministic ordering for GUI parsing.
  std::sort(d.available_effects.begin(), d.available_effects.end());
  d.available_effects.erase(
      std::unique(d.available_effects.begin(), d.available_effects.end()),
      d.available_effects.end());

  // Aggregate problems + hints.
  if (!d.gpu.ok && !d.gpu.error.empty()) {
    d.problems.push_back(d.gpu.error);
  }
  d.problems.insert(d.problems.end(), d.vfx.problems.begin(),
                    d.vfx.problems.end());
  d.problems.insert(d.problems.end(), d.ar.problems.begin(),
                    d.ar.problems.end());

  // Compute missing effects (for Maxine-backed effects only) with actionable
  // reasons.
  {
    const auto descs = studiocast::video::effects::VideoEffectDescriptors();
    std::set<std::string> avail(d.available_effects.begin(),
                                d.available_effects.end());

    auto add_reasons_from_component = [&](const ComponentDiagnostics &c,
                                          const char *component_code,
                                          std::vector<std::string> *out) {
      if (!out)
        return;

      // For the UI/CLI contract, emit stable reason codes (not English
      // sentences). Human-actionable details are provided via top-level
      // `blocked_details`.
      if (c.component == "VFX") {
        if (!c.root_exists || !c.library_exists) {
          out->push_back(std::string(reasons::kMissingVfxSdk));
          return;
        }
      }
      if (c.component == "AR") {
        if (!c.root_exists || !c.library_exists) {
          out->push_back(std::string(reasons::kMissingArSdk));
          return;
        }
      }

      if (c.library_exists && !c.library_loadable) {
        out->push_back(ComponentBlockedReason(c, component_code));
      }
    };

    auto effect_feature_reasons = [&](const std::string &effect_id,
                                      std::vector<std::string> *out) {
      if (!out)
        return;

      // VFX features are only meaningful when the VFX component is runnable.
      if (vfx_ready) {
        if (effect_id == studiocast::video::effects::contract::
                             kEffectIdVirtualBackgroundRemove ||
            effect_id == studiocast::video::effects::contract::
                             kEffectIdVirtualBackgroundReplace) {
          if (!vfx_has("greenscreen"))
            out->push_back(reasons::MissingVfxFeature("greenscreen"));
        } else if (effect_id == studiocast::video::effects::contract::
                                    kEffectIdVirtualBackgroundBlur) {
          if (!vfx_has("greenscreen"))
            out->push_back(reasons::MissingVfxFeature("greenscreen"));
          if (!vfx_has("bgblur"))
            out->push_back(reasons::MissingVfxFeature("bgblur"));
        } else if (effect_id == studiocast::video::effects::contract::
                                    kEffectIdVideoNoiseRemoval) {
          if (!vfx_has("denoise"))
            out->push_back(reasons::MissingVfxFeature("denoise"));
        } else if (effect_id == studiocast::video::effects::contract::
                                    kEffectIdVirtualKeyLight) {
          if (!vfx_has("relighting"))
            out->push_back(reasons::MissingVfxFeature("relighting"));
        }
      }

      // AR features are only meaningful when the AR component is runnable.
      if (ar_ready) {
        if (effect_id ==
            studiocast::video::effects::contract::kEffectIdAutoFrame) {
          if (!(ar_has("face_detection") || ar_has("body_detection"))) {
            out->push_back(reasons::MissingArFeature("face_detection"));
          }
        } else if (effect_id ==
                   studiocast::video::effects::contract::kEffectIdEyeContact) {
          if (!(ar_has("face_detection") || ar_has("body_detection"))) {
            out->push_back(reasons::MissingArFeature("face_detection"));
          }
          if (!ar_has("gaze_redirection"))
            out->push_back(reasons::MissingArFeature("gaze_redirection"));
        }
      }
    };

    for (const auto &ed : descs) {
      bool requires_vfx = false;
      bool requires_ar = false;
      for (const auto c : ed.required_components) {
        if (c == studiocast::video::effects::RequiredComponent::maxine_vfx)
          requires_vfx = true;
        if (c == studiocast::video::effects::RequiredComponent::maxine_ar)
          requires_ar = true;
      }
      if (!requires_vfx && !requires_ar)
        continue;
      if (avail.count(ed.id))
        continue;

      std::vector<std::string> reasons;
      if (!d.driver.ok)
        reasons.push_back(DriverBlockedReason(d));
      if (!d.gpu.ok)
        reasons.push_back(GpuBlockedReason(d));

      if (requires_vfx) {
        if (!vfx_ready) {
          add_reasons_from_component(d.vfx, "vfx", &reasons);
        }
      }
      if (requires_ar) {
        if (!ar_ready) {
          add_reasons_from_component(d.ar, "ar", &reasons);
        }
      }
      effect_feature_reasons(ed.id, &reasons);
      if (reasons.empty())
        reasons.push_back(std::string(studiocast::maxine::reasons::kUnknown));
      DedupPreserveOrder(&reasons);
      d.missing_effects[ed.id] = std::move(reasons);
    }
  }

  if (!d.gpu.ok) {
    d.hints.push_back("Run 'nvidia-smi' to verify the NVIDIA driver is "
                      "installed and the GPU is visible.");
  }

  if (!d.vfx.root_exists) {
    d.hints.push_back(
        "Install/extract the Maxine VideoFX SDK and set " + d.vfx.root_env_var +
        " to the SDK root (or install under the default XDG path).");
  }
  if (!d.ar.root_exists) {
    d.hints.push_back(
        "Install/extract the Maxine AR SDK and set " + d.ar.root_env_var +
        " to the SDK root (or install under the default XDG path).");
  }
  if (!d.afx.root_exists) {
    d.hints.push_back(
        "Install/extract the Maxine Audio Effects SDK and set " +
        d.afx.root_env_var +
        " to the SDK root (or install under the default XDG path).");
  }

  if (d.gpu.maxine_gpu_arg.has_value()) {
    d.hints.push_back("When installing features, use the GPU flag suggested by "
                      "StudioCast probe: --gpu " +
                      *d.gpu.maxine_gpu_arg + ".");
  }

  d.ok = !d.available_effects.empty() || !d.available_audio_effects.empty();
  d.supported = d.ok;

  // Blocked reason/details for stable GUI behavior.
  if (d.supported) {
    d.blocked_reason = std::string(studiocast::maxine::reasons::kNone);
    d.blocked_details.clear();
    d.summary = "Maxine available (" +
                std::to_string(d.available_effects.size()) +
                " video effect(s), " +
                std::to_string(d.available_audio_effects.size()) +
                " audio effect(s) available).";
  } else {
    if (!d.gpu.ok) {
      d.blocked_reason = GpuBlockedReason(d);
    } else if (!d.driver.ok) {
      d.blocked_reason = DriverBlockedReason(d);
    } else if (!vfx_ready) {
      d.blocked_reason = ComponentBlockedReason(d.vfx, "vfx");
    } else if (!ar_ready) {
      d.blocked_reason = ComponentBlockedReason(d.ar, "ar");
    } else {
      d.blocked_reason = PickTopReasonFromMissingEffects(d.missing_effects);
      if (d.blocked_reason.empty()) {
        d.blocked_reason = std::string(studiocast::maxine::reasons::kUnknown);
      }
    }

    const auto msg = BuildCanonicalMaxineBlockedCopy(d, MaxineNeed::any);
    d.summary = msg.summary;
    d.blocked_details = msg.steps;
    if (d.summary.empty()) {
      d.summary = "Maxine unavailable (no effects reported available).";
    }
  }

  return d;
}

std::string MaxineDiagnostics::ToJson() const {
  std::ostringstream oss;

  auto json_bool = [](bool v) { return v ? "true" : "false"; };
  auto json_string = [&](const std::string &s) {
    oss << '"' << JsonEscape(s) << '"';
  };

  auto json_string_array = [&](const std::vector<std::string> &arr) {
    oss << "[";
    for (size_t i = 0; i < arr.size(); ++i) {
      if (i)
        oss << ",";
      json_string(arr[i]);
    }
    oss << "]";
  };

  auto json_feature_status_map =
      [&](const std::vector<FeatureInstallStatus> &feats) {
        // Deterministic: sort by ID.
        std::vector<FeatureInstallStatus> tmp = feats;
        std::sort(tmp.begin(), tmp.end(),
                  [](const FeatureInstallStatus &a,
                     const FeatureInstallStatus &b) { return a.id < b.id; });
        oss << "{";
        for (size_t i = 0; i < tmp.size(); ++i) {
          if (i)
            oss << ",";
          json_string(tmp[i].id);
          oss << ":{";
          oss << "\"installed\":" << json_bool(tmp[i].installed)
              << ",\"details\":";
          json_string(tmp[i].details);
          oss << "}";
        }
        oss << "}";
      };

  oss << "{";
  oss << "\"supported\":" << json_bool(supported) << ",";
  oss << "\"blocked_reason\":";
  json_string(blocked_reason);
  oss << ",";
  oss << "\"blocked_details\":";
  json_string_array(blocked_details);
  oss << ",";

  // Legacy/compat fields.
  oss << "\"ok\":" << json_bool(ok) << ",";
  oss << "\"summary\":";
  json_string(summary);
  oss << ",";
  oss << "\"last_error\":";
  json_string(last_error);
  oss << ",";

  oss << "\"driver\":{\"version\":";
  json_string(driver.version);
  oss << ",\"min_version\":";
  json_string(driver.min_version);
  oss << ",\"meets_min_version\":" << json_bool(driver.meets_min_version);
  oss << ",\"ok\":" << json_bool(driver.ok);
  oss << ",\"details\":";
  json_string(driver.details);
  oss << "},";

  oss << "\"nvidia_driver\":";
  json_string(nvidia_driver);
  oss << ",";

  oss << "\"gpus\":[";
  for (size_t i = 0; i < gpus.size(); ++i) {
    if (i)
      oss << ",";
    oss << "{";
    oss << "\"index\":" << gpus[i].index << ",";
    oss << "\"uuid\":";
    json_string(gpus[i].uuid);
    oss << ",\"name\":";
    json_string(gpus[i].name);
    oss << ",\"compute_cap\":";
    json_string(gpus[i].compute_cap);
    oss << ",\"likely_supported\":" << json_bool(gpus[i].likely_supported)
        << ",";
    oss << "\"maxine_gpu_arg\":";
    json_string(gpus[i].maxine_gpu_arg);
    oss << "}";
  }
  oss << "],";

  // Selected GPU summary (selection policy + the selected device).
  oss << "\"gpu\":{";
  oss << "\"selection_mode\":";
  json_string(gpu.selection_mode);
  oss << ",";
  oss << "\"selected_index\":"
      << (gpu.selected_index.has_value() ? std::to_string(*gpu.selected_index)
                                         : "null")
      << ",";
  oss << "\"selected_uuid\":";
  json_string(gpu.selected_uuid);
  oss << ",";
  oss << "\"selected_name\":";
  json_string(gpu.selected_name);
  oss << ",";
  oss << "\"compute_cap\":";
  json_string(gpu.compute_cap.value_or(""));
  oss << ",";
  oss << "\"maxine_gpu_arg\":";
  json_string(gpu.maxine_gpu_arg.value_or(""));
  oss << ",";
  oss << "\"ok\":" << json_bool(gpu.ok) << ",";
  oss << "\"error\":";
  json_string(gpu.error);
  oss << "},";

  auto component_summary_json = [&](const ComponentDiagnostics &c) {
    oss << "{";
    oss << "\"found\":" << json_bool(c.root_exists && c.library_exists) << ",";
    oss << "\"root\":";
    json_string(c.root.string());
    oss << ",\"core_lib\":";
    json_string(c.library.string());
    oss << ",\"features_dir\":";
    json_string(c.features_dir.string());
    oss << ",\"feature_status\":";
    json_feature_status_map(c.features);
    oss << "}";
  };

  oss << "\"components\":{";
  oss << "\"vfx\":";
  component_summary_json(vfx);
  oss << ",\"ar\":";
  component_summary_json(ar);
  oss << ",\"afx\":";
  component_summary_json(afx);
  oss << "},";

  auto component_json = [&](const ComponentDiagnostics &c) {
    oss << "{";
    oss << "\"component\":";
    json_string(c.component);
    oss << ",";
    oss << "\"root_env_var\":";
    json_string(c.root_env_var);
    oss << ",";
    oss << "\"root\":";
    json_string(c.root.string());
    oss << ",";
    oss << "\"root_found\":" << json_bool(c.root_exists) << ",";
    oss << "\"models_dir_exists\":" << json_bool(c.models_dir_exists) << ",";
    oss << "\"features_dir_exists\":" << json_bool(c.features_dir_exists)
        << ",";
    oss << "\"root_source\":";
    json_string(c.root_source);
    oss << ",";
    oss << "\"library\":";
    json_string(c.library.string());
    oss << ",";
    oss << "\"library_exists\":" << json_bool(c.library_exists) << ",";
    oss << "\"library_loadable\":" << json_bool(c.library_loadable) << ",";
    oss << "\"library_dlopen_error\":";
    json_string(c.library_dlopen_error);
    oss << ",";
    oss << "\"models_dir\":";
    json_string(c.models_dir.string());
    oss << ",";
    oss << "\"models_dir_source\":";
    json_string(c.models_dir_source);
    oss << ",";
    oss << "\"features_dir\":";
    json_string(c.features_dir.string());
    oss << ",";
    oss << "\"ok\":" << json_bool(c.ok) << ",";
    oss << "\"problems\":[";
    for (size_t i = 0; i < c.problems.size(); ++i) {
      if (i)
        oss << ",";
      json_string(c.problems[i]);
    }
    oss << "],";
    oss << "\"feature_status\":";
    json_feature_status_map(c.features);
    oss << ",";
    oss << "\"features\":[";
    for (size_t i = 0; i < c.features.size(); ++i) {
      if (i)
        oss << ",";
      oss << "{";
      oss << "\"id\":";
      json_string(c.features[i].id);
      oss << ",\"installed\":" << json_bool(c.features[i].installed)
          << ",\"details\":";
      json_string(c.features[i].details);
      oss << "}";
    }
    oss << "]";
    oss << "}";
  };

  oss << "\"vfx\":";
  component_json(vfx);
  oss << ",";
  oss << "\"ar\":";
  component_json(ar);
  oss << ",";

  oss << "\"afx\":";
  component_json(afx);
  oss << ",";

  oss << "\"available_effects\":[";
  for (size_t i = 0; i < available_effects.size(); ++i) {
    if (i)
      oss << ",";
    json_string(available_effects[i]);
  }
  oss << "],";

  oss << "\"available_audio_effects\":";
  json_string_array(available_audio_effects);
  oss << ",";

  oss << "\"missing_effects\":{";
  bool first = true;
  for (const auto &kv : missing_effects) {
    if (!first)
      oss << ",";
    first = false;
    json_string(kv.first);
    oss << ":";
    json_string_array(kv.second);
  }
  oss << "},";

  oss << "\"problems\":[";
  for (size_t i = 0; i < problems.size(); ++i) {
    if (i)
      oss << ",";
    json_string(problems[i]);
  }
  oss << "],";

  oss << "\"hints\":[";
  for (size_t i = 0; i < hints.size(); ++i) {
    if (i)
      oss << ",";
    json_string(hints[i]);
  }
  oss << "]";

  oss << "}";
  return oss.str();
}

} // namespace studiocast::maxine
