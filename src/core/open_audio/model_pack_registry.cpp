#include "core/open_audio/model_pack_registry.h"

#include <algorithm>
#include <sstream>

#include "core/util/fs.h"
#include "core/util/json.h"
#include "core/util/xdg.h"

namespace fs = std::filesystem;

namespace studiocast::model_integrity_internal {

std::string NormalizeSha256Hex(std::string s);
bool IsSha256Hex(const std::string &s);
bool ComputeSha256File(const fs::path &path, std::string *out,
                       std::string *error);

} // namespace studiocast::model_integrity_internal

namespace studiocast::open_audio {

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

bool IsPlaceholderModelId(const std::string &id) {
  return id.find("placeholder") != std::string::npos;
}

std::string ClassifyLoadError(const std::string &err) {
  if (err.find("missing ONNX file") != std::string::npos ||
      err.find("missing model.json") != std::string::npos ||
      err.find("No such") != std::string::npos) {
    return "missing";
  }
  return "invalid_manifest";
}

std::string ClassifiedMessage(const std::string &status,
                              const std::string &message) {
  if (message.empty())
    return status;
  if (message.starts_with(status + ":"))
    return message;
  return status + ": " + message;
}

bool ParseModelJsonV1(const fs::path &pack_dir, ModelPack *out,
                      std::string *error) {
  const fs::path manifestPath = pack_dir / "model.json";
  out->root_dir = pack_dir;
  out->manifest_path = manifestPath;

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
  if (!GetStringRequired(*obj, "display_name", &out->display_name, error))
    return false;
  if (!GetStringRequired(*obj, "onnx_filename", &out->onnx_filename, error))
    return false;

  const auto *originVal = Get(*obj, "origin");
  if (originVal) {
    const auto *originObj = originVal->AsObject();
    if (!originObj)
      return Fail(error, "model.json: field 'origin' must be an object");
    if (!GetStringOptional(*originObj, "sha256", &out->origin_sha256, error))
      return false;
  }

  // Optional fields
  if (!GetStringArrayOptional(*obj, "effects", &out->effects, error))
    return false;
  if (!GetIntOptional(*obj, "sample_rate", &out->sample_rate, error))
    return false;
  if (!GetIntOptional(*obj, "channels", &out->channels, error))
    return false;

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
    return Fail(error, "model.json: sample_rate must be 16000 or 48000 "
                       "(10ms-frame waveform models)");
  }

  // Optional: ONNX model I/O mapping for streaming models.
  const auto *ioVal = Get(*obj, "onnx_io");
  if (ioVal) {
    const auto *ioObj = ioVal->AsObject();
    if (!ioObj)
      return Fail(error, "model.json: field 'onnx_io' must be an object");

    out->has_onnx_io = true;
    if (!GetIntOptional(*ioObj, "frame_samples", &out->onnx_io.frame_samples,
                        error))
      return false;
    if (!GetStringOptional(*ioObj, "audio_input", &out->onnx_io.audio_input,
                           error))
      return false;
    if (!GetStringOptional(*ioObj, "audio_output", &out->onnx_io.audio_output,
                           error))
      return false;
    if (!GetStringArrayOptional(*ioObj, "state_inputs",
                                &out->onnx_io.state_inputs, error))
      return false;
    if (!GetStringArrayOptional(*ioObj, "state_outputs",
                                &out->onnx_io.state_outputs, error))
      return false;

    // Optional auxiliary inputs.
    const auto *auxVal = Get(*ioObj, "aux_inputs");
    if (auxVal) {
      const auto *auxObj = auxVal->AsObject();
      if (!auxObj)
        return Fail(error,
                    "model.json: field 'onnx_io.aux_inputs' must be an object");

      // Currently supported: aux_inputs.strength
      auto it = auxObj->find("strength");
      if (it != auxObj->end()) {
        const auto &v = it->second;
        out->onnx_io.has_strength_input = true;

        // Allow either a string (tensor name) or an object with more metadata.
        if (const auto *sname = v.AsString()) {
          if (sname->empty()) {
            return Fail(
                error,
                "model.json: onnx_io.aux_inputs.strength must be non-empty");
          }
          out->onnx_io.strength_input.name = *sname;
        } else {
          const auto *sobj = v.AsObject();
          if (!sobj) {
            return Fail(error, "model.json: onnx_io.aux_inputs.strength must "
                               "be a string or an object");
          }

          const auto *nameVal = Get(*sobj, "name");
          if (!nameVal)
            return Fail(error, "model.json: onnx_io.aux_inputs.strength is "
                               "missing required field 'name'");
          const auto *nameStr = nameVal->AsString();
          if (!nameStr || nameStr->empty()) {
            return Fail(error, "model.json: onnx_io.aux_inputs.strength.name "
                               "must be a non-empty string");
          }
          out->onnx_io.strength_input.name = *nameStr;

          const auto *rangeVal = Get(*sobj, "range");
          if (rangeVal) {
            const auto *a = rangeVal->AsArray();
            if (!a || a->size() != 2) {
              return Fail(error,
                          "model.json: onnx_io.aux_inputs.strength.range must "
                          "be an array of 2 numbers");
            }
            const auto *lo = (*a)[0].AsNumber();
            const auto *hi = (*a)[1].AsNumber();
            if (!lo || !hi) {
              return Fail(error,
                          "model.json: onnx_io.aux_inputs.strength.range must "
                          "contain only numbers");
            }
            out->onnx_io.strength_input.min_value = static_cast<float>(*lo);
            out->onnx_io.strength_input.max_value = static_cast<float>(*hi);
          }

          const auto *shapeVal = Get(*sobj, "shape");
          if (shapeVal) {
            const auto *a = shapeVal->AsArray();
            if (!a || a->empty()) {
              return Fail(error,
                          "model.json: onnx_io.aux_inputs.strength.shape must "
                          "be a non-empty array of integers");
            }
            out->onnx_io.strength_input.shape.clear();
            out->onnx_io.strength_input.shape.reserve(a->size());
            for (const auto &el : *a) {
              const auto *n = el.AsNumber();
              if (!n) {
                return Fail(error,
                            "model.json: onnx_io.aux_inputs.strength.shape "
                            "must contain only integers");
              }
              const int64_t d = static_cast<int64_t>(*n);
              if (static_cast<double>(d) != *n || d <= 0) {
                return Fail(error,
                            "model.json: onnx_io.aux_inputs.strength.shape "
                            "must contain only positive integers");
              }
              out->onnx_io.strength_input.shape.push_back(d);
            }
          }
        }

        // Defaults + validation.
        if (out->onnx_io.strength_input.shape.empty()) {
          out->onnx_io.strength_input.shape.push_back(1);
        }
        if (out->onnx_io.strength_input.max_value <
            out->onnx_io.strength_input.min_value) {
          return Fail(error, "model.json: onnx_io.aux_inputs.strength.range is "
                             "invalid (max < min)");
        }

        // For now, only scalar aux inputs are supported.
        int64_t prod = 1;
        for (const auto d : out->onnx_io.strength_input.shape) {
          if (d <= 0)
            return Fail(error, "model.json: onnx_io.aux_inputs.strength.shape "
                               "must be positive");
          if (prod > 1)
            break;
          prod *= d;
        }
        if (prod != 1) {
          return Fail(error, "model.json: onnx_io.aux_inputs.strength.shape "
                             "must have exactly 1 element (scalar)");
        }
      }
    }

    if (out->onnx_io.frame_samples < 0) {
      return Fail(error, "model.json: onnx_io.frame_samples must be >= 0");
    }

    // If frame_samples is specified, enforce 10ms framing to match the
    // real-time pipeline.
    if (out->onnx_io.frame_samples > 0) {
      const int expected = out->sample_rate / 100;
      if (expected <= 0 || out->onnx_io.frame_samples != expected) {
        return Fail(error, "model.json: onnx_io.frame_samples must match "
                           "sample_rate/100 (10ms frames)");
      }
    }

    // If state inputs are declared, state outputs must match in count (and vice
    // versa).
    if (!out->onnx_io.state_inputs.empty() ||
        !out->onnx_io.state_outputs.empty()) {
      if (out->onnx_io.state_inputs.size() !=
          out->onnx_io.state_outputs.size()) {
        return Fail(error, "model.json: onnx_io.state_inputs and state_outputs "
                           "must have the same length");
      }
    }
  }

  {
    const fs::path onnxRel(out->onnx_filename);
    if (!IsSafeRelativePath(onnxRel)) {
      return Fail(
          error,
          "model.json: field 'onnx_filename' must be a safe relative path");
    }
    out->onnx_path = pack_dir / onnxRel;
  }

  // Optional files
  const fs::path licPath = pack_dir / "LICENSE.txt";
  std::error_code ec;
  if (fs::exists(licPath, ec) && !ec)
    out->license_path = licPath;

  return true;
}

ModelFileVerification VerifyOnnxFile(const ModelPack &pack) {
  ModelFileVerification out;
  out.name = pack.onnx_filename;
  out.kind = "onnx";
  out.path = pack.onnx_path;
  out.expected_sha256 =
      studiocast::model_integrity_internal::NormalizeSha256Hex(
          pack.origin_sha256);

  std::error_code ec;
  if (!fs::exists(pack.onnx_path, ec) || ec) {
    out.status = "missing";
    out.message =
        std::string("missing ONNX file: ") + PathForError(pack.onnx_path);
    out.ok = false;
    return out;
  }
  if (!fs::is_regular_file(pack.onnx_path, ec) || ec) {
    out.status = "missing";
    out.message = std::string("ONNX path is not a regular file: ") +
                  PathForError(pack.onnx_path);
    out.ok = false;
    return out;
  }

  if (out.expected_sha256.empty()) {
    out.status = "unchecked";
    out.message = "no origin.sha256 in model.json";
    out.ok = true;
    return out;
  }
  if (!studiocast::model_integrity_internal::IsSha256Hex(
          out.expected_sha256)) {
    out.status = "invalid_manifest";
    out.message =
        "model.json: origin.sha256 must be a 64-character hex SHA-256 digest";
    out.ok = false;
    return out;
  }
  out.checksum_kind = "installed_sha256";

  std::string err;
  if (!studiocast::model_integrity_internal::ComputeSha256File(
          pack.onnx_path, &out.actual_sha256, &err)) {
    out.status = "read_error";
    out.message = err.empty() ? std::string("failed to hash ONNX file") : err;
    out.ok = false;
    return out;
  }

  if (out.actual_sha256 != out.expected_sha256) {
    out.status = "checksum_mismatch";
    out.message = "checksum_mismatch: expected " + out.expected_sha256 +
                  ", got " + out.actual_sha256;
    out.ok = false;
    return out;
  }

  out.status = "ok";
  out.message = "sha256 OK";
  out.ok = true;
  return out;
}

ModelPackVerification VerifyParsedPack(const ModelPack &pack) {
  ModelPackVerification out;
  out.id = pack.id;
  out.display_name = pack.display_name;
  out.root_dir = pack.root_dir;
  out.manifest_path = pack.manifest_path;

  if (IsPlaceholderModelId(pack.id)) {
    out.status = "placeholder";
    out.message =
        "placeholder: model id contains 'placeholder' and is skipped by default";
    out.ok = false;
    return out;
  }

  auto vf = VerifyOnnxFile(pack);
  out.files.push_back(std::move(vf));
  out.ok = out.files.front().ok;
  out.status = out.ok ? "ok" : out.files.front().status;
  out.message = out.ok ? "ok" : out.files.front().message;
  return out;
}

ModelPackVerification FailedVerification(const fs::path &pack_dir,
                                         const ModelPack &pack,
                                         const std::string &err) {
  ModelPackVerification out;
  out.id = pack.id.empty() ? pack_dir.filename().string() : pack.id;
  out.display_name = pack.display_name;
  out.root_dir = pack_dir;
  out.manifest_path = pack_dir / "model.json";
  out.status =
      IsPlaceholderModelId(pack.id) ? "placeholder" : ClassifyLoadError(err);
  const std::string msg =
      IsPlaceholderModelId(pack.id)
          ? "model id contains 'placeholder' and is skipped by default"
          : (err.empty() ? "failed to load model pack" : err);
  out.message = ClassifiedMessage(out.status, msg);
  out.ok = false;
  return out;
}

} // namespace

ModelPackRegistry
ModelPackRegistry::Scan(const fs::path &open_audio_models_dir) {
  ModelPackRegistry reg;
  reg.root_ = open_audio_models_dir;

  std::error_code ec;
  if (!fs::exists(open_audio_models_dir, ec) || ec) {
    // No directory is not an error; it just means nothing installed.
    return reg;
  }
  if (!fs::is_directory(open_audio_models_dir, ec) || ec) {
    reg.problems_[open_audio_models_dir.filename().string()] =
        std::string("open_audio models path is not a directory: ") +
        PathForError(open_audio_models_dir);
    return reg;
  }

  std::vector<fs::path> dirs;
  for (fs::directory_iterator it(open_audio_models_dir, ec);
       !ec && it != fs::directory_iterator(); it.increment(ec)) {
    const auto &e = *it;
    if (!e.is_directory(ec) || ec) {
      ec.clear();
      continue;
    }
    dirs.push_back(e.path());
  }

  std::sort(dirs.begin(), dirs.end(), [](const fs::path &a, const fs::path &b) {
    return a.filename().string() < b.filename().string();
  });

  std::map<std::string, fs::path> seenIds;
  for (const auto &d : dirs) {
    const std::string dirKey = d.filename().string();

    ModelPack pack;
    std::string perr;
    if (!ParseModelJsonV1(d, &pack, &perr)) {
      const std::string key = pack.id.empty() ? dirKey : pack.id;
      const std::string status =
          IsPlaceholderModelId(pack.id) ? "placeholder" : ClassifyLoadError(perr);
      const std::string msg =
          IsPlaceholderModelId(pack.id)
              ? "model id contains 'placeholder' and is skipped by default"
              : perr;
      reg.problems_[key] = ClassifiedMessage(status, msg);
      continue;
    }

    if (IsPlaceholderModelId(pack.id)) {
      reg.problems_[pack.id] =
          "placeholder: model id contains 'placeholder' and is skipped by "
          "default";
      continue;
    }

    if (auto it = seenIds.find(pack.id); it != seenIds.end()) {
      reg.problems_[pack.id] = std::string("duplicate model id '") + pack.id +
                               "' in " + PathForError(d) +
                               " (already provided by " +
                               PathForError(it->second) + ")";
      continue;
    }
    seenIds[pack.id] = d;

    if (!fs::exists(pack.onnx_path, ec) || ec) {
      reg.problems_[pack.id] =
          std::string("missing: missing ONNX file: ") +
          PathForError(pack.onnx_path);
      ec.clear();
      continue;
    }
    if (!fs::is_regular_file(pack.onnx_path, ec) || ec) {
      reg.problems_[pack.id] =
          std::string("missing: ONNX path is not a regular file: ") +
          PathForError(pack.onnx_path);
      ec.clear();
      continue;
    }

    reg.models_.push_back(std::move(pack));
  }

  std::sort(reg.models_.begin(), reg.models_.end(),
            [](const ModelPack &a, const ModelPack &b) { return a.id < b.id; });

  return reg;
}

std::vector<ModelPackVerification>
ModelPackRegistry::Verify(const fs::path &open_audio_models_dir) {
  std::vector<ModelPackVerification> out;

  std::error_code ec;
  if (!fs::exists(open_audio_models_dir, ec) || ec) {
    return out;
  }
  if (!fs::is_directory(open_audio_models_dir, ec) || ec) {
    ModelPackVerification r;
    r.id = open_audio_models_dir.filename().string();
    r.root_dir = open_audio_models_dir;
    r.manifest_path = open_audio_models_dir / "model.json";
    r.status = "invalid_manifest";
    r.message = "invalid_manifest: open_audio models path is not a directory: " +
                PathForError(open_audio_models_dir);
    r.ok = false;
    out.push_back(std::move(r));
    return out;
  }

  std::vector<fs::path> dirs;
  for (fs::directory_iterator it(open_audio_models_dir, ec);
       !ec && it != fs::directory_iterator(); it.increment(ec)) {
    const auto &e = *it;
    if (!e.is_directory(ec) || ec) {
      ec.clear();
      continue;
    }
    dirs.push_back(e.path());
  }
  if (ec) {
    ModelPackVerification r;
    r.id = open_audio_models_dir.string();
    r.root_dir = open_audio_models_dir;
    r.status = "read_error";
    r.message = std::string("failed to scan directory: ") + ec.message();
    r.ok = false;
    out.push_back(std::move(r));
    return out;
  }

  std::sort(dirs.begin(), dirs.end(), [](const fs::path &a, const fs::path &b) {
    return a.filename().string() < b.filename().string();
  });

  for (const auto &d : dirs) {
    ModelPack pack;
    std::string err;
    if (!ParseModelJsonV1(d, &pack, &err)) {
      out.push_back(FailedVerification(d, pack, err));
      continue;
    }

    out.push_back(VerifyParsedPack(pack));
  }

  std::sort(out.begin(), out.end(),
            [](const ModelPackVerification &a,
               const ModelPackVerification &b) { return a.id < b.id; });
  return out;
}

ModelPackRegistry ModelPackRegistry::ScanDefault() {
  const auto modelsRoot = studiocast::util::StudioCastModelsDir();
  if (modelsRoot.empty()) {
    // Fallback string; callers should surface install hints.
    return Scan(fs::path{"~/.local/share/studiocast/models/open_audio"});
  }
  return Scan(modelsRoot / "open_audio");
}

std::vector<ModelPackVerification> ModelPackRegistry::VerifyDefault() {
  const auto modelsRoot = studiocast::util::StudioCastModelsDir();
  if (modelsRoot.empty()) {
    return Verify(fs::path{"~/.local/share/studiocast/models/open_audio"});
  }
  return Verify(modelsRoot / "open_audio");
}

std::optional<ModelPack>
ModelPackRegistry::ResolveModel(const std::string &id) const {
  for (const auto &m : models_) {
    if (m.id == id)
      return m;
  }
  return std::nullopt;
}

std::string ModelPackRegistry::DefaultModelId() const {
  // Default for the most common case: microphone noise removal.
  return DefaultModelIdForEffect("noise_removal");
}

std::string
ModelPackRegistry::DefaultModelIdForEffect(const std::string &effect_id) const {
  if (effect_id.empty())
    return DefaultModelId();
  if (models_.empty())
    return {};

  // Candidate models that declare support for the effect.
  // If a model omits the `effects` list, we treat it as "supports all".
  std::vector<const ModelPack *> candidates;
  candidates.reserve(models_.size());
  for (const auto &m : models_) {
    if (m.effects.empty()) {
      candidates.push_back(&m);
      continue;
    }
    const bool supports = std::find(m.effects.begin(), m.effects.end(),
                                    effect_id) != m.effects.end();
    if (supports)
      candidates.push_back(&m);
  }

  // Curated defaults (keep deterministic): pick a good trade-off for each
  // effect. These are "best-effort" preferences; user config can override via
  // model_id/model_path.
  std::vector<std::string> prefer;
  if (effect_id == "studio_voice") {
    // Studio voice generally benefits from a stronger enhancer.
    prefer = {"fastenhancer_m_vd_v1", "fastenhancer_l_vd_v1"};
  } else if (effect_id == "room_echo_removal") {
    // Echo/room removal is harder; prefer a stronger model when available.
    prefer = {"fastenhancer_m_vd_v1", "fastenhancer_l_vd_v1",
              "fastenhancer_s_vd_v1"};
  } else if (effect_id == "noise_removal") {
    // Noise removal: prefer a lighter default, then scale up.
    prefer = {"fastenhancer_s_vd_v1", "fastenhancer_m_vd_v1",
              "fastenhancer_l_vd_v1"};
  } else {
    // Unknown effect: fall back to generic selection.
    prefer = {"fastenhancer_m_vd_v1", "fastenhancer_s_vd_v1",
              "fastenhancer_l_vd_v1"};
  }

  for (const auto &id : prefer) {
    for (const auto *m : candidates) {
      if (m && m->id == id)
        return m->id;
    }
  }

  // Deterministic fallback: first candidate, otherwise first installed model.
  if (!candidates.empty() && candidates.front())
    return candidates.front()->id;
  return models_.front().id;
}

} // namespace studiocast::open_audio
