#include "core/open_cuda/model_pack_registry.h"

#include <algorithm>
#include <cerrno>
#include <sstream>

#include "core/util/fs.h"
#include "core/util/json.h"
#include "core/util/xdg.h"

namespace fs = std::filesystem;

namespace studiocast::open_cuda {

namespace {

constexpr int kMaxPackScanDepth = 6;

std::string PathForError(const fs::path& p) {
  // Avoid platform-specific quoting. We only need human-readable paths.
  return p.string();
}

bool IsSafeRelativePath(const fs::path& p) {
  if (p.empty()) return false;
  if (p.is_absolute()) return false;
  for (const auto& part : p) {
    if (part == "." || part == "..") return false;
  }
  return true;
}

bool Fail(std::string* error, std::string msg) {
  if (error) *error = std::move(msg);
  return false;
}

const util::json::Value::Object* AsObject(const util::json::Value& v, std::string* error, const char* what) {
  const auto* o = v.AsObject();
  if (o) return o;
  if (error) *error = std::string("model.json: expected object for ") + what;
  return nullptr;
}

const util::json::Value* Get(const util::json::Value::Object& o, const char* key) {
  auto it = o.find(key);
  if (it == o.end()) return nullptr;
  return &it->second;
}

bool GetStringRequired(const util::json::Value::Object& o, const char* key, std::string* out, std::string* error) {
  const auto* v = Get(o, key);
  if (!v) return Fail(error, std::string("model.json: missing required field '") + key + "'");
  const auto* s = v->AsString();
  if (!s) return Fail(error, std::string("model.json: field '") + key + "' must be a string");
  if (s->empty()) return Fail(error, std::string("model.json: field '") + key + "' must be non-empty");
  *out = *s;
  return true;
}

bool GetIntRequired(const util::json::Value::Object& o, const char* key, int* out, std::string* error) {
  const auto* v = Get(o, key);
  if (!v) return Fail(error, std::string("model.json: missing required field '") + key + "'");
  const auto* n = v->AsNumber();
  if (!n) return Fail(error, std::string("model.json: field '") + key + "' must be a number");
  const int i = static_cast<int>(*n);
  if (static_cast<double>(i) != *n) {
    return Fail(error, std::string("model.json: field '") + key + "' must be an integer");
  }
  *out = i;
  return true;
}

bool GetNumberArray3Required(const util::json::Value::Object& o,
                             const char* key,
                             std::array<double, 3>* out,
                             std::string* error) {
  const auto* v = Get(o, key);
  if (!v) return Fail(error, std::string("model.json: missing required field '") + key + "'");
  const auto* a = v->AsArray();
  if (!a) return Fail(error, std::string("model.json: field '") + key + "' must be an array");
  if (a->size() != 3) return Fail(error, std::string("model.json: field '") + key + "' must have length 3");
  for (std::size_t i = 0; i < 3; ++i) {
    const auto* n = (*a)[i].AsNumber();
    if (!n) return Fail(error, std::string("model.json: field '") + key + "' must contain only numbers");
    (*out)[i] = *n;
  }
  return true;
}

bool IsOneOf(const std::string& v, std::initializer_list<const char*> allowed) {
  for (const char* a : allowed) {
    if (v == a) return true;
  }
  return false;
}

bool ParseModelJsonV1(const fs::path& pack_dir, ModelPack* out, std::string* error) {
  const fs::path manifestPath = pack_dir / "model.json";
  out->root_dir = pack_dir;
  out->manifest_path = manifestPath;

  const auto textOpt = util::ReadTextFile(manifestPath.string());
  if (!textOpt) {
    return Fail(error, std::string("missing model.json at ") + PathForError(manifestPath));
  }

  util::json::Value root;
  std::string parseErr;
  if (!util::json::Parse(*textOpt, &root, &parseErr)) {
    return Fail(error, std::string("model.json: ") + parseErr);
  }

  const auto* obj = root.AsObject();
  if (!obj) return Fail(error, "model.json: root must be an object");

  if (!GetStringRequired(*obj, "id", &out->id, error)) return false;
  if (!GetStringRequired(*obj, "display_name", &out->display_name, error)) return false;
  if (!GetStringRequired(*obj, "task", &out->task, error)) return false;
  if (!GetStringRequired(*obj, "onnx_filename", &out->onnx_filename, error)) return false;

  if (!IsOneOf(out->task, {"matting"})) {
    return Fail(error, "model.json: field 'task' must be 'matting' (v1)");
  }

  {
    const fs::path onnxRel(out->onnx_filename);
    if (!IsSafeRelativePath(onnxRel)) {
      return Fail(error, "model.json: field 'onnx_filename' must be a safe relative path");
    }
    out->onnx_path = pack_dir / onnxRel;
  }

  const auto* inputV = Get(*obj, "input");
  if (!inputV) return Fail(error, "model.json: missing required field 'input'");
  const auto* inputObj = AsObject(*inputV, error, "'input'");
  if (!inputObj) return false;

  if (!GetStringRequired(*inputObj, "name", &out->input.name, error)) return false;
  if (!GetStringRequired(*inputObj, "layout", &out->input.layout, error)) return false;
  if (!GetStringRequired(*inputObj, "dtype", &out->input.dtype, error)) return false;
  if (!GetIntRequired(*inputObj, "width", &out->input.width, error)) return false;
  if (!GetIntRequired(*inputObj, "height", &out->input.height, error)) return false;
  if (!GetIntRequired(*inputObj, "channels", &out->input.channels, error)) return false;

  if (!IsOneOf(out->input.layout, {"nchw", "nhwc"})) {
    return Fail(error, "model.json: input.layout must be one of: nchw, nhwc");
  }
  if (!IsOneOf(out->input.dtype, {"float32", "float16"})) {
    return Fail(error, "model.json: input.dtype must be one of: float32, float16");
  }
  if (out->input.width <= 0 || out->input.height <= 0 || out->input.channels <= 0) {
    return Fail(error, "model.json: input width/height/channels must be positive");
  }

  const auto* outputV = Get(*obj, "output");
  if (!outputV) return Fail(error, "model.json: missing required field 'output'");
  const auto* outputObj = AsObject(*outputV, error, "'output'");
  if (!outputObj) return false;

  if (!GetStringRequired(*outputObj, "name", &out->output.name, error)) return false;
  if (!GetStringRequired(*outputObj, "kind", &out->output.kind, error)) return false;
  if (!GetStringRequired(*outputObj, "dtype", &out->output.dtype, error)) return false;

  if (!IsOneOf(out->output.kind, {"alpha"})) {
    return Fail(error, "model.json: output.kind must be 'alpha' (v1)");
  }
  if (!IsOneOf(out->output.dtype, {"float32", "float16"})) {
    return Fail(error, "model.json: output.dtype must be one of: float32, float16");
  }

  const auto* ppV = Get(*obj, "preprocess");
  if (!ppV) return Fail(error, "model.json: missing required field 'preprocess'");
  const auto* ppObj = AsObject(*ppV, error, "'preprocess'");
  if (!ppObj) return false;

  if (!GetNumberArray3Required(*ppObj, "mean", &out->preprocess.mean, error)) return false;
  if (!GetNumberArray3Required(*ppObj, "std", &out->preprocess.std, error)) return false;
  if (!GetStringRequired(*ppObj, "color", &out->preprocess.color, error)) return false;
  if (!GetStringRequired(*ppObj, "range", &out->preprocess.range, error)) return false;

  if (!IsOneOf(out->preprocess.color, {"rgb"})) {
    return Fail(error, "model.json: preprocess.color must be 'rgb' (v1)");
  }
  if (!IsOneOf(out->preprocess.range, {"0..1"})) {
    return Fail(error, "model.json: preprocess.range must be '0..1' (v1)");
  }

  // Optional files
  const fs::path licPath = pack_dir / "LICENSE.txt";
  std::error_code ec;
  if (fs::exists(licPath, ec) && !ec) out->license_path = licPath;

  return true;
}


std::string PackDirKey(const fs::path& root, const fs::path& pack_dir) {
  // Use a stable, human-readable key for error reporting.
  // Prefer the relative path from the scan root, prefixed with the root directory name.
  // Example:
  //   open_video/segmentation/Best Quality
  std::error_code ec;
  fs::path rel = fs::relative(pack_dir, root, ec);
  std::string rels;
  if (!ec && !rel.empty() && rel != ".") {
    rels = rel.generic_string();
  } else {
    rels = pack_dir.filename().string();
  }

  const std::string rootName = root.filename().string();
  if (rootName.empty()) return rels;
  if (rels.empty()) return rootName;
  return rootName + "/" + rels;
}

std::vector<fs::path> DiscoverPackDirs(const fs::path& root) {
  std::vector<fs::path> out;
  std::error_code ec;

  for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec);
       !ec && it != fs::recursive_directory_iterator();
       it.increment(ec)) {
    const auto& e = *it;

    if (it.depth() > kMaxPackScanDepth) {
      it.disable_recursion_pending();
      continue;
    }

    if (!e.is_regular_file(ec) || ec) {
      ec.clear();
      continue;
    }

    if (e.path().filename() == "model.json") {
      out.push_back(e.path().parent_path());
    }
  }

  // Make deterministic.
  std::sort(out.begin(), out.end(), [](const fs::path& a, const fs::path& b) {
    return a.generic_string() < b.generic_string();
  });
  out.erase(std::unique(out.begin(), out.end()), out.end());

  return out;
}

}  // namespace

ModelPackRegistry ModelPackRegistry::Scan(const fs::path& open_cuda_models_dir) {
  ModelPackRegistry reg;
  reg.root_ = open_cuda_models_dir;

  std::error_code ec;
  if (!fs::exists(open_cuda_models_dir, ec) || ec) {
    // No directory is not an error; it just means nothing installed.
    return reg;
  }
  if (!fs::is_directory(open_cuda_models_dir, ec) || ec) {
    reg.problems_[open_cuda_models_dir.filename().string()] =
        std::string("model packs path is not a directory: ") + PathForError(open_cuda_models_dir);
    return reg;
  }

  // Packs are discovered by searching for model.json recursively.
  // This supports human-friendly categorization like:
  //   open_video/segmentation/Best Quality/model.json
  const std::vector<fs::path> dirs = DiscoverPackDirs(open_cuda_models_dir);


  std::map<std::string, fs::path> seenIds;
  for (const auto& d : dirs) {
    const std::string dirKey = PackDirKey(open_cuda_models_dir, d);

    ModelPack pack;
    std::string perr;
    if (!ParseModelJsonV1(d, &pack, &perr)) {
      reg.problems_[dirKey] = perr;
      continue;
    }

    if (auto it = seenIds.find(pack.id); it != seenIds.end()) {
      reg.problems_[pack.id] = std::string("duplicate model id '") + pack.id + "' in " +
                               PathForError(d) + " (already provided by " + PathForError(it->second) + ")";
      continue;
    }
    seenIds[pack.id] = d;

    if (!fs::exists(pack.onnx_path, ec) || ec) {
      reg.problems_[pack.id] = std::string("missing ONNX file: ") + PathForError(pack.onnx_path);
      ec.clear();
      continue;
    }
    if (!fs::is_regular_file(pack.onnx_path, ec) || ec) {
      reg.problems_[pack.id] = std::string("ONNX path is not a regular file: ") + PathForError(pack.onnx_path);
      ec.clear();
      continue;
    }

    reg.models_.push_back(std::move(pack));
  }

  std::sort(reg.models_.begin(), reg.models_.end(), [](const ModelPack& a, const ModelPack& b) {
    return a.id < b.id;
  });

  return reg;
}

ModelPackRegistry ModelPackRegistry::ScanDefault() {
  const auto modelsRoot = util::StudioCastModelsDir();
  if (modelsRoot.empty()) return {};
  // Open CUDA currently consumes only the matting/segmentation packs.
  // Other open-source video models (face detection, video denoise, eye contact, etc.)
  // live alongside these under open_video/, but are consumed by their own effect paths.
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
  if (ResolveModel("modnet")) return "modnet";
  if (models_.empty()) return {};
  return models_.front().id;
}

}  // namespace studiocast::open_cuda
