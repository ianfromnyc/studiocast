#include "core/open_video/model_pack_registry.h"

#include <algorithm>
#include <set>
#include <sstream>

#include "core/util/fs.h"
#include "core/util/json.h"
#include "core/util/xdg.h"

namespace fs = std::filesystem;

namespace studiocast::open_video {
namespace {

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

bool GetStringOptional(const util::json::Value::Object& o,
                       const char* key,
                       std::string* out,
                       std::string* error) {
  const auto* v = Get(o, key);
  if (!v) return true;
  const auto* s = v->AsString();
  if (!s) return Fail(error, std::string("model.json: field '") + key + "' must be a string");
  *out = *s;
  return true;
}

bool GetIntOptional(const util::json::Value::Object& o, const char* key, int* out, std::string* error) {
  const auto* v = Get(o, key);
  if (!v) return true;
  const auto* n = v->AsNumber();
  if (!n) return Fail(error, std::string("model.json: field '") + key + "' must be a number");
  const int i = static_cast<int>(*n);
  if (static_cast<double>(i) != *n) {
    return Fail(error, std::string("model.json: field '") + key + "' must be an integer");
  }
  *out = i;
  return true;
}

bool GetStringArrayOptional(const util::json::Value::Object& o,
                            const char* key,
                            std::vector<std::string>* out,
                            std::string* error) {
  const auto* v = Get(o, key);
  if (!v) return true;
  const auto* a = v->AsArray();
  if (!a) return Fail(error, std::string("model.json: field '") + key + "' must be an array");
  out->clear();
  out->reserve(a->size());
  for (const auto& el : *a) {
    const auto* s = el.AsString();
    if (!s) return Fail(error, std::string("model.json: field '") + key + "' must contain only strings");
    if (s->empty()) continue;
    out->push_back(*s);
  }
  return true;
}

bool ParseModelJsonV1(const fs::path& pack_dir, ModelPack* out, std::string* error) {
  const fs::path manifestPath = pack_dir / "model.json";
  out->root_dir = pack_dir;
  out->manifest_path = manifestPath;
  out->schema_version = 1;

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
  if (!GetStringOptional(*obj, "display_name", &out->display_name, error)) return false;
  if (!GetStringRequired(*obj, "task", &out->task, error)) return false;

  std::string onnx_filename;
  if (!GetStringRequired(*obj, "onnx_filename", &onnx_filename, error)) return false;

  const fs::path rel = onnx_filename;
  if (!IsSafeRelativePath(rel)) {
    return Fail(error, "model.json: onnx_filename must be a safe relative path");
  }

  ModelFile f;
  f.name = onnx_filename;
  f.kind = "onnx";
  f.role = "main";
  f.sha256 = "";
  f.path = pack_dir / rel;

  if (!fs::exists(f.path)) {
    return Fail(error, std::string("missing model file: ") + PathForError(f.path));
  }

  out->files.clear();
  out->files.push_back(std::move(f));

  return true;
}

bool ParseModelJsonV2(const fs::path& pack_dir, ModelPack* out, std::string* error) {
  const fs::path manifestPath = pack_dir / "model.json";
  out->root_dir = pack_dir;
  out->manifest_path = manifestPath;
  out->schema_version = 2;

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
  if (!GetStringOptional(*obj, "display_name", &out->display_name, error)) return false;
  if (!GetStringRequired(*obj, "task", &out->task, error)) return false;

  if (!GetStringArrayOptional(*obj, "depends_on", &out->depends_on, error)) return false;

  const auto* filesVal = Get(*obj, "files");
  if (!filesVal) return Fail(error, "model.json: missing required field 'files'");
  const auto* filesArr = filesVal->AsArray();
  if (!filesArr) return Fail(error, "model.json: field 'files' must be an array");
  if (filesArr->empty()) return Fail(error, "model.json: field 'files' must be non-empty");

  out->files.clear();
  out->files.reserve(filesArr->size());
  for (const auto& el : *filesArr) {
    const auto* fo = el.AsObject();
    if (!fo) return Fail(error, "model.json: files[] entries must be objects");

    ModelFile f;
    if (!GetStringRequired(*fo, "name", &f.name, error)) return false;
    if (!GetStringRequired(*fo, "kind", &f.kind, error)) return false;
    if (!GetStringOptional(*fo, "role", &f.role, error)) return false;
    if (!GetStringOptional(*fo, "sha256", &f.sha256, error)) return false;

    const fs::path rel = f.name;
    if (!IsSafeRelativePath(rel)) {
      return Fail(error, "model.json: files[].name must be a safe relative path");
    }
    f.path = pack_dir / rel;

    if (!fs::exists(f.path)) {
      std::ostringstream oss;
      oss << "missing model file: " << PathForError(f.path);
      if (!out->task.empty()) oss << " (task=" << out->task << ")";
      return Fail(error, oss.str());
    }

    out->files.push_back(std::move(f));
  }

  return true;
}

int BestEffortReadSchemaVersion(const fs::path& manifest) {
  const auto textOpt = util::ReadTextFile(manifest.string());
  if (!textOpt) return 1;

  util::json::Value root;
  std::string parseErr;
  if (!util::json::Parse(*textOpt, &root, &parseErr)) return 1;

  const auto* obj = root.AsObject();
  if (!obj) return 1;

  int schema = 1;
  std::string ignore;
  if (!GetIntOptional(*obj, "schema_version", &schema, &ignore)) return 1;
  return schema;
}

}  // namespace

ModelPackRegistry ModelPackRegistry::Scan(const std::filesystem::path& open_video_models_dir) {
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
  fs::recursive_directory_iterator it(open_video_models_dir, fs::directory_options::skip_permission_denied, ec);
  if (ec) {
    reg.problems_[open_video_models_dir.string()] = std::string("failed to scan directory: ") + ec.message();
    return reg;
  }

  for (const auto& entry : it) {
    if (!entry.is_regular_file()) continue;
    if (entry.path().filename() != "model.json") continue;

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
      const std::string key = pack.id.empty() ? pack_dir.filename().string() : pack.id;
      reg.problems_[key] = err.empty() ? "failed to load model pack" : err;
      continue;
    }

    const fs::path lic = pack_dir / "LICENSE.txt";
    if (fs::exists(lic)) {
      pack.license_path = lic;
    }

    reg.models_.push_back(std::move(pack));
  }

  // Sort deterministically.
  std::sort(reg.models_.begin(), reg.models_.end(), [](const ModelPack& a, const ModelPack& b) {
    if (a.task != b.task) return a.task < b.task;
    return a.id < b.id;
  });

  // Deduplicate by id (keep first, record problem for duplicates).
  std::set<std::string> seen;
  std::vector<ModelPack> unique;
  unique.reserve(reg.models_.size());
  for (auto& m : reg.models_) {
    if (!seen.insert(m.id).second) {
      reg.problems_[m.id] = "duplicate model id";
      continue;
    }
    unique.push_back(std::move(m));
  }
  reg.models_.swap(unique);

  // Build task -> model id index (deterministic ordering matches models_).
  reg.tasks_.clear();
  for (const auto& m : reg.models_) {
    reg.tasks_[m.task].push_back(m.id);
  }

  return reg;
}

ModelPackRegistry ModelPackRegistry::ScanDefault() {
  const auto modelsRoot = util::StudioCastModelsDir();
  if (modelsRoot.empty()) return {};
  return Scan(modelsRoot / "open_video");
}

std::optional<ModelPack> ModelPackRegistry::ResolveModel(const std::string& id) const {
  // models_ is sorted by (task, id) for human-friendly grouping in tools.
  // Model counts are small, so a linear search is fine and avoids maintaining a separate index.
  for (const auto& m : models_) {
    if (m.id == id) return m;
  }
  return std::nullopt;
}

std::optional<ModelPack> ModelPackRegistry::Find(const std::string& task, const std::string& id) const {
  if (id.empty()) return std::nullopt;
  const auto m = ResolveModel(id);
  if (!m.has_value()) return std::nullopt;
  if (!task.empty() && m->task != task) return std::nullopt;
  return m;
}

std::string ModelPackRegistry::DefaultModelIdForTask(const std::string& task) const {
  if (!task.empty()) {
    for (const auto& m : models_) {
      if (m.task == task) return m.id;
    }
  }
  if (models_.empty()) return {};
  return models_.front().id;
}

}  // namespace studiocast::open_video
