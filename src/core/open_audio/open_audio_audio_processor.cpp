#include "core/open_audio/open_audio_audio_processor.h"

#include <algorithm>
#include <system_error>

#include "core/open_audio/model_pack_registry.h"
#include "core/util/fs.h"
#include "core/util/json.h"
#include "core/util/xdg.h"

namespace fs = std::filesystem;

namespace studiocast::open_audio {

// Forward-declared in the header; Phase 5 uses these for 48k <-> model rate conversion.
// Define them here so `std::unique_ptr` deletion sees complete types.
struct OpenAudioAudioProcessor::Decimator3 {};
struct OpenAudioAudioProcessor::Interpolator3 {};

namespace {

fs::path ExpandTilde(fs::path p) {
  const std::string s = p.string();
  if (s == "~") {
    return studiocast::util::HomeDir();
  }
  if (s.rfind("~/", 0) == 0) {
    return studiocast::util::HomeDir() / s.substr(2);
  }
  return p;
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

const studiocast::util::json::Value* Get(const studiocast::util::json::Value::Object& o, const char* key) {
  auto it = o.find(key);
  if (it == o.end()) return nullptr;
  return &it->second;
}

bool GetStringRequired(const studiocast::util::json::Value::Object& o,
                       const char* key,
                       std::string* out,
                       std::string* error) {
  const auto* v = Get(o, key);
  if (!v) return Fail(error, std::string("model.json: missing required field '") + key + "'");
  const auto* s = v->AsString();
  if (!s) return Fail(error, std::string("model.json: field '") + key + "' must be a string");
  if (s->empty()) return Fail(error, std::string("model.json: field '") + key + "' must be non-empty");
  *out = *s;
  return true;
}

bool ResolveFromPackDir(const fs::path& pack_dir, ResolvedOpenAudioModel* out, std::string* error) {
  const fs::path manifestPath = pack_dir / "model.json";
  const auto textOpt = studiocast::util::ReadTextFile(manifestPath.string());
  if (!textOpt) {
    return Fail(error, std::string("missing model.json at ") + manifestPath.string());
  }

  studiocast::util::json::Value root;
  std::string parseErr;
  if (!studiocast::util::json::Parse(*textOpt, &root, &parseErr)) {
    return Fail(error, std::string("model.json: ") + parseErr);
  }
  const auto* obj = root.AsObject();
  if (!obj) return Fail(error, "model.json: root must be an object");

  std::string id;
  std::string display;
  std::string onnxFilename;
  if (!GetStringRequired(*obj, "id", &id, error)) return false;
  if (!GetStringRequired(*obj, "display_name", &display, error)) return false;
  if (!GetStringRequired(*obj, "onnx_filename", &onnxFilename, error)) return false;

  const fs::path rel(onnxFilename);
  if (!IsSafeRelativePath(rel)) {
    return Fail(error, "model.json: field 'onnx_filename' must be a safe relative path");
  }
  const fs::path onnxPath = pack_dir / rel;

  std::error_code ec;
  if (!fs::exists(onnxPath, ec) || ec) {
    ec.clear();
    return Fail(error, std::string("missing ONNX file: ") + onnxPath.string());
  }
  if (!fs::is_regular_file(onnxPath, ec) || ec) {
    ec.clear();
    return Fail(error, std::string("ONNX path is not a regular file: ") + onnxPath.string());
  }

  if (out) {
    out->model_id = id;
    out->display_name = display;
    out->onnx_path = onnxPath;
    out->sample_rate = 0;  // unknown for user-specified packs resolved here
    out->channels = 1;
    out->is_user_path = true;
  }
  return true;
}

bool ResolveFromOnnxFile(const fs::path& onnxPath, ResolvedOpenAudioModel* out, std::string* error) {
  std::error_code ec;
  if (!fs::exists(onnxPath, ec) || ec) {
    ec.clear();
    return Fail(error, std::string("model_path does not exist: ") + onnxPath.string());
  }
  if (!fs::is_regular_file(onnxPath, ec) || ec) {
    ec.clear();
    return Fail(error, std::string("model_path is not a file: ") + onnxPath.string());
  }

  // Extension check is best-effort; allow non-.onnx for advanced users.
  if (out) {
    out->model_id.clear();
    out->display_name = onnxPath.filename().string();
    out->onnx_path = onnxPath;
    out->sample_rate = 0;  // unknown; caller may assume pipeline sample rate
    out->channels = 1;
    out->is_user_path = true;
  }
  return true;
}

}  // namespace

bool ResolveOpenAudioModelForMicrophone(const studiocast::audio::effects::BroadcastAudioEffects& fx,
                                       ResolvedOpenAudioModel* out,
                                       std::string* error) {
  if (error) error->clear();

  const auto& mic = fx.microphone;

  // 1) Explicit model path wins.
  if (!mic.model_path.empty()) {
    fs::path p = ExpandTilde(fs::path(mic.model_path));
    std::error_code ec;
    if (!fs::exists(p, ec) || ec) {
      ec.clear();
      return Fail(error, std::string("Open Audio model_path does not exist: ") + p.string());
    }
    if (fs::is_directory(p, ec) && !ec) {
      return ResolveFromPackDir(p, out, error);
    }
    ec.clear();
    return ResolveFromOnnxFile(p, out, error);
  }

  // 2/3) Installed pack id or default.
  const auto reg = ModelPackRegistry::ScanDefault();

  std::string id = mic.model_id;
  if (id.empty()) {
    id = reg.DefaultModelId();
  }
  if (id.empty()) {
    return Fail(error,
                "Open Audio: no usable model packs found (install under ~/.local/share/studiocast/models/open_audio/<model_id>/)."
    );
  }

  const auto packOpt = reg.ResolveModel(id);
  if (!packOpt.has_value()) {
    std::string msg = std::string("Open Audio: model_id '") + id + "' not found.";
    if (!reg.ListModels().empty()) {
      msg += " Available models: ";
      for (std::size_t i = 0; i < reg.ListModels().size(); ++i) {
        if (i) msg += ", ";
        msg += reg.ListModels()[i].id;
      }
      msg += ".";
    }
    return Fail(error, msg);
  }

  const auto& pack = *packOpt;
  std::error_code ec;
  if (!fs::exists(pack.onnx_path, ec) || ec) {
    ec.clear();
    return Fail(error, std::string("Open Audio: missing ONNX file: ") + pack.onnx_path.string());
  }
  if (!fs::is_regular_file(pack.onnx_path, ec) || ec) {
    ec.clear();
    return Fail(error, std::string("Open Audio: ONNX path is not a file: ") + pack.onnx_path.string());
  }

  if (out) {
    out->model_id = pack.id;
    out->display_name = pack.display_name;
    out->onnx_path = pack.onnx_path;
    out->sample_rate = pack.sample_rate;
    out->channels = pack.channels;
    out->is_user_path = false;
  }
  return true;
}

bool ResolveOpenAudioModelForSpeaker(const studiocast::audio::effects::BroadcastAudioEffects& fx,
                                    ResolvedOpenAudioModel* out,
                                    std::string* error) {
  if (error) error->clear();

  const auto& spk = fx.speaker;

  // 1) Explicit model path wins.
  if (!spk.model_path.empty()) {
    fs::path p = ExpandTilde(fs::path(spk.model_path));
    std::error_code ec;
    if (!fs::exists(p, ec) || ec) {
      ec.clear();
      return Fail(error, std::string("Open Audio speaker model_path does not exist: ") + p.string());
    }
    if (fs::is_directory(p, ec) && !ec) {
      return ResolveFromPackDir(p, out, error);
    }
    ec.clear();
    return ResolveFromOnnxFile(p, out, error);
  }

  // 2/3) Installed pack id or default.
  const auto reg = ModelPackRegistry::ScanDefault();

  std::string id = spk.model_id;
  if (id.empty()) {
    id = reg.DefaultModelId();
  }
  if (id.empty()) {
    return Fail(error,
                "Open Audio: no usable model packs found (install under ~/.local/share/studiocast/models/open_audio/<model_id>/)."
    );
  }

  const auto packOpt = reg.ResolveModel(id);
  if (!packOpt.has_value()) {
    std::string msg = std::string("Open Audio: model_id '") + id + "' not found.";
    if (!reg.ListModels().empty()) {
      msg += " Available models: ";
      for (std::size_t i = 0; i < reg.ListModels().size(); ++i) {
        if (i) msg += ", ";
        msg += reg.ListModels()[i].id;
      }
      msg += ".";
    }
    return Fail(error, msg);
  }

  const auto& pack = *packOpt;
  std::error_code ec;
  if (!fs::exists(pack.onnx_path, ec) || ec) {
    ec.clear();
    return Fail(error, std::string("Open Audio: missing ONNX file: ") + pack.onnx_path.string());
  }
  if (!fs::is_regular_file(pack.onnx_path, ec) || ec) {
    ec.clear();
    return Fail(error, std::string("Open Audio: ONNX path is not a file: ") + pack.onnx_path.string());
  }

  if (out) {
    out->model_id = pack.id;
    out->display_name = pack.display_name;
    out->onnx_path = pack.onnx_path;
    out->sample_rate = pack.sample_rate;
    out->channels = pack.channels;
    out->is_user_path = false;
  }
  return true;
}

std::unique_ptr<OpenAudioAudioProcessor> OpenAudioAudioProcessor::CreateForMicrophone(
    const studiocast::audio::effects::BroadcastAudioEffects& fx,
    ResolvedOpenAudioModel* resolved_out,
    std::string* error) {
  OrtSessionOptions opts;
  opts.prefer_cuda = true;
  opts.cuda_device_id = 0;
  return CreateForMicrophoneWithOrtOptions(fx, opts, resolved_out, error);
}

std::unique_ptr<OpenAudioAudioProcessor> OpenAudioAudioProcessor::CreateForSpeaker(
    const studiocast::audio::effects::BroadcastAudioEffects& fx,
    ResolvedOpenAudioModel* resolved_out,
    std::string* error) {
  OrtSessionOptions opts;
  opts.prefer_cuda = true;
  opts.cuda_device_id = 0;
  return CreateForSpeakerWithOrtOptions(fx, opts, resolved_out, error);
}

std::unique_ptr<OpenAudioAudioProcessor> OpenAudioAudioProcessor::CreateForSpeakerWithOrtOptions(
    const studiocast::audio::effects::BroadcastAudioEffects& fx,
    const studiocast::open_audio::OrtSessionOptions& ort_opts,
    ResolvedOpenAudioModel* resolved_out,
    std::string* error) {
#if !STUDIOCAST_ENABLE_OPEN_AUDIO
  (void)fx;
  (void)ort_opts;
  (void)resolved_out;
  if (error) *error = "Open Audio backend is disabled in this build (STUDIOCAST_ENABLE_OPEN_AUDIO=0).";
  return nullptr;
#else
  ResolvedOpenAudioModel resolved;
  std::string err;
  if (!ResolveOpenAudioModelForSpeaker(fx, &resolved, &err)) {
    if (error) *error = err;
    return nullptr;
  }
  if (resolved_out) *resolved_out = resolved;

#if !STUDIOCAST_HAVE_ONNXRUNTIME
  (void)ort_opts;
  if (error) {
    *error =
        "Open Audio backend unavailable: ONNX Runtime was not found at build time (STUDIOCAST_HAVE_ONNXRUNTIME=0).";
  }
  return nullptr;
#else
  OrtSessionInfo si;
  std::string ort_err;
  const auto onnx_path = resolved.onnx_path;
  auto session = OpenAudioOrtSession::Create(onnx_path, ort_opts, &si, &ort_err);
  if (!session) {
    if (error) {
      *error = ort_err.empty() ? "Failed to create ONNX Runtime session for Open Audio model." : ort_err;
    }
    return nullptr;
  }

  auto proc = std::make_unique<OpenAudioAudioProcessor>(std::move(resolved));
  proc->UpdateFromSpeakerConfig(fx.speaker);

  if (si.using_cuda) {
    proc->ort_session_cuda_ = std::move(session);
    proc->ort_session_active_ = proc->ort_session_cuda_.get();

    OrtSessionOptions cpu_opts = ort_opts;
    cpu_opts.prefer_cuda = false;
    OrtSessionInfo cpu_si;
    std::string cpu_err;
    auto cpu = OpenAudioOrtSession::Create(onnx_path, cpu_opts, &cpu_si, &cpu_err);
    if (cpu) {
      proc->ort_session_cpu_ = std::move(cpu);
    }
  } else {
    proc->ort_session_cpu_ = std::move(session);
    proc->ort_session_active_ = proc->ort_session_cpu_.get();
    proc->using_cpu_fallback_ = true;
  }

  return proc;
#endif  // STUDIOCAST_HAVE_ONNXRUNTIME
#endif  // STUDIOCAST_ENABLE_OPEN_AUDIO
}

std::unique_ptr<OpenAudioAudioProcessor> OpenAudioAudioProcessor::CreateForMicrophoneWithOrtOptions(
    const studiocast::audio::effects::BroadcastAudioEffects& fx,
    const studiocast::open_audio::OrtSessionOptions& ort_opts,
    ResolvedOpenAudioModel* resolved_out,
    std::string* error) {
#if !STUDIOCAST_ENABLE_OPEN_AUDIO
  (void)fx;
  (void)ort_opts;
  (void)resolved_out;
  if (error) *error = "Open Audio backend is disabled in this build (STUDIOCAST_ENABLE_OPEN_AUDIO=0).";
  return nullptr;
#else
  ResolvedOpenAudioModel resolved;
  std::string err;
  if (!ResolveOpenAudioModelForMicrophone(fx, &resolved, &err)) {
    if (error) *error = err;
    return nullptr;
  }
  if (resolved_out) *resolved_out = resolved;

#if !STUDIOCAST_HAVE_ONNXRUNTIME
  (void)ort_opts;
  if (error) {
    *error =
        "Open Audio backend unavailable: ONNX Runtime was not found at build time (STUDIOCAST_HAVE_ONNXRUNTIME=0).";
  }
  return nullptr;
#else
  // Create the ORT session up-front so we fail fast (actionable error)
  // and avoid repeatedly attempting to load the model in the realtime thread.
  OrtSessionInfo si;
  std::string ort_err;
  const auto onnx_path = resolved.onnx_path;
  auto session = OpenAudioOrtSession::Create(onnx_path, ort_opts, &si, &ort_err);
  if (!session) {
    if (error) {
      *error = ort_err.empty() ? "Failed to create ONNX Runtime session for Open Audio model." : ort_err;
    }
    return nullptr;
  }

  auto proc = std::make_unique<OpenAudioAudioProcessor>(std::move(resolved));
  proc->UpdateFromMicrophoneConfig(fx.microphone);

  if (si.using_cuda) {
    proc->ort_session_cuda_ = std::move(session);
    proc->ort_session_active_ = proc->ort_session_cuda_.get();

    // Best-effort CPU fallback session (used when CUDA session fails at runtime).
    OrtSessionOptions cpu_opts = ort_opts;
    cpu_opts.prefer_cuda = false;
    OrtSessionInfo cpu_si;
    std::string cpu_err;
    auto cpu = OpenAudioOrtSession::Create(onnx_path, cpu_opts, &cpu_si, &cpu_err);
    if (cpu) {
      proc->ort_session_cpu_ = std::move(cpu);
    }
  } else {
    proc->ort_session_cpu_ = std::move(session);
    proc->ort_session_active_ = proc->ort_session_cpu_.get();
    proc->using_cpu_fallback_ = true;
  }

  return proc;
#endif  // STUDIOCAST_HAVE_ONNXRUNTIME
#endif  // STUDIOCAST_ENABLE_OPEN_AUDIO
}

OpenAudioAudioProcessor::OpenAudioAudioProcessor(ResolvedOpenAudioModel model) : model_(std::move(model)) {
  if (model_.sample_rate > 0) {
    model_sample_rate_ = model_.sample_rate;
  }
  if (model_.has_onnx_io && model_.onnx_io.frame_samples > 0) {
    model_frame_samples_ = static_cast<std::uint32_t>(model_.onnx_io.frame_samples);
  } else if (model_sample_rate_ > 0) {
    model_frame_samples_ = static_cast<std::uint32_t>(model_sample_rate_ / 100);
  }
}

OpenAudioAudioProcessor::~OpenAudioAudioProcessor() = default;

void OpenAudioAudioProcessor::UpdateFromMicrophoneConfig(const studiocast::audio::effects::BroadcastMicrophoneEffects& mic) {
  int s = mic.strength;
  if (s < 0) s = 0;
  if (s > 100) s = 100;
  strength_.store(s);
  studio_voice_enabled_.store(mic.studio_voice_enabled);
}

void OpenAudioAudioProcessor::UpdateFromSpeakerConfig(const studiocast::audio::effects::BroadcastSpeakerEffects& spk) {
  int s = spk.strength;
  if (s < 0) s = 0;
  if (s > 100) s = 100;
  strength_.store(s);
  studio_voice_enabled_.store(false);
}

void OpenAudioAudioProcessor::Reset() {
  mono_in_.clear();
  mono_out_.clear();
  model_in_.clear();
  model_out_.clear();
  sticky_warning_.clear();
  model_disabled_ = false;
  using_cpu_fallback_ = false;
  if (ort_session_cuda_) {
    ort_session_active_ = ort_session_cuda_.get();
  } else {
    ort_session_active_ = ort_session_cpu_.get();
  }
}

bool OpenAudioAudioProcessor::Process(const float* in,
                                      float* out,
                                      std::uint32_t frames,
                                      std::uint32_t channels,
                                      std::string* error) {
  if (!in || !out) {
    if (error) *error = "null audio buffer";
    return false;
  }

  const std::uint64_t samples64 = static_cast<std::uint64_t>(frames) * static_cast<std::uint64_t>(channels);
  const auto samples = static_cast<std::size_t>(samples64);
  if (samples == 0) return true;

  // If ORT session is not available (should not happen after CreateForMicrophone),
  // fall back to pass-through.
  if (!ort_session_active_ || model_disabled_) {
    std::copy_n(in, samples, out);
    return true;
  }

  // Convert interleaved audio to mono for the model.
  mono_in_.resize(frames);
  if (channels == 1) {
    std::copy_n(in, frames, mono_in_.data());
  } else {
    for (std::uint32_t f = 0; f < frames; ++f) {
      float acc = 0.0f;
      for (std::uint32_t c = 0; c < channels; ++c) {
        const std::size_t idx = static_cast<std::size_t>(f) * channels + c;
        acc += in[idx];
      }
      mono_in_[f] = acc / static_cast<float>(channels);
    }
  }

  mono_out_.resize(frames);
  std::size_t out_samples = 0;
  std::string ort_err;
  if (!ort_session_active_->Run1D(mono_in_.data(), frames, mono_out_.data(), mono_out_.size(), &out_samples, &ort_err)) {
    // Fail open: log a best-effort error string and pass input through unchanged.
    if (error) *error = std::string("Open Audio ORT run failed: ") + ort_err;

    // If CUDA is active and CPU fallback exists, switch over once.
    if (!using_cpu_fallback_ && ort_session_cuda_ && ort_session_cpu_) {
      ort_session_active_ = ort_session_cpu_.get();
      using_cpu_fallback_ = true;
      sticky_warning_ = "Open Audio: switched to CPU fallback after a CUDA runtime failure.";
    } else {
      model_disabled_ = true;
      sticky_warning_ = "Open Audio: disabled after repeated runtime failures.";
    }
    std::copy_n(in, samples, out);
    return true;
  }

  if (out_samples < frames) {
    // If the model produced fewer samples than requested, pad with dry mono input.
    std::copy(mono_in_.begin() + static_cast<std::ptrdiff_t>(out_samples),
              mono_in_.begin() + static_cast<std::ptrdiff_t>(frames),
              mono_out_.begin() + static_cast<std::ptrdiff_t>(out_samples));
    out_samples = frames;
  }

  // Map user "strength" (0..100) to a wet/dry mix.
  const int strength = strength_.load();
  const float t = static_cast<float>(strength) / 100.0f;
  const float wet = 0.2f + 0.8f * t;  // 0.2 .. 1.0
  const float dry = 1.0f - wet;

  // Fan-out processed mono to all channels with wet/dry mix.
  for (std::uint32_t f = 0; f < frames; ++f) {
    const float wet_sample = (f < out_samples) ? mono_out_[f] : mono_in_[f];
    for (std::uint32_t c = 0; c < channels; ++c) {
      const std::size_t idx = static_cast<std::size_t>(f) * channels + c;
      const float dry_sample = in[idx];
      out[idx] = wet * wet_sample + dry * dry_sample;
    }
  }

  return true;
}

}  // namespace studiocast::open_audio
