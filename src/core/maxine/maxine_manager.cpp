#include "core/maxine/maxine_manager.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "core/maxine/availability.h"
#include "core/maxine/ar_api.h"
#include "core/maxine/paths.h"
#include "core/maxine/vfx_api.h"
#include "core/probe/probe.h"
#include "core/util/dynlib.h"
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

bool HasFeatureMarker(const fs::path &features_dir,
                      const std::string &feature_id) {
  if (features_dir.empty()) {
    return false;
  }
  std::error_code ec;
  if (!fs::exists(features_dir, ec) || !fs::is_directory(features_dir, ec)) {
    return false;
  }

  // Fast path: exact directory/file match.
  if (fs::exists(features_dir / feature_id, ec)) {
    return true;
  }

  // Heuristic: match against entry names (case-insensitive substring).
  const std::string needle = ToLowerCopy(feature_id);
  for (const auto &entry : fs::directory_iterator(features_dir, ec)) {
    if (ec) {
      break;
    }
    const std::string name = ToLowerCopy(entry.path().filename().string());
    if (name.find(needle) != std::string::npos) {
      return true;
    }
  }

  return false;
}

bool HasAnyFeatureMarker(const fs::path &features_dir,
                         const std::vector<std::string> &ids) {
  for (const auto &id : ids) {
    if (HasFeatureMarker(features_dir, id))
      return true;
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

} // namespace

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

  // Feature install markers (best-effort heuristics).
  auto add_feature = [&](ComponentDiagnostics *comp, const std::string &id,
                         const std::string &hint,
                         const std::vector<std::string> &marker_aliases = {}) {
    FeatureInstallStatus f;
    f.id = id;
    std::vector<std::string> markers;
    markers.push_back(id);
    markers.insert(markers.end(), marker_aliases.begin(), marker_aliases.end());
    f.installed = HasAnyFeatureMarker(comp->features_dir, markers);
    f.details = f.installed ? "installed" : hint;
    comp->features.push_back(std::move(f));
  };

  add_feature(&d.vfx, "greenscreen",
              "missing (run VFX install_feature.sh for greenscreen)");
  add_feature(&d.vfx, "bgblur",
              "missing (run VFX install_feature.sh for bgblur)", {"blur"});
  add_feature(&d.vfx, "denoise",
              "missing (run VFX install_feature.sh for denoise)");
  add_feature(&d.vfx, "relighting",
              "missing (run VFX install_feature.sh for relighting)");
  add_feature(&d.ar, "gaze_redirection",
              "missing (run AR install_feature.sh for gaze_redirection)",
              {"eyecontact", "GazeRedirection"});
  add_feature(&d.ar, "face_detection",
              "missing (run AR install_feature.sh for face_detection)",
              {"FaceBoxDetection", "facebox"});
  add_feature(&d.ar, "body_detection",
              "optional (install AR body detection to improve tracking)",
              {"BodyBoxDetection", "bodybox"});

  // Derive effect availability using stable effect IDs.
  const bool gpu_ok = d.gpu.ok && d.driver.ok;
  const bool vfx_ready = gpu_ok && d.vfx.ok && d.vfx.library_loadable;
  const bool ar_ready = gpu_ok && d.ar.ok && d.ar.library_loadable;

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
                                          const std::string &label,
                                          std::vector<std::string> *out) {
      if (!out)
        return;
      if (!c.root_exists)
        out->push_back(label + " SDK root not found.");
      if (!c.library_exists)
        out->push_back(label + " library not found.");
      if (c.library_exists && !c.library_loadable) {
        out->push_back(label + " library exists but could not be loaded." +
                       (c.library_dlopen_error.empty()
                            ? ""
                            : " (" + c.library_dlopen_error + ")"));
      }
      for (const auto &p : c.problems) {
        out->push_back(p);
      }
    };

    auto effect_feature_reasons = [&](const std::string &effect_id,
                                      std::vector<std::string> *out) {
      if (!out)
        return;
      // VFX
      if (effect_id == "virtual_background.remove" ||
          effect_id == "virtual_background.replace") {
        if (!vfx_has("greenscreen"))
          out->push_back("VFX feature 'greenscreen' not installed.");
      } else if (effect_id == "virtual_background.blur") {
        if (!vfx_has("greenscreen"))
          out->push_back("VFX feature 'greenscreen' not installed.");
        if (!vfx_has("bgblur"))
          out->push_back("VFX feature 'bgblur' not installed.");
      } else if (effect_id == "video_noise_removal") {
        if (!vfx_has("denoise"))
          out->push_back("VFX feature 'denoise' not installed.");
      } else if (effect_id == "virtual_key_light") {
        if (!vfx_has("relighting"))
          out->push_back("VFX feature 'relighting' not installed.");
      }

      // AR
      if (effect_id == "auto_frame") {
        if (!(ar_has("face_detection") || ar_has("body_detection"))) {
          out->push_back("AR feature 'face_detection' not installed.");
        }
      } else if (effect_id == "eye_contact") {
        if (!(ar_has("face_detection") || ar_has("body_detection"))) {
          out->push_back("AR feature 'face_detection' not installed.");
        }
        if (!ar_has("gaze_redirection"))
          out->push_back("AR feature 'gaze_redirection' not installed.");
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
      if (!d.driver.ok) {
        reasons.push_back(d.driver.details.empty()
                              ? "NVIDIA driver not ready for Maxine."
                              : d.driver.details);
      }
      if (!d.gpu.ok) {
        reasons.push_back(d.gpu.error.empty()
                              ? "NVIDIA GPU not selected/supported."
                              : d.gpu.error);
      }

      if (requires_vfx) {
        if (!vfx_ready) {
          add_reasons_from_component(d.vfx, "VFX", &reasons);
        }
      }
      if (requires_ar) {
        if (!ar_ready) {
          add_reasons_from_component(d.ar, "AR", &reasons);
        }
      }
      effect_feature_reasons(ed.id, &reasons);
      if (reasons.empty())
        reasons.push_back("Effect is not available (unknown reason).");
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

  if (d.gpu.maxine_gpu_arg.has_value()) {
    d.hints.push_back("When installing features, use the GPU flag suggested by "
                      "StudioCast probe: --gpu " +
                      *d.gpu.maxine_gpu_arg + ".");
  }

  d.ok = !d.available_effects.empty();
  d.supported = d.ok;

  // Blocked reason/details for stable GUI behavior.
  if (d.supported) {
    d.blocked_reason = "none";
    d.blocked_details.clear();
    d.summary = "Maxine available (" + std::to_string(d.available_effects.size()) +
                " effect(s) available).";
  } else {
    if (!d.gpu.ok) {
      d.blocked_reason = "gpu";
    } else if (!d.driver.ok) {
      d.blocked_reason = "driver";
    } else if (!vfx_ready || !ar_ready) {
      d.blocked_reason = "sdk";
    } else {
      d.blocked_reason = "features";
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

  oss << "\"available_effects\":[";
  for (size_t i = 0; i < available_effects.size(); ++i) {
    if (i)
      oss << ",";
    json_string(available_effects[i]);
  }
  oss << "],";

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
