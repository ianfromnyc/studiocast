#include "core/open_cuda/model_pack_registry.h"

#include <algorithm>
#include <map>

#include "core/open_video/model_pack_registry.h"
#include "core/util/xdg.h"

namespace fs = std::filesystem;

namespace studiocast::open_cuda {
namespace {

const studiocast::open_video::ModelFile* FindMainOnnxFile(const studiocast::open_video::ModelPack& p) {
  // Prefer (kind=onnx, role=main|empty), then fall back to the first ONNX.
  for (const auto& f : p.files) {
    if (f.kind == "onnx" && (f.role.empty() || f.role == "main")) return &f;
  }
  for (const auto& f : p.files) {
    if (f.kind == "onnx") return &f;
  }
  return nullptr;
}

}  // namespace

ModelPackRegistry ModelPackRegistry::Scan(const fs::path& open_cuda_models_dir) {
  ModelPackRegistry reg;
  reg.root_ = open_cuda_models_dir;

  // Canonical scanning/validation lives in open_video::ModelPackRegistry.
  // This registry is kept as a thin adapter for the existing Open CUDA matting runtime.
  const auto ov_reg = studiocast::open_video::ModelPackRegistry::Scan(open_cuda_models_dir);
  reg.problems_ = ov_reg.Problems();

  for (const auto& p : ov_reg.ListModels()) {
    if (p.task != "matting") continue;

    const auto* onnx = FindMainOnnxFile(p);
    if (!onnx) {
      const std::string key = p.id.empty() ? p.root_dir.filename().string() : p.id;
      reg.problems_[key] = "missing ONNX file (kind=onnx)";
      continue;
    }

    if (!p.matting.has_value()) {
      const std::string key = p.id.empty() ? p.root_dir.filename().string() : p.id;
      reg.problems_[key] = "matting model pack is missing required metadata (input/output/preprocess)";
      continue;
    }

    ModelPack out;
    out.id = p.id;
    out.display_name = p.display_name.empty() ? p.id : p.display_name;
    out.task = p.task;
    out.onnx_filename = onnx->name;

    out.input.name = p.matting->input.name;
    out.input.layout = p.matting->input.layout;
    out.input.dtype = p.matting->input.dtype;
    out.input.width = p.matting->input.width;
    out.input.height = p.matting->input.height;
    out.input.channels = p.matting->input.channels;

    out.output.name = p.matting->output.name;
    out.output.kind = p.matting->output.kind;
    out.output.dtype = p.matting->output.dtype;

    out.preprocess.mean = p.matting->preprocess.mean;
    out.preprocess.std = p.matting->preprocess.std;
    out.preprocess.color = p.matting->preprocess.color;
    out.preprocess.range = p.matting->preprocess.range;

    out.root_dir = p.root_dir;
    out.manifest_path = p.manifest_path;
    out.onnx_path = onnx->path;
    out.license_path = p.license_path;

    reg.models_.push_back(std::move(out));
  }

  // Deterministic ordering for ResolveModel()/DefaultModelId.
  std::sort(reg.models_.begin(), reg.models_.end(), [](const ModelPack& a, const ModelPack& b) {
    return a.id < b.id;
  });

  // Defensive: dedupe by id.
  std::map<std::string, fs::path> seen;
  std::vector<ModelPack> unique;
  unique.reserve(reg.models_.size());
  for (auto& m : reg.models_) {
    if (auto it = seen.find(m.id); it != seen.end()) {
      reg.problems_[m.id] = std::string("duplicate model id '") + m.id + "' in " + m.root_dir.string() +
                           " (already provided by " + it->second.string() + ")";
      continue;
    }
    seen[m.id] = m.root_dir;
    unique.push_back(std::move(m));
  }
  reg.models_.swap(unique);

  return reg;
}

ModelPackRegistry ModelPackRegistry::ScanDefault() {
  const auto modelsRoot = util::StudioCastModelsDir();
  if (modelsRoot.empty()) return {};
  // Open CUDA currently consumes only the matting/segmentation packs.
  return Scan(modelsRoot / "open_video" / "segmentation");
}

std::optional<ModelPack> ModelPackRegistry::ResolveModel(const std::string& id) const {
  auto it = std::lower_bound(models_.begin(), models_.end(), id, [](const ModelPack& a, const std::string& b) {
    return a.id < b;
  });
  if (it == models_.end() || it->id != id) return std::nullopt;
  return *it;
}

std::string ModelPackRegistry::DefaultModelId() const {
  // Middle-of-the-road quality default for foreground matting.
  if (ResolveModel("birefnet_lite")) return "birefnet_lite";
  if (models_.empty()) return {};
  return models_.front().id;
}

}  // namespace studiocast::open_cuda
