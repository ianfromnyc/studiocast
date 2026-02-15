#include "core/open_audio/model_pack_registry.h"

#include <algorithm>
#include <sstream>

#include "core/util/fs.h"
#include "core/util/json.h"
#include "core/util/xdg.h"

namespace fs = std::filesystem;

namespace studiocast::open_audio {

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
  if (!GetStringRequired(*obj, "onnx_filename", &out->onnx_filename, error)) return false;

  // Optional fields
  if (!GetStringArrayOptional(*obj, "effects", &out->effects, error)) return false;
  if (!GetIntOptional(*obj, "sample_rate", &out->sample_rate, error)) return false;
  if (!GetIntOptional(*obj, "channels", &out->channels, error)) return false;

  if (out->sample_rate <= 0) {
    return Fail(error, "model.json: sample_rate must be positive");
  }
  if (out->channels <= 0) {
    return Fail(error, "model.json: channels must be positive");
  }

  // MVP engine currently supports only mono 16kHz or 48kHz waveform models.
  if (out->channels != 1) {
    return Fail(error, "model.json: channels must be 1 (mono models only)");
  }
  if (out->sample_rate != 16000 && out->sample_rate != 48000) {
    return Fail(error, "model.json: sample_rate must be 16000 or 48000 (10ms-frame waveform models)");
  }

  // Optional: ONNX model I/O mapping for streaming models.
  const auto* ioVal = Get(*obj, "onnx_io");
  if (ioVal) {
    const auto* ioObj = ioVal->AsObject();
    if (!ioObj) return Fail(error, "model.json: field 'onnx_io' must be an object");

    out->has_onnx_io = true;
    if (!GetIntOptional(*ioObj, "frame_samples", &out->onnx_io.frame_samples, error)) return false;
    if (!GetStringOptional(*ioObj, "audio_input", &out->onnx_io.audio_input, error)) return false;
    if (!GetStringOptional(*ioObj, "audio_output", &out->onnx_io.audio_output, error)) return false;
    if (!GetStringArrayOptional(*ioObj, "state_inputs", &out->onnx_io.state_inputs, error)) return false;
    if (!GetStringArrayOptional(*ioObj, "state_outputs", &out->onnx_io.state_outputs, error)) return false;

    if (out->onnx_io.frame_samples < 0) {
      return Fail(error, "model.json: onnx_io.frame_samples must be >= 0");
    }

    // If frame_samples is specified, enforce 10ms framing to match the real-time pipeline.
    if (out->onnx_io.frame_samples > 0) {
      const int expected = out->sample_rate / 100;
      if (expected <= 0 || out->onnx_io.frame_samples != expected) {
        return Fail(error, "model.json: onnx_io.frame_samples must match sample_rate/100 (10ms frames)");
      }
    }

    // If state inputs are declared, state outputs must match in count (and vice versa).
    if (!out->onnx_io.state_inputs.empty() || !out->onnx_io.state_outputs.empty()) {
      if (out->onnx_io.state_inputs.size() != out->onnx_io.state_outputs.size()) {
        return Fail(error,
                    "model.json: onnx_io.state_inputs and state_outputs must have the same length");
      }
    }
  }

  {
    const fs::path onnxRel(out->onnx_filename);
    if (!IsSafeRelativePath(onnxRel)) {
      return Fail(error, "model.json: field 'onnx_filename' must be a safe relative path");
    }
    out->onnx_path = pack_dir / onnxRel;
  }

  // Optional files
  const fs::path licPath = pack_dir / "LICENSE.txt";
  std::error_code ec;
  if (fs::exists(licPath, ec) && !ec) out->license_path = licPath;

  return true;
}

}  // namespace

ModelPackRegistry ModelPackRegistry::Scan(const fs::path& open_audio_models_dir) {
  ModelPackRegistry reg;
  reg.root_ = open_audio_models_dir;

  std::error_code ec;
  if (!fs::exists(open_audio_models_dir, ec) || ec) {
    // No directory is not an error; it just means nothing installed.
    return reg;
  }
  if (!fs::is_directory(open_audio_models_dir, ec) || ec) {
    reg.problems_[open_audio_models_dir.filename().string()] =
        std::string("open_audio models path is not a directory: ") + PathForError(open_audio_models_dir);
    return reg;
  }

  std::vector<fs::path> dirs;
  for (fs::directory_iterator it(open_audio_models_dir, ec);
       !ec && it != fs::directory_iterator();
       it.increment(ec)) {
    const auto& e = *it;
    if (!e.is_directory(ec) || ec) {
      ec.clear();
      continue;
    }
    dirs.push_back(e.path());
  }

  std::sort(dirs.begin(), dirs.end(), [](const fs::path& a, const fs::path& b) {
    return a.filename().string() < b.filename().string();
  });

  std::map<std::string, fs::path> seenIds;
  for (const auto& d : dirs) {
    const std::string dirKey = d.filename().string();

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
  const auto modelsRoot = studiocast::util::StudioCastModelsDir();
  if (modelsRoot.empty()) {
    // Fallback string; callers should surface install hints.
    return Scan(fs::path{"~/.local/share/studiocast/models/open_audio"});
  }
  return Scan(modelsRoot / "open_audio");
}

std::optional<ModelPack> ModelPackRegistry::ResolveModel(const std::string& id) const {
  for (const auto& m : models_) {
    if (m.id == id) return m;
  }
  return std::nullopt;
}

std::string ModelPackRegistry::DefaultModelId() const {
  if (models_.empty()) return std::string();
  return models_.front().id;
}

}  // namespace studiocast::open_audio
