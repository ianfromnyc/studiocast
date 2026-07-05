#include "core/open_video/fastdvdnet_denoiser.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <sstream>

#include "core/util/fs.h"
#include "core/util/json.h"

namespace studiocast::open_video {
namespace {

int ParseSigmaFromIdFallback(const std::string &id) {
  // Best-effort extraction for curated ids like: fastdvdnet_sigma25.
  const std::string key = "sigma";
  const auto pos = id.find(key);
  if (pos == std::string::npos)
    return 25;
  int v = 0;
  bool have = false;
  for (std::size_t i = pos + key.size(); i < id.size(); ++i) {
    const char c = id[i];
    if (c < '0' || c > '9')
      break;
    have = true;
    v = v * 10 + (c - '0');
    if (v > 255)
      break;
  }
  if (!have)
    return 25;
  return std::clamp(v, 0, 255);
}

} // namespace

FastDvdnetDenoiser::FastDvdnetDenoiser() = default;
FastDvdnetDenoiser::~FastDvdnetDenoiser() = default;

int FastDvdnetDenoiser::AlignUp(int v, int align) {
  if (align <= 1)
    return v;
  const int r = v % align;
  if (r == 0)
    return v;
  return v + (align - r);
}

float FastDvdnetDenoiser::Clamp01(float x) {
  if (x < 0.0f)
    return 0.0f;
  if (x > 1.0f)
    return 1.0f;
  return x;
}

std::string
FastDvdnetDenoiser::ChoosePreferredModelId(const ModelPackRegistry &reg) {
  // Prefer the "medium" curated pack if present; otherwise fall back to any
  // installed model for the task.
  const auto &tasks = reg.Tasks();
  const auto it = tasks.find("video_denoise");
  if (it == tasks.end() || it->second.empty())
    return {};

  auto has = [&](const char *id) {
    return std::find(it->second.begin(), it->second.end(), std::string(id)) !=
           it->second.end();
  };

  if (has("fastdvdnet_sigma25"))
    return "fastdvdnet_sigma25";
  if (has("fastdvdnet_sigma15"))
    return "fastdvdnet_sigma15";
  if (has("fastdvdnet_sigma50"))
    return "fastdvdnet_sigma50";

  // Deterministic fallback: first model id for task.
  return it->second.front();
}

bool FastDvdnetDenoiser::LoadDefaultSigmaFromManifest(
    const std::filesystem::path &manifest_path, int *out_sigma) {
  if (out_sigma)
    *out_sigma = 25;
  if (manifest_path.empty())
    return false;

  const auto textOpt = util::ReadTextFile(manifest_path.string());
  if (!textOpt.has_value())
    return false;

  util::json::Value root;
  std::string err;
  if (!util::json::Parse(*textOpt, &root, &err)) {
    return false;
  }
  const auto *obj = root.AsObject();
  if (!obj)
    return false;

  auto it = obj->find("runtime_hints");
  if (it == obj->end())
    return false;
  const auto *hints = it->second.AsObject();
  if (!hints)
    return false;

  auto itS = hints->find("default_sigma");
  if (itS == hints->end())
    return false;
  const double *n = itS->second.AsNumber();
  if (!n)
    return false;
  const int v = static_cast<int>(std::lround(*n));
  if (out_sigma)
    *out_sigma = std::clamp(v, 0, 255);
  return true;
}

bool FastDvdnetDenoiser::ResolveModelFromRegistry(
    const ModelPackRegistry &reg, const std::string &requested_model_id,
    LoadedModel *out, std::string *error) {
  if (error)
    error->clear();
  if (!out) {
    if (error)
      *error = "internal error: out is null";
    return false;
  }

  const std::string chosen = requested_model_id.empty()
                                 ? ChoosePreferredModelId(reg)
                                 : requested_model_id;
  if (chosen.empty()) {
    if (error)
      *error = "Open Video FastDVDnet: no installed model packs for task "
               "'video_denoise'.";
    return false;
  }

  const auto pack = reg.Find("video_denoise", chosen);
  if (!pack.has_value()) {
    if (error) {
      *error = requested_model_id.empty()
                   ? ("Open Video FastDVDnet: selected model id not found: " +
                      chosen)
                   : ("Open Video FastDVDnet: requested model id not found: " +
                      chosen);
    }
    return false;
  }

  // Pick the ONNX file marked role=main, else first ONNX.
  std::filesystem::path onnx;
  for (const auto &f : pack->files) {
    if (f.kind == "onnx" && f.role == "main") {
      onnx = pack->root_dir / f.name;
      break;
    }
  }
  if (onnx.empty()) {
    for (const auto &f : pack->files) {
      if (f.kind == "onnx") {
        onnx = pack->root_dir / f.name;
        break;
      }
    }
  }

  if (onnx.empty()) {
    if (error)
      *error = "Open Video FastDVDnet: model pack declares no ONNX file.";
    return false;
  }

  out->id = pack->id;
  out->onnx = onnx;
  out->default_sigma = ParseSigmaFromIdFallback(pack->id);
  int hinted = out->default_sigma;
  if (LoadDefaultSigmaFromManifest(pack->manifest_path, &hinted)) {
    out->default_sigma = hinted;
  }
  return true;
}

bool FastDvdnetDenoiser::EnsureSessionForModel(const LoadedModel &model,
                                               std::string *error) {
  if (error)
    error->clear();

  if (model.onnx.empty()) {
    if (error)
      *error = "Open Video FastDVDnet: model ONNX path is empty.";
    return false;
  }

  // Avoid reloading if already active.
  if (initialized_ && !active_model_id_.empty() &&
      model.id == active_model_id_ && model.onnx == active_model_path_ &&
      ort_session_active_) {
    model_default_sigma_ = model.default_sigma;
    return true;
  }

  ort_session_cuda_.reset();
  ort_session_cpu_.reset();
  ort_session_active_ = nullptr;
  using_cpu_fallback_ = false;
  session_info_ = studiocast::onnx::OrtSessionInfo{};

  studiocast::onnx::OrtSessionOptions cuda_opts;
  cuda_opts.prefer_cuda = true;

  std::string err;
  studiocast::onnx::OrtSessionInfo info_cuda;
  auto cuda = studiocast::onnx::OrtSession::Create(model.onnx, cuda_opts,
                                                   &info_cuda, &err);
  if (!cuda) {
    if (error) {
      *error = "Open Video FastDVDnet: failed to create ORT session: " +
               (err.empty() ? std::string("unknown") : err);
    }
    return false;
  }

  std::unique_ptr<studiocast::onnx::OrtSession> cpu;
  studiocast::onnx::OrtSessionInfo info_cpu;
  if (info_cuda.using_cuda) {
    studiocast::onnx::OrtSessionOptions cpu_opts;
    cpu_opts.prefer_cuda = false;
    std::string err_cpu;
    cpu = studiocast::onnx::OrtSession::Create(model.onnx, cpu_opts, &info_cpu,
                                               &err_cpu);
    // CPU fallback is best-effort; if it fails, we can still run CUDA-only.
    if (!cpu && !err_cpu.empty()) {
      if (!info_cuda.warnings.empty()) {
        // Keep existing warnings.
      }
    }
  }

  ort_session_cuda_ = std::move(cuda);
  ort_session_cpu_ = std::move(cpu);
  ort_session_active_ = ort_session_cuda_.get();
  session_info_ = info_cuda;
  using_cpu_fallback_ = false;

  active_model_id_ = model.id;
  active_model_path_ = model.onnx;
  model_default_sigma_ = model.default_sigma;

  // Force IO re-detection.
  noisy_name_.clear();
  noise_map_name_.clear();
  denoised_name_.clear();

  ResetTemporalState();
  return true;
}

bool FastDvdnetDenoiser::RefreshGeometry(int src_w, int src_h,
                                         std::string *error) {
  if (error)
    error->clear();

  if (src_w <= 0 || src_h <= 0) {
    if (error)
      *error = "Open Video FastDVDnet: invalid frame size.";
    return false;
  }

  const int new_proc_w = AlignUp(src_w, 4);
  const int new_proc_h = AlignUp(src_h, 4);

  if (new_proc_w == proc_w_ && new_proc_h == proc_h_ && src_w == src_w_ &&
      src_h == src_h_) {
    return true;
  }

  proc_w_ = new_proc_w;
  proc_h_ = new_proc_h;
  src_w_ = src_w;
  src_h_ = src_h;

  const std::size_t plane =
      static_cast<std::size_t>(proc_w_) * static_cast<std::size_t>(proc_h_);

  noisy_shape_ = {1, 15, proc_h_, proc_w_};
  noise_map_shape_ = {1, 1, proc_h_, proc_w_};
  denoised_shape_ = {1, 3, proc_h_, proc_w_};

  noisy_tensor_.assign(15 * plane, 0.0f);
  noise_map_tensor_.assign(plane, 0.0f);
  denoised_tensor_.assign(3 * plane, 0.0f);
  last_noise_map_value_ = -1.0f;

  history_.clear();
  history_.resize(kHistoryFrames);
  for (auto &f : history_) {
    f.assign(3 * plane, 0.0f);
  }
  history_filled_ = 0;
  history_write_idx_ = 0;
  have_last_sequence_ = false;
  last_capture_sequence_ = 0;

  // Pre-build ORT bindings.
  ort_inputs_.clear();
  ort_outputs_.clear();

  if (!DetectIoNames(error)) {
    return false;
  }

  studiocast::onnx::OrtSession::RunInput in_noisy;
  in_noisy.name = noisy_name_.c_str();
  in_noisy.data = noisy_tensor_.data();
  in_noisy.num_floats = noisy_tensor_.size();
  in_noisy.shape = noisy_shape_.data();
  in_noisy.shape_rank = noisy_shape_.size();

  studiocast::onnx::OrtSession::RunInput in_noise;
  in_noise.name = noise_map_name_.c_str();
  in_noise.data = noise_map_tensor_.data();
  in_noise.num_floats = noise_map_tensor_.size();
  in_noise.shape = noise_map_shape_.data();
  in_noise.shape_rank = noise_map_shape_.size();

  ort_inputs_.push_back(in_noisy);
  ort_inputs_.push_back(in_noise);

  studiocast::onnx::OrtSession::RunOutput out_den;
  out_den.name = denoised_name_.c_str();
  out_den.data = denoised_tensor_.data();
  out_den.num_floats = denoised_tensor_.size();
  out_den.shape = denoised_shape_.data();
  out_den.shape_rank = denoised_shape_.size();
  ort_outputs_.push_back(out_den);

  return true;
}

bool FastDvdnetDenoiser::DetectIoNames(std::string *error) {
  if (error)
    error->clear();
  if (!ort_session_active_) {
    if (error)
      *error = "Open Video FastDVDnet: ORT session not initialized.";
    return false;
  }

  const auto &info = session_info_;
  if (info.input_names.size() < 2 || info.output_names.empty()) {
    if (error)
      *error = "Open Video FastDVDnet: unexpected ONNX IO count (need >=2 "
               "inputs, >=1 output).";
    return false;
  }

  // Heuristic: find input with channel dim == 15 and one with channel dim == 1.
  int noisy_idx = -1;
  int noise_idx = -1;
  for (std::size_t i = 0; i < info.input_shapes.size(); ++i) {
    const auto &s = info.input_shapes[i];
    if (s.size() == 4) {
      if (s[1] == 15)
        noisy_idx = static_cast<int>(i);
      if (s[1] == 1)
        noise_idx = static_cast<int>(i);
    }
  }
  if (noisy_idx < 0 || noise_idx < 0 || noisy_idx == noise_idx) {
    // Fallback: match by name.
    for (std::size_t i = 0; i < info.input_names.size(); ++i) {
      const auto &n = info.input_names[i];
      if (noisy_idx < 0 &&
          (n == "noisy" || n.find("noisy") != std::string::npos))
        noisy_idx = static_cast<int>(i);
      if (noise_idx < 0 &&
          (n == "noise_map" || n.find("noise") != std::string::npos))
        noise_idx = static_cast<int>(i);
    }
  }
  if (noisy_idx < 0)
    noisy_idx = 0;
  if (noise_idx < 0)
    noise_idx = (noisy_idx == 0 ? 1 : 0);

  int out_idx = -1;
  for (std::size_t i = 0; i < info.output_shapes.size(); ++i) {
    const auto &s = info.output_shapes[i];
    if (s.size() == 4 && s[1] == 3) {
      out_idx = static_cast<int>(i);
      break;
    }
  }
  if (out_idx < 0)
    out_idx = 0;

  noisy_name_ = info.input_names[static_cast<std::size_t>(noisy_idx)];
  noise_map_name_ = info.input_names[static_cast<std::size_t>(noise_idx)];
  denoised_name_ = info.output_names[static_cast<std::size_t>(out_idx)];

  if (noisy_name_.empty() || noise_map_name_.empty() ||
      denoised_name_.empty()) {
    if (error)
      *error = "Open Video FastDVDnet: failed to resolve IO tensor names.";
    return false;
  }
  return true;
}

bool FastDvdnetDenoiser::EnsureInitialized(
    int src_w, int src_h, const std::string &requested_model_id,
    std::string *error) {
  if (error)
    error->clear();

  if (disabled_) {
    if (error && !sticky_warning_.empty())
      *error = sticky_warning_;
    return false;
  }

  if (initialized_ && ort_session_active_ &&
      requested_model_id == active_requested_model_id_) {
    return RefreshGeometry(src_w, src_h, error);
  }

  // Scan installed packs only when the requested model configuration changes.
  registry_ = ModelPackRegistry::ScanDefault();
  LoadedModel model;
  std::string resolve_err;
  if (!ResolveModelFromRegistry(registry_, requested_model_id, &model,
                                &resolve_err)) {
    if (error)
      *error = resolve_err;
    return false;
  }

  std::string sess_err;
  if (!EnsureSessionForModel(model, &sess_err)) {
    if (error)
      *error = sess_err;
    return false;
  }

  std::string geo_err;
  if (!RefreshGeometry(src_w, src_h, &geo_err)) {
    if (error)
      *error = geo_err;
    return false;
  }

  initialized_ = true;
  active_requested_model_id_ = requested_model_id;
  return true;
}

void FastDvdnetDenoiser::ResetTemporalState() {
  history_filled_ = 0;
  history_write_idx_ = 0;
  have_last_sequence_ = false;
  last_capture_sequence_ = 0;
}

void FastDvdnetDenoiser::PreprocessRgbToChwPadded(
    const std::uint8_t *rgb, int width, int height, std::size_t stride,
    std::vector<float> *out_chw) const {
  if (!out_chw)
    return;
  const std::size_t plane =
      static_cast<std::size_t>(proc_w_) * static_cast<std::size_t>(proc_h_);
  if (out_chw->size() != 3 * plane) {
    out_chw->assign(3 * plane, 0.0f);
  }

  float *out_r = out_chw->data() + 0 * plane;
  float *out_g = out_chw->data() + 1 * plane;
  float *out_b = out_chw->data() + 2 * plane;

  // Convert + pad by edge replication.
  for (int y = 0; y < proc_h_; ++y) {
    const int sy = std::clamp(y, 0, height - 1);
    const std::uint8_t *row = rgb + static_cast<std::size_t>(sy) * stride;
    for (int x = 0; x < proc_w_; ++x) {
      const int sx = std::clamp(x, 0, width - 1);
      const std::uint8_t *px = row + sx * 3;
      const std::size_t idx =
          static_cast<std::size_t>(y) * static_cast<std::size_t>(proc_w_) +
          static_cast<std::size_t>(x);
      out_r[idx] = static_cast<float>(px[0]) * (1.0f / 255.0f);
      out_g[idx] = static_cast<float>(px[1]) * (1.0f / 255.0f);
      out_b[idx] = static_cast<float>(px[2]) * (1.0f / 255.0f);
    }
  }
}

void FastDvdnetDenoiser::BuildNoisyTensorFromHistory(
    std::vector<float> *out_noisy) const {
  if (!out_noisy)
    return;
  const std::size_t plane =
      static_cast<std::size_t>(proc_w_) * static_cast<std::size_t>(proc_h_);
  if (out_noisy->size() != 15 * plane) {
    out_noisy->assign(15 * plane, 0.0f);
  }

  // Determine indices for t-2, t-1, t.
  auto idx_t = [&](int back) -> int {
    // back=0 -> newest, back=1 -> t-1, back=2 -> t-2.
    if (history_filled_ <= 0)
      return 0;
    const int newest =
        (history_write_idx_ - 1 + kHistoryFrames) % kHistoryFrames;
    if (back == 0)
      return newest;
    if (history_filled_ < back + 1) {
      // Not enough history; replicate oldest available.
      const int oldest =
          (history_write_idx_ - history_filled_ + kHistoryFrames * 8) %
          kHistoryFrames;
      return oldest;
    }
    return (newest - back + kHistoryFrames) % kHistoryFrames;
  };

  const int i_t2 = idx_t(2);
  const int i_t1 = idx_t(1);
  const int i_t0 = idx_t(0);

  const std::vector<float> *f_t2 = &history_[static_cast<std::size_t>(i_t2)];
  const std::vector<float> *f_t1 = &history_[static_cast<std::size_t>(i_t1)];
  const std::vector<float> *f_t0 = &history_[static_cast<std::size_t>(i_t0)];

  // Future frames replicated as current.
  const std::vector<float> *f_tp1 = f_t0;
  const std::vector<float> *f_tp2 = f_t0;

  const std::vector<const std::vector<float> *> frames = {f_t2, f_t1, f_t0,
                                                          f_tp1, f_tp2};

  for (std::size_t fi = 0; fi < frames.size(); ++fi) {
    const auto *f = frames[fi];
    if (!f || f->size() < 3 * plane)
      continue;

    const float *in_r = f->data() + 0 * plane;
    const float *in_g = f->data() + 1 * plane;
    const float *in_b = f->data() + 2 * plane;

    float *out_r = out_noisy->data() + (fi * 3 + 0) * plane;
    float *out_g = out_noisy->data() + (fi * 3 + 1) * plane;
    float *out_b = out_noisy->data() + (fi * 3 + 2) * plane;
    std::memcpy(out_r, in_r, plane * sizeof(float));
    std::memcpy(out_g, in_g, plane * sizeof(float));
    std::memcpy(out_b, in_b, plane * sizeof(float));
  }
}

void FastDvdnetDenoiser::EnsureNoiseMap(float sigma_over_255) {
  const float v = std::clamp(sigma_over_255, 0.0f, 1.0f);
  if (std::abs(v - last_noise_map_value_) < 1e-6f && !noise_map_tensor_.empty())
    return;
  last_noise_map_value_ = v;
  std::fill(noise_map_tensor_.begin(), noise_map_tensor_.end(), v);
}

void FastDvdnetDenoiser::PostprocessToRgbInPlace(std::uint8_t *rgb, int width,
                                                 int height,
                                                 std::size_t stride) const {
  if (!rgb)
    return;
  if (denoised_tensor_.empty())
    return;

  const std::size_t plane =
      static_cast<std::size_t>(proc_w_) * static_cast<std::size_t>(proc_h_);
  if (denoised_tensor_.size() < 3 * plane)
    return;

  const float *in_r = denoised_tensor_.data() + 0 * plane;
  const float *in_g = denoised_tensor_.data() + 1 * plane;
  const float *in_b = denoised_tensor_.data() + 2 * plane;

  for (int y = 0; y < height; ++y) {
    std::uint8_t *row = rgb + static_cast<std::size_t>(y) * stride;
    const std::size_t base =
        static_cast<std::size_t>(y) * static_cast<std::size_t>(proc_w_);
    for (int x = 0; x < width; ++x) {
      const std::size_t idx = base + static_cast<std::size_t>(x);
      const float r = std::clamp(in_r[idx], 0.0f, 1.0f);
      const float g = std::clamp(in_g[idx], 0.0f, 1.0f);
      const float b = std::clamp(in_b[idx], 0.0f, 1.0f);

      const int ir = static_cast<int>(r * 255.0f + 0.5f);
      const int ig = static_cast<int>(g * 255.0f + 0.5f);
      const int ib = static_cast<int>(b * 255.0f + 0.5f);

      std::uint8_t *px = row + x * 3;
      px[0] = static_cast<std::uint8_t>(std::clamp(ir, 0, 255));
      px[1] = static_cast<std::uint8_t>(std::clamp(ig, 0, 255));
      px[2] = static_cast<std::uint8_t>(std::clamp(ib, 0, 255));
    }
  }
}

void FastDvdnetDenoiser::DisableAfterFailure(const std::string &why) {
  disabled_ = true;
  sticky_warning_ = why;
}

bool FastDvdnetDenoiser::ApplyRgbInPlace(std::uint64_t capture_sequence,
                                         std::uint8_t *rgb, int width,
                                         int height, std::size_t stride,
                                         int strength,
                                         const std::string &requested_model_id,
                                         std::string *error) {
  if (error)
    error->clear();

  if (disabled_) {
    if (error && !sticky_warning_.empty())
      *error = sticky_warning_;
    return false;
  }
  if (!rgb) {
    if (error)
      *error = "Open Video FastDVDnet: null RGB buffer.";
    return false;
  }
  const int s = std::clamp(strength, 0, 100);
  if (s <= 0) {
    ResetTemporalState();
    return true;
  }

  std::string init_err;
  if (!EnsureInitialized(width, height, requested_model_id, &init_err)) {
    if (error)
      *error = init_err;
    return false;
  }
  if (!ort_session_active_) {
    if (error)
      *error = "Open Video FastDVDnet: ORT session missing.";
    return false;
  }

  // If capture sequence jumps (drop/restart), reset temporal history to avoid
  // blending stale frames.
  if (have_last_sequence_) {
    if (capture_sequence != last_capture_sequence_ + 1) {
      ResetTemporalState();
    }
  }
  have_last_sequence_ = true;
  last_capture_sequence_ = capture_sequence;

  // Preprocess current frame into history buffer (CHW float + padded).
  if (history_.empty()) {
    // Shouldn't happen (RefreshGeometry allocates), but be defensive.
    const std::size_t plane =
        static_cast<std::size_t>(proc_w_) * static_cast<std::size_t>(proc_h_);
    history_.resize(kHistoryFrames);
    for (auto &f : history_)
      f.assign(3 * plane, 0.0f);
  }

  PreprocessRgbToChwPadded(
      rgb, width, height, stride,
      &history_[static_cast<std::size_t>(history_write_idx_)]);
  history_write_idx_ = (history_write_idx_ + 1) % kHistoryFrames;
  history_filled_ = std::min(history_filled_ + 1, kHistoryFrames);

  // Assemble ORT inputs.
  BuildNoisyTensorFromHistory(&noisy_tensor_);

  // Strength -> sigma in [0..55], then normalize to [0..1] via /255.
  const float t = Clamp01(static_cast<float>(s) / 100.0f);
  const float sigma = std::clamp(55.0f * t, 0.0f, 55.0f);
  const float sigma_over_255 = sigma * (1.0f / 255.0f);
  EnsureNoiseMap(sigma_over_255);

  // Refresh binding pointers (in case vectors reallocated).
  if (ort_inputs_.size() != 2 || ort_outputs_.size() != 1) {
    // Rebuild.
    std::string geo_err;
    if (!RefreshGeometry(width, height, &geo_err)) {
      if (error)
        *error = geo_err;
      return false;
    }
  }

  ort_inputs_[0].data = noisy_tensor_.data();
  ort_inputs_[0].num_floats = noisy_tensor_.size();
  ort_inputs_[1].data = noise_map_tensor_.data();
  ort_inputs_[1].num_floats = noise_map_tensor_.size();
  ort_outputs_[0].data = denoised_tensor_.data();
  ort_outputs_[0].num_floats = denoised_tensor_.size();

  std::string ort_err;
  if (!ort_session_active_->RunCpu(ort_inputs_.data(), ort_inputs_.size(),
                                   ort_outputs_.data(), ort_outputs_.size(),
                                   &ort_err)) {
    std::ostringstream oss;
    oss << "Open Video FastDVDnet ORT run failed: "
        << (ort_err.empty() ? "unknown" : ort_err);

    // If CUDA is active and CPU fallback exists, switch once.
    if (!using_cpu_fallback_ && ort_session_cuda_ && ort_session_cpu_ &&
        session_info_.using_cuda) {
      ort_session_active_ = ort_session_cpu_.get();
      using_cpu_fallback_ = true;
      sticky_warning_ = "Open Video FastDVDnet: switched to CPU fallback after "
                        "a CUDA runtime failure.";
      runtime_failures_ = 0;
      // Try once on CPU immediately.
      std::string cpu_err;
      if (!ort_session_active_->RunCpu(ort_inputs_.data(), ort_inputs_.size(),
                                       ort_outputs_.data(), ort_outputs_.size(),
                                       &cpu_err)) {
        oss << " (CPU fallback also failed: "
            << (cpu_err.empty() ? "unknown" : cpu_err) << ")";
      } else {
        // CPU fallback succeeded.
        PostprocessToRgbInPlace(rgb, width, height, stride);
        return true;
      }
    }

    runtime_failures_++;
    if (runtime_failures_ >= 3) {
      DisableAfterFailure(
          "Open Video FastDVDnet: disabled after repeated runtime failures.");
    }

    if (error)
      *error = oss.str();
    return false;
  }

  runtime_failures_ = 0;

  // Convert output back into the RGB buffer.
  PostprocessToRgbInPlace(rgb, width, height, stride);
  return true;
}

} // namespace studiocast::open_video
