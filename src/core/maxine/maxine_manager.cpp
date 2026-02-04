#include "core/maxine/maxine_manager.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "core/maxine/paths.h"
#include "core/maxine/ar_api.h"
#include "core/maxine/vfx_api.h"
#include "core/probe/probe.h"
#include "core/util/dynlib.h"

namespace studiocast::maxine {
namespace {

namespace fs = std::filesystem;

std::string ToLowerCopy(std::string s) {
  for (char& c : s) {
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  }
  return s;
}

std::string JsonEscape(const std::string& s) {
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

bool TryDlopen(const fs::path& p, std::string* error_out) {
  util::DynLib lib;
  return lib.Open(p, util::DynLib::Scope::Local, error_out);
}

bool HasFeatureMarker(const fs::path& features_dir, const std::string& feature_id) {
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
  for (const auto& entry : fs::directory_iterator(features_dir, ec)) {
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

ComponentDiagnostics ConvertComponent(const ComponentPaths& c) {
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

std::optional<studiocast::probe::GpuInfo> FindSelectedGpu(const studiocast::probe::Report& rep) {
  if (!rep.selected_gpu_index.has_value()) {
    return std::nullopt;
  }
  for (const auto& g : rep.gpus) {
    if (g.index == *rep.selected_gpu_index) {
      return g;
    }
  }
  return std::nullopt;
}

}  // namespace

MaxineDiagnostics MaxineManager::Diagnose(bool verbose_probe) const {
  MaxineDiagnostics d;

  const auto probe_rep = studiocast::probe::Run(verbose_probe);

  // Driver
  if (probe_rep.nvidia_driver.has_value()) {
    d.nvidia_driver = probe_rep.nvidia_driver->original;
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
      d.gpu.error = "Selected GPU appears unsupported for Maxine (requires RTX-class/Turing+).";
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

  // dlopen checks are best-effort: they validate that the resolved .so is a real, loadable ELF.
  if (d.vfx.library_exists) {
    std::string err;
    studiocast::maxine::vfx::VfxApi api;
    d.vfx.library_loadable = api.InitializeFromLibraryPath(d.vfx.library, &err);
    d.vfx.library_dlopen_error = d.vfx.library_loadable ? "" : err;
    if (!d.vfx.library_loadable) {
      d.vfx.problems.push_back("VFX library exists but required symbols could not be loaded: " + d.vfx.library.string() +
                               (err.empty() ? "" : " (" + err + ")"));
    }
  }
  if (d.ar.library_exists) {
    std::string err;
    studiocast::maxine::ar::ArApi api;
    d.ar.library_loadable = api.InitializeFromLibraryPath(d.ar.library, &err);
    d.ar.library_dlopen_error = d.ar.library_loadable ? "" : err;
    if (!d.ar.library_loadable) {
      d.ar.problems.push_back("AR library exists but required symbols could not be loaded: " + d.ar.library.string() +
                              (err.empty() ? "" : " (" + err + ")"));
    }
  }

  // Feature install markers (best-effort heuristics).
  auto add_feature = [&](ComponentDiagnostics* comp, const std::string& id, const std::string& hint) {
    FeatureInstallStatus f;
    f.id = id;
    f.installed = HasFeatureMarker(comp->features_dir, id);
    f.details = f.installed ? "installed" : hint;
    comp->features.push_back(std::move(f));
  };

  add_feature(&d.vfx, "greenscreen", "missing (run VFX install_feature.sh for greenscreen)");
  add_feature(&d.vfx, "blur", "missing (run VFX install_feature.sh for blur)");
  add_feature(&d.vfx, "denoise", "missing (run VFX install_feature.sh for denoise)");
  add_feature(&d.vfx, "relighting", "missing (run VFX install_feature.sh for relighting)");
  add_feature(&d.ar, "eyecontact", "missing (run AR install_feature.sh for eyecontact)");

  // Derive effect availability using stable effect IDs.
  const bool gpu_ok = d.gpu.ok;
  const bool vfx_ready = gpu_ok && d.vfx.ok && d.vfx.library_loadable;
  const bool ar_ready = gpu_ok && d.ar.ok && d.ar.library_loadable;

  auto vfx_has = [&](const char* id) {
    for (const auto& f : d.vfx.features) {
      if (f.id == id) return f.installed;
    }
    return false;
  };
  auto ar_has = [&](const char* id) {
    for (const auto& f : d.ar.features) {
      if (f.id == id) return f.installed;
    }
    return false;
  };

  if (vfx_ready) {
    if (vfx_has("greenscreen")) {
      d.available_effects.push_back("virtual_background.remove");
      d.available_effects.push_back("virtual_background.replace");
    }
    // Background blur in Broadcast-style UX generally implies segmentation.
    if (vfx_has("greenscreen") && vfx_has("blur")) {
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
    d.available_effects.push_back("auto_frame");
    if (ar_has("eyecontact")) {
      d.available_effects.push_back("eye_contact");
    }
  }

  // Aggregate problems + hints.
  if (!d.gpu.ok && !d.gpu.error.empty()) {
    d.problems.push_back(d.gpu.error);
  }
  d.problems.insert(d.problems.end(), d.vfx.problems.begin(), d.vfx.problems.end());
  d.problems.insert(d.problems.end(), d.ar.problems.begin(), d.ar.problems.end());

  if (!d.gpu.ok) {
    d.hints.push_back("Run 'nvidia-smi' to verify the NVIDIA driver is installed and the GPU is visible.");
  }

  if (!d.vfx.root_exists) {
    d.hints.push_back("Install/extract the Maxine VideoFX SDK and set " + d.vfx.root_env_var +
                      " to the SDK root (or install under the default XDG path)." );
  }
  if (!d.ar.root_exists) {
    d.hints.push_back("Install/extract the Maxine AR SDK and set " + d.ar.root_env_var +
                      " to the SDK root (or install under the default XDG path)." );
  }

  if (d.gpu.maxine_gpu_arg.has_value()) {
    d.hints.push_back("When installing features, use the GPU flag suggested by StudioCast probe: --gpu " +
                      *d.gpu.maxine_gpu_arg + ".");
  }

  d.ok = !d.available_effects.empty();
  if (d.ok) {
    d.summary = "Maxine available (" + std::to_string(d.available_effects.size()) + " effect(s) available).";
  } else {
    if (!d.problems.empty()) {
      d.summary = "Maxine unavailable: " + d.problems.front();
    } else {
      d.summary = "Maxine unavailable (no effects reported available).";
    }
  }

  return d;
}

std::string MaxineDiagnostics::ToJson() const {
  std::ostringstream oss;

  auto json_bool = [](bool v) { return v ? "true" : "false"; };
  auto json_string = [&](const std::string& s) { oss << '"' << JsonEscape(s) << '"'; };

  oss << "{";
  oss << "\"ok\":" << json_bool(ok) << ",";
  oss << "\"summary\":";
  json_string(summary);
  oss << ",";

  oss << "\"nvidia_driver\":";
  json_string(nvidia_driver);
  oss << ",";

  oss << "\"gpu\":{";
  oss << "\"selection_mode\":";
  json_string(gpu.selection_mode);
  oss << ",";
  oss << "\"selected_index\":" << (gpu.selected_index.has_value() ? std::to_string(*gpu.selected_index) : "null")
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

  auto component_json = [&](const ComponentDiagnostics& c) {
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
      if (i) oss << ",";
      json_string(c.problems[i]);
    }
    oss << "],";
    oss << "\"features\":[";
    for (size_t i = 0; i < c.features.size(); ++i) {
      if (i) oss << ",";
      oss << "{";
      oss << "\"id\":";
      json_string(c.features[i].id);
      oss << ",\"installed\":" << json_bool(c.features[i].installed) << ",\"details\":";
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
    if (i) oss << ",";
    json_string(available_effects[i]);
  }
  oss << "],";

  oss << "\"problems\":[";
  for (size_t i = 0; i < problems.size(); ++i) {
    if (i) oss << ",";
    json_string(problems[i]);
  }
  oss << "],";

  oss << "\"hints\":[";
  for (size_t i = 0; i < hints.size(); ++i) {
    if (i) oss << ",";
    json_string(hints[i]);
  }
  oss << "]";

  oss << "}";
  return oss.str();
}

}  // namespace studiocast::maxine
