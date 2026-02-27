#include "core/open_video/model_pack_registry.h"

#include <algorithm>
#include <map>
#include <sstream>

#include "core/util/fs.h"
#include "core/util/json.h"
#include "core/util/xdg.h"

namespace fs = std::filesystem;

namespace studiocast::open_video {
namespace {

std::string PathForError(const fs::path &p) {
  // Avoid platform-specific quoting. We only need human-readable paths.
  return p.string();
}

bool IsSafeRelativePath(const fs::path &p) {
  if (p.empty())
    return false;
  if (p.is_absolute())
    return false;
  for (const auto &part : p) {
    if (part == "." || part == "..")
      return false;
  }
  return true;
}

bool Fail(std::string *error, std::string msg) {
  if (error)
    *error = std::move(msg);
  return false;
}

const util::json::Value::Object *
AsObject(const util::json::Value &v, std::string *error, const char *what) {
  const auto *o = v.AsObject();
  if (o)
    return o;
  if (error)
    *error = std::string("model.json: expected object for ") + what;
  return nullptr;
}

const util::json::Value *Get(const util::json::Value::Object &o,
                             const char *key) {
  auto it = o.find(key);
  if (it == o.end())
    return nullptr;
  return &it->second;
}

bool GetStringRequired(const util::json::Value::Object &o, const char *key,
                       std::string *out, std::string *error) {
  const auto *v = Get(o, key);
  if (!v)
    return Fail(error, std::string("model.json: missing required field '") +
                           key + "'");
  const auto *s = v->AsString();
  if (!s)
    return Fail(error, std::string("model.json: field '") + key +
                           "' must be a string");
  if (s->empty())
    return Fail(error, std::string("model.json: field '") + key +
                           "' must be non-empty");
  *out = *s;
  return true;
}

bool GetStringOptional(const util::json::Value::Object &o, const char *key,
                       std::string *out, std::string *error) {
  const auto *v = Get(o, key);
  if (!v)
    return true;
  const auto *s = v->AsString();
  if (!s)
    return Fail(error, std::string("model.json: field '") + key +
                           "' must be a string");
  *out = *s;
  return true;
}

bool GetIntOptional(const util::json::Value::Object &o, const char *key,
                    int *out, std::string *error) {
  const auto *v = Get(o, key);
  if (!v)
    return true;
  const auto *n = v->AsNumber();
  if (!n)
    return Fail(error, std::string("model.json: field '") + key +
                           "' must be a number");
  const int i = static_cast<int>(*n);
  if (static_cast<double>(i) != *n) {
    return Fail(error, std::string("model.json: field '") + key +
                           "' must be an integer");
  }
  *out = i;
  return true;
}

bool GetIntRequired(const util::json::Value::Object &o, const char *key,
                    int *out, std::string *error) {
  const auto *v = Get(o, key);
  if (!v)
    return Fail(error, std::string("model.json: missing required field '") +
                           key + "'");
  const auto *n = v->AsNumber();
  if (!n)
    return Fail(error, std::string("model.json: field '") + key +
                           "' must be a number");
  const int i = static_cast<int>(*n);
  if (static_cast<double>(i) != *n) {
    return Fail(error, std::string("model.json: field '") + key +
                           "' must be an integer");
  }
  *out = i;
  return true;
}

bool GetNumberArray3Required(const util::json::Value::Object &o,
                             const char *key, std::array<double, 3> *out,
                             std::string *error) {
  const auto *v = Get(o, key);
  if (!v)
    return Fail(error, std::string("model.json: missing required field '") +
                           key + "'");
  const auto *a = v->AsArray();
  if (!a)
    return Fail(error, std::string("model.json: field '") + key +
                           "' must be an array");
  if (a->size() != 3)
    return Fail(error, std::string("model.json: field '") + key +
                           "' must have length 3");
  for (std::size_t i = 0; i < 3; ++i) {
    const auto *n = (*a)[i].AsNumber();
    if (!n)
      return Fail(error, std::string("model.json: field '") + key +
                             "' must contain only numbers");
    (*out)[i] = *n;
  }
  return true;
}

bool IsOneOf(const std::string &v,
             std::initializer_list<const char *> allowed) {
  for (const char *a : allowed) {
    if (v == a)
      return true;
  }
  return false;
}

bool GetStringArrayOptional(const util::json::Value::Object &o, const char *key,
                            std::vector<std::string> *out, std::string *error) {
  const auto *v = Get(o, key);
  if (!v)
    return true;
  const auto *a = v->AsArray();
  if (!a)
    return Fail(error, std::string("model.json: field '") + key +
                           "' must be an array");
  out->clear();
  out->reserve(a->size());
  for (const auto &el : *a) {
    const auto *s = el.AsString();
    if (!s)
      return Fail(error, std::string("model.json: field '") + key +
                             "' must contain only strings");
    if (s->empty())
      continue;
    out->push_back(*s);
  }
  return true;
}

bool ParseMattingSpecFromV1Fields(const util::json::Value::Object &root,
                                  MattingSpec *out, std::string *error) {
  const auto *inputV = Get(root, "input");
  if (!inputV)
    return Fail(error, "model.json: missing required field 'input'");
  const auto *inputObj = AsObject(*inputV, error, "'input'");
  if (!inputObj)
    return false;

  if (!GetStringRequired(*inputObj, "name", &out->input.name, error))
    return false;
  if (!GetStringRequired(*inputObj, "layout", &out->input.layout, error))
    return false;
  if (!GetStringRequired(*inputObj, "dtype", &out->input.dtype, error))
    return false;
  if (!GetIntRequired(*inputObj, "width", &out->input.width, error))
    return false;
  if (!GetIntRequired(*inputObj, "height", &out->input.height, error))
    return false;
  if (!GetIntRequired(*inputObj, "channels", &out->input.channels, error))
    return false;

  if (!IsOneOf(out->input.layout, {"nchw", "nhwc"})) {
    return Fail(error, "model.json: input.layout must be one of: nchw, nhwc");
  }
  if (!IsOneOf(out->input.dtype, {"float32", "float16"})) {
    return Fail(error,
                "model.json: input.dtype must be one of: float32, float16");
  }
  if (out->input.width <= 0 || out->input.height <= 0 ||
      out->input.channels <= 0) {
    return Fail(error,
                "model.json: input width/height/channels must be positive");
  }

  const auto *outputV = Get(root, "output");
  if (!outputV)
    return Fail(error, "model.json: missing required field 'output'");
  const auto *outputObj = AsObject(*outputV, error, "'output'");
  if (!outputObj)
    return false;

  if (!GetStringRequired(*outputObj, "name", &out->output.name, error))
    return false;
  if (!GetStringRequired(*outputObj, "kind", &out->output.kind, error))
    return false;
  if (!GetStringRequired(*outputObj, "dtype", &out->output.dtype, error))
    return false;

  if (!IsOneOf(out->output.kind, {"alpha"})) {
    return Fail(error, "model.json: output.kind must be 'alpha'");
  }
  if (!IsOneOf(out->output.dtype, {"float32", "float16"})) {
    return Fail(error,
                "model.json: output.dtype must be one of: float32, float16");
  }

  const auto *ppV = Get(root, "preprocess");
  if (!ppV)
    return Fail(error, "model.json: missing required field 'preprocess'");
  const auto *ppObj = AsObject(*ppV, error, "'preprocess'");
  if (!ppObj)
    return false;

  if (!GetNumberArray3Required(*ppObj, "mean", &out->preprocess.mean, error))
    return false;
  if (!GetNumberArray3Required(*ppObj, "std", &out->preprocess.std, error))
    return false;
  if (!GetStringRequired(*ppObj, "color", &out->preprocess.color, error))
    return false;
  if (!GetStringRequired(*ppObj, "range", &out->preprocess.range, error))
    return false;

  if (!IsOneOf(out->preprocess.color, {"rgb"})) {
    return Fail(error, "model.json: preprocess.color must be 'rgb'");
  }
  if (!IsOneOf(out->preprocess.range, {"0..1"})) {
    return Fail(error, "model.json: preprocess.range must be '0..1'");
  }

  return true;
}

bool ParseModelJsonV1(const fs::path &pack_dir, ModelPack *out,
                      std::string *error) {
  const fs::path manifestPath = pack_dir / "model.json";
  out->root_dir = pack_dir;
  out->manifest_path = manifestPath;
  out->schema_version = 1;

  const auto textOpt = util::ReadTextFile(manifestPath.string());
  if (!textOpt) {
    return Fail(error, std::string("missing model.json at ") +
                           PathForError(manifestPath));
  }

  util::json::Value root;
  std::string parseErr;
  if (!util::json::Parse(*textOpt, &root, &parseErr)) {
    return Fail(error, std::string("model.json: ") + parseErr);
  }

  const auto *obj = root.AsObject();
  if (!obj)
    return Fail(error, "model.json: root must be an object");

  if (!GetStringRequired(*obj, "id", &out->id, error))
    return false;
  if (!GetStringOptional(*obj, "display_name", &out->display_name, error))
    return false;
  if (!GetStringRequired(*obj, "task", &out->task, error))
    return false;

  std::string onnx_filename;
  if (!GetStringRequired(*obj, "onnx_filename", &onnx_filename, error))
    return false;

  const fs::path rel = onnx_filename;
  if (!IsSafeRelativePath(rel)) {
    return Fail(error,
                "model.json: onnx_filename must be a safe relative path");
  }

  ModelFile f;
  f.name = onnx_filename;
  f.kind = "onnx";
  f.role = "main";
  f.sha256 = "";
  f.path = pack_dir / rel;

  std::error_code ec;
  if (!fs::exists(f.path, ec) || ec) {
    return Fail(error,
                std::string("missing model file: ") + PathForError(f.path));
  }
  if (!fs::is_regular_file(f.path, ec) || ec) {
    return Fail(error, std::string("model file is not a regular file: ") +
                           PathForError(f.path));
  }

  out->files.clear();
  out->files.push_back(std::move(f));

  out->matting.reset();
  if (out->task == "matting") {
    MattingSpec spec;
    std::string perr;
    if (!ParseMattingSpecFromV1Fields(*obj, &spec, &perr)) {
      return Fail(error, perr);
    }
    out->matting = std::move(spec);
  }

  return true;
}

bool ParseModelJsonV2(const fs::path &pack_dir, ModelPack *out,
                      std::string *error) {
  const fs::path manifestPath = pack_dir / "model.json";
  out->root_dir = pack_dir;
  out->manifest_path = manifestPath;
  out->schema_version = 2;

  const auto textOpt = util::ReadTextFile(manifestPath.string());
  if (!textOpt) {
    return Fail(error, std::string("missing model.json at ") +
                           PathForError(manifestPath));
  }

  util::json::Value root;
  std::string parseErr;
  if (!util::json::Parse(*textOpt, &root, &parseErr)) {
    return Fail(error, std::string("model.json: ") + parseErr);
  }

  const auto *obj = root.AsObject();
  if (!obj)
    return Fail(error, "model.json: root must be an object");

  if (!GetStringRequired(*obj, "id", &out->id, error))
    return false;
  if (!GetStringOptional(*obj, "display_name", &out->display_name, error))
    return false;
  if (!GetStringRequired(*obj, "task", &out->task, error))
    return false;

  if (!GetStringArrayOptional(*obj, "depends_on", &out->depends_on, error))
    return false;

  const auto *filesVal = Get(*obj, "files");
  if (!filesVal)
    return Fail(error, "model.json: missing required field 'files'");
  const auto *filesArr = filesVal->AsArray();
  if (!filesArr)
    return Fail(error, "model.json: field 'files' must be an array");
  if (filesArr->empty())
    return Fail(error, "model.json: field 'files' must be non-empty");

  out->files.clear();
  out->files.reserve(filesArr->size());
  for (const auto &el : *filesArr) {
    const auto *fo = el.AsObject();
    if (!fo)
      return Fail(error, "model.json: files[] entries must be objects");

    ModelFile f;
    if (!GetStringRequired(*fo, "name", &f.name, error))
      return false;
    if (!GetStringRequired(*fo, "kind", &f.kind, error))
      return false;
    if (!GetStringOptional(*fo, "role", &f.role, error))
      return false;
    if (!GetStringOptional(*fo, "sha256", &f.sha256, error))
      return false;

    const fs::path rel = f.name;
    if (!IsSafeRelativePath(rel)) {
      return Fail(error,
                  "model.json: files[].name must be a safe relative path");
    }
    f.path = pack_dir / rel;

    std::error_code ec;
    if (!fs::exists(f.path, ec) || ec) {
      std::ostringstream oss;
      oss << "missing model file: " << PathForError(f.path);
      if (!out->task.empty())
        oss << " (task=" << out->task << ")";
      return Fail(error, oss.str());
    }
    if (!fs::is_regular_file(f.path, ec) || ec) {
      return Fail(error, std::string("model file is not a regular file: ") +
                             PathForError(f.path));
    }

    out->files.push_back(std::move(f));
  }

  out->matting.reset();
  if (out->task == "matting") {
    MattingSpec spec;
    std::string perr;
    if (!ParseMattingSpecFromV1Fields(*obj, &spec, &perr)) {
      return Fail(error, perr);
    }
    out->matting = std::move(spec);
  }

  return true;
}

std::string PackDirKey(const fs::path &root, const fs::path &pack_dir) {
  // Use a stable, human-readable key for error reporting.
  // Prefer the relative path from the scan root, prefixed with the root
  // directory name. Example (when scanning
  // ~/.local/share/studiocast/models/open_video):
  //   open_video/matting/Better Quality
  std::error_code ec;
  fs::path rel = fs::relative(pack_dir, root, ec);
  std::string rels;
  if (!ec && !rel.empty() && rel != ".") {
    rels = rel.generic_string();
  } else {
    rels = pack_dir.filename().string();
  }

  const std::string rootName = root.filename().string();
  if (rootName.empty())
    return rels;
  if (rels.empty())
    return rootName;
  return rootName + "/" + rels;
}

int BestEffortReadSchemaVersion(const fs::path &manifest) {
  const auto textOpt = util::ReadTextFile(manifest.string());
  if (!textOpt)
    return 1;

  util::json::Value root;
  std::string parseErr;
  if (!util::json::Parse(*textOpt, &root, &parseErr))
    return 1;

  const auto *obj = root.AsObject();
  if (!obj)
    return 1;

  int schema = 1;
  std::string ignore;
  if (!GetIntOptional(*obj, "schema_version", &schema, &ignore))
    return 1;
  return schema;
}

} // namespace

ModelPackRegistry
ModelPackRegistry::Scan(const std::filesystem::path &open_video_models_dir) {
  ModelPackRegistry reg;
  reg.root_ = open_video_models_dir;

  if (open_video_models_dir.empty()) {
    reg.problems_["(open_video)"] = "open_video models directory is empty";
    return reg;
  }

  if (!fs::exists(open_video_models_dir)) {
    // Not an error; just no models installed.
    return reg;
  }

  std::error_code ec;
  fs::recursive_directory_iterator it(
      open_video_models_dir, fs::directory_options::skip_permission_denied, ec);
  if (ec) {
    reg.problems_[open_video_models_dir.string()] =
        std::string("failed to scan directory: ") + ec.message();
    return reg;
  }

  for (const auto &entry : it) {
    if (!entry.is_regular_file())
      continue;
    if (entry.path().filename() != "model.json")
      continue;

    const fs::path manifest = entry.path();
    const fs::path pack_dir = manifest.parent_path();

    ModelPack pack;
    std::string err;

    const int schema = BestEffortReadSchemaVersion(manifest);

    bool ok = false;
    if (schema == 2) {
      ok = ParseModelJsonV2(pack_dir, &pack, &err);
    } else {
      ok = ParseModelJsonV1(pack_dir, &pack, &err);
    }

    if (!ok) {
      const std::string dirKey = PackDirKey(open_video_models_dir, pack_dir);
      const std::string key = pack.id.empty() ? dirKey : pack.id;
      reg.problems_[key] = err.empty() ? "failed to load model pack" : err;
      continue;
    }

    const fs::path lic = pack_dir / "LICENSE.txt";
    if (fs::exists(lic)) {
      pack.license_path = lic;
    }

    // Task-specific validation.
    // Matting packs must include at least one ONNX file, since the runtime
    // expects an ONNX model.
    if (pack.task == "matting") {
      bool has_onnx = false;
      for (const auto &f : pack.files) {
        if (f.kind == "onnx") {
          has_onnx = true;
          break;
        }
      }
      if (!has_onnx) {
        const std::string dirKey = PackDirKey(open_video_models_dir, pack_dir);
        const std::string key = pack.id.empty() ? dirKey : pack.id;
        reg.problems_[key] = "missing ONNX file (kind=onnx)";
        continue;
      }
    }

    reg.models_.push_back(std::move(pack));
  }

  // Sort deterministically.
  std::sort(reg.models_.begin(), reg.models_.end(),
            [](const ModelPack &a, const ModelPack &b) {
              if (a.task != b.task)
                return a.task < b.task;
              return a.id < b.id;
            });

  // Deduplicate by id (keep first, record problem for duplicates).
  std::map<std::string, fs::path> seen;
  std::vector<ModelPack> unique;
  unique.reserve(reg.models_.size());
  for (auto &m : reg.models_) {
    if (auto it2 = seen.find(m.id); it2 != seen.end()) {
      reg.problems_[m.id] = std::string("duplicate model id '") + m.id +
                            "' in " + PackDirKey(reg.root_, m.root_dir) +
                            " (already provided by " +
                            PackDirKey(reg.root_, it2->second) + ")";
      continue;
    }
    seen[m.id] = m.root_dir;
    unique.push_back(std::move(m));
  }
  reg.models_.swap(unique);

  // Build task -> model id index (deterministic ordering matches models_).
  reg.tasks_.clear();
  for (const auto &m : reg.models_) {
    reg.tasks_[m.task].push_back(m.id);
  }

  return reg;
}

ModelPackRegistry ModelPackRegistry::ScanDefault() {
  const auto modelsRoot = util::StudioCastModelsDir();
  if (modelsRoot.empty())
    return {};
  return Scan(modelsRoot / "open_video");
}

std::optional<ModelPack>
ModelPackRegistry::ResolveModel(const std::string &id) const {
  // models_ is sorted by (task, id) for human-friendly grouping in tools.
  // Model counts are small, so a linear search is fine and avoids maintaining a
  // separate index.
  for (const auto &m : models_) {
    if (m.id == id)
      return m;
  }
  return std::nullopt;
}

std::optional<ModelPack> ModelPackRegistry::Find(const std::string &task,
                                                 const std::string &id) const {
  if (id.empty())
    return std::nullopt;
  const auto m = ResolveModel(id);
  if (!m.has_value())
    return std::nullopt;
  if (!task.empty() && m->task != task)
    return std::nullopt;
  return m;
}

std::string
ModelPackRegistry::DefaultModelIdForTask(const std::string &task) const {
  if (task == "matting") {
    // Prefer the lightest matting model by default (good enough for tracking/segmentation
    // and keeps latency low on mid-range GPUs).
    if (Find("matting", "modnet-webnn-256-fp32"))
      return "modnet-webnn-256-fp32";

    // Fall back to a higher-quality option when MODNet isn't installed.
    if (Find("matting", "birefnet_lite"))
      return "birefnet_lite";
  }

  if (!task.empty()) {
    for (const auto &m : models_) {
      if (m.task == task)
        return m.id;
    }
  }
  if (models_.empty())
    return {};
  return models_.front().id;
}

} // namespace studiocast::open_video
