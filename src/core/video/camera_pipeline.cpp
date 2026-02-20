#include "camera_pipeline.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <memory>
#include <set>
#include <sstream>
#include <unordered_map>
#include <vector>

#include "core/cuda/cuda_image.h"
#include "core/cuda/cuda_tensor.h"
#include "core/cuda/kernels/open_cuda_vb_kernels.h"
#include "core/cuda/kernels/resize_bilinear.h"
#include "core/maxine/availability.h"
#include "core/maxine/maxine_manager.h"
#include "core/maxine/effects/ar_eye_contact_effect.h"
#include "core/maxine/effects/ar_auto_frame_tracker.h"
#include "core/maxine/effects/vfx_background_blur_effect.h"
#include "core/maxine/effects/vfx_green_screen_effect.h"
#include "core/maxine/effects/vfx_relighting_effect.h"
#include "core/maxine/effects/vfx_denoise_effect.h"
#include "core/maxine/cuda_crop_scale.h"
#include "core/maxine/cuda_driver_api.h"
#include "core/maxine/cuda_vignette.h"
#include "core/maxine/nvcv_api.h"
#include "core/maxine/vfx_api.h"
#include "core/open_video/model_pack_registry.h"
#include "../open_video/matting_session.h"
#include "core/open_video/frame_analysis_cache.h"
#include "core/open_video/fastdvdnet_denoiser.h"
#include "core/open_video/gaze_correction_eye_contact.h"
#include "core/open_video/yunet_face_detector.h"
#include "core/video/convert.h"
#include "core/video/capture_error_policy.h"
#include "core/video/image_ppm.h"
#include "core/video/mjpeg_decode.h"
#include "core/video/scaling_policy.h"
#include "core/video/effects/effect_chain.h"
#include "core/video/effects/broadcast_effect_contract.h"
#include "core/video/effects/broadcast_effect_open_cuda_gate.h"
#include "core/video/effects/broadcast_effect_rules.h"
#include "core/video/v4l2loopback.h"

namespace studiocast::video {
namespace {

std::string ChooseDefaultOutputLoopback(std::string* error) {
  const auto rep = ProbeLoopback();
  for (const auto& d : rep.devices) {
    if (d.is_loopback && d.can_write) return d.dev_node;
  }
  if (error) {
    std::ostringstream oss;
    oss << "No writable v4l2loopback device found.\n"
        << "Run the suggested command from studiocast-video status, e.g.:\n"
        << "  " << rep.suggested_modprobe_cmd << "\n";
    *error = oss.str();
  }
  return {};
}

std::string ToLowerAscii(std::string s) {
  for (char& c : s) {
    const unsigned char uc = static_cast<unsigned char>(c);
    c = static_cast<char>(std::tolower(uc));
  }
  return s;
}

bool ParseRgbHex(const std::string& s, std::uint32_t* out_rgb) {
  if (!out_rgb) return false;
  *out_rgb = 0;

  std::string t = s;
  if (t.rfind("0x", 0) == 0 || t.rfind("0X", 0) == 0) {
    t = t.substr(2);
  }
  if (!t.empty() && t.front() == '#') {
    t = t.substr(1);
  }
  if (t.size() != 6) return false;

  std::uint32_t v = 0;
  for (const char ch : t) {
    v <<= 4u;
    if (ch >= '0' && ch <= '9') {
      v |= static_cast<std::uint32_t>(ch - '0');
    } else if (ch >= 'a' && ch <= 'f') {
      v |= static_cast<std::uint32_t>(ch - 'a' + 10);
    } else if (ch >= 'A' && ch <= 'F') {
      v |= static_cast<std::uint32_t>(ch - 'A' + 10);
    } else {
      return false;
    }
  }
  *out_rgb = (v & 0xFFFFFFu);
  return true;
}

float Clamp01FromPercent(int percent) {
  const int p = std::max(0, std::min(100, percent));
  return static_cast<float>(p) / 100.0f;
}

int ParseVideoIndex(const std::string& sys_name) {
  // sys_name is usually "videoN".
  if (sys_name.rfind("video", 0) != 0) return -1;
  const std::string rest = sys_name.substr(5);
  if (rest.empty()) return -1;
  int v = 0;
  for (const char ch : rest) {
    if (ch < '0' || ch > '9') return -1;
    v = v * 10 + (ch - '0');
    if (v > 4096) return -1;
  }
  return v;
}

bool CheckedPositiveDims(int width, int height, unsigned* out_w, unsigned* out_h, std::string* error, const char* what) {
  if (!out_w || !out_h) return false;

  if (width <= 0 || height <= 0) {
    if (error) {
      std::ostringstream oss;
      oss << (what ? what : "") << " invalid frame size: " << width << "x" << height << ".";
      *error = oss.str();
    }
    return false;
  }

  *out_w = static_cast<unsigned>(width);
  *out_h = static_cast<unsigned>(height);
  return true;
}

[[maybe_unused]] static bool SyncAfterGpuToCpuTransfer(studiocast::maxine::CudaDriverApi& cuda,
                                     studiocast::maxine::CUstream stream,
                                     const char* context,
                                     std::string* error) {
  std::string sync_err;
  if (!cuda.StreamSynchronize(stream, &sync_err)) {
    if (error) {
      const char* ctx = context ? context : "(unknown context)";
      *error = std::string(ctx) + ": cuda StreamSynchronize failed: " + sync_err;
    }
    return false;
  }
  return true;
}

int ScoreCamera(const VideoDevice& d) {
  // Heuristic score for "auto" camera selection.
  // Prefer typical UVC webcams, avoid IR/depth/metadata nodes when possible.
  int score = 0;
  if (d.driver == "uvcvideo") score += 10;

  const auto name = ToLowerAscii(d.name);
  if (name.find("ir") != std::string::npos) score -= 50;
  if (name.find("depth") != std::string::npos) score -= 50;
  if (name.find("metadata") != std::string::npos) score -= 50;

  if (name.find("hd") != std::string::npos) score += 5;
  if (name.find("camera") != std::string::npos) score += 2;

  return score;
}

std::vector<VideoDevice> ListCandidateCameras() {
  const auto rep = ProbeLoopback();
  std::vector<VideoDevice> cams;
  cams.reserve(rep.devices.size());
  for (const auto& d : rep.devices) {
    if (!d.is_loopback && d.can_read) cams.push_back(d);
  }

  std::sort(cams.begin(), cams.end(), [](const VideoDevice& a, const VideoDevice& b) {
    const int sa = ScoreCamera(a);
    const int sb = ScoreCamera(b);
    if (sa != sb) return sa > sb;

    const int ia = ParseVideoIndex(a.sys_name);
    const int ib = ParseVideoIndex(b.sys_name);
    if (ia != ib) {
      // Prefer lower indices (video0, video1, ...)
      if (ia < 0) return false;
      if (ib < 0) return true;
      return ia < ib;
    }

    return a.dev_node < b.dev_node;
  });

  return cams;
}

}  // namespace

CameraPipeline::~CameraPipeline() { Stop(); }

void CameraPipeline::SetMirrorEnabled(bool enabled) {
  std::lock_guard<std::mutex> lock(effects_mu_);
  effects_.mirror = enabled;
}

void CameraPipeline::SetEffects(const studiocast::video::effects::BroadcastCameraEffects& effects) {
  std::lock_guard<std::mutex> lock(effects_mu_);
  effects_ = effects;
}

CameraPipelineStatus CameraPipeline::Status() const {
  std::lock_guard<std::mutex> lock(mu_);
  CameraPipelineStatus s;
  s.running = running_;
  s.starting = starting_;
  s.input_device = input_device_;
  s.output_device = output_device_;
  if (running_ || starting_) {
    s.capture = capture_;
    s.output = output_;
    s.scaling_backend_active = scaling_backend_active_;
    s.scaling_from = scaling_from_;
    s.scaling_to = scaling_to_;
    s.ms_per_frame = ms_per_frame_;
    s.fps_actual = fps_actual_;
    s.perf_sample_frames = perf_sample_frames_;
    s.debug = debug_;
    s.open_cuda_transfers = open_cuda_transfers_;
  } else {
    // Avoid exposing stale negotiated formats when the pipeline is idle.
    //
    // Exception: if the loopback writer is held open (v4l2loopback/exclusive_caps),
    // expose the negotiated output format so consumers (including the GUI preview)
    // can open the device without attempting to renegotiate caps.
    s.capture = CaptureFormat{};
    s.output = writer_.IsOpen() ? writer_.Actual() : ActualFormat{};
    s.scaling_backend_active.clear();
    s.scaling_from = CaptureFormat{};
    s.scaling_to = ActualFormat{};
    s.ms_per_frame = CameraPipelineStatus::MsPerFrame{};
    s.fps_actual = 0.0;
    s.perf_sample_frames = 0;
    s.debug = CameraPipelineStatus::Debug{};
    s.open_cuda_transfers = CameraPipelineStatus::OpenCudaTransfers{};
  }
  s.frame_index = frame_index_;
  s.effects_backends = effects_backends_;
  s.effects_note = effects_note_;
  s.last_error = last_error_;
  return s;
}

bool CameraPipeline::Start(const CameraPipelineConfig& cfg, std::string* error) {
  const bool w_set = cfg.width > 0;
  const bool h_set = cfg.height > 0;
  if (cfg.capture_mode == CaptureMode::requested) {
    if (!w_set || !h_set) {
      if (error) *error = "Invalid width/height.";
      return false;
    }
  } else {
    // In auto capture mode, width/height are optional (sentinel <= 0). If one is set, both must be set.
    if (w_set != h_set) {
      if (error) *error = "Invalid width/height (must both be >0 or both be <=0 in auto capture mode).";
      return false;
    }
  }
  if (cfg.fps <= 0 || cfg.fps > 240) {
    if (error) *error = "Invalid fps (1..240).";
    return false;
  }

  std::unique_lock<std::mutex> lock(mu_);
  if (running_ || starting_) {
    if (error) *error = "Camera pipeline already running.";
    return false;
  }

  if (th_.joinable()) {
    lock.unlock();
    Stop();
    lock.lock();
  }

  stop_.store(false);

  {
    std::lock_guard<std::mutex> fxLock(effects_mu_);
    effects_ = cfg.effects;
  }

  last_error_.clear();
  input_device_.clear();
  output_device_.clear();
  capture_ = CaptureFormat{};
  output_ = ActualFormat{};
  scaling_backend_active_.clear();
  scaling_from_ = CaptureFormat{};
  scaling_to_ = ActualFormat{};
  frame_index_ = 0;
  ms_per_frame_ = CameraPipelineStatus::MsPerFrame{};
  fps_actual_ = 0.0;
  perf_sample_frames_ = 0;
  debug_ = CameraPipelineStatus::Debug{};
  open_cuda_transfers_ = CameraPipelineStatus::OpenCudaTransfers{};

  effects_backends_.clear();
  effects_note_.clear();

  starting_ = true;
  running_ = false;
  start_notified_ = false;

  th_ = std::thread(&CameraPipeline::ThreadMain, this, cfg);

  cv_.wait(lock, [&]() { return start_notified_; });
  starting_ = false;

  if (!running_) {
    const std::string err = last_error_.empty() ? "Failed to start camera pipeline." : last_error_;
    lock.unlock();
    if (th_.joinable()) th_.join();
    if (error) *error = err;
    return false;
  }

  return true;
}

bool CameraPipeline::OpenOutputLocked(const std::string& outDev,
                                     int width,
                                     int height,
                                     int fps,
                                     bool strict_fps,
                                     bool* out_opened_or_renegotiated,
                                     std::string* error) {
  if (out_opened_or_renegotiated) *out_opened_or_renegotiated = false;

  // Try to reuse an existing open writer when possible.
  if (writer_.IsOpen() && writer_device_ == outDev) {
    // Refresh cached negotiated format.
    //
    // v4l2loopback allows capture clients to renegotiate global caps via VIDIOC_S_FMT.
    // If that happens, size_image/bytes_per_line can change under an already-open
    // writer fd. Refreshing avoids write() failures and start/stop thrashing.
    // NOTE: RefreshActual() is best-effort. Some v4l2loopback configurations
    // can temporarily reject VIDIOC_G_FMT (e.g. when consumers disconnect).
    // Do not tear down a working writer fd just because the refresh query fails.
    std::string refresh_err;
    (void)writer_.RefreshActual(&refresh_err);

    const auto& a = writer_.Actual();

    // When the pipeline is idle (not running / starting), consumers may alter
    // stream parameters (notably FPS) on some v4l2loopback configurations.
    // Treat FPS mismatch as non-fatal in the idle keep-alive path.
    if (a.width == width && a.height == height && (!strict_fps || a.fps == fps)) {
      output_ = a;
      output_device_ = outDev;
      return true;
    }

    // If format/dimensions changed, renegotiate.
    writer_.Close();
    writer_device_.clear();
  } else if (writer_.IsOpen() && writer_device_ != outDev) {
    writer_.Close();
    writer_device_.clear();
  }

  // Open writer: prefer RGB24 output (avoids RGB->YUYV conversion), fallback to YUYV.
  if (out_opened_or_renegotiated) *out_opened_or_renegotiated = true;

  std::string werr;
  if (!writer_.Open(outDev, width, height, fps, PixelFormat::rgb24, &werr)) {
    std::string werr2;
    if (!writer_.Open(outDev, width, height, fps, PixelFormat::yuyv, &werr2)) {
      if (error) {
        *error = "Failed to open v4l2loopback output " + outDev + ":\n" +
                 "Tried rgb24:\n" + werr + "\n\n" +
                 "Tried yuyv:\n" + werr2;
      }
      return false;
    }
  }

  writer_device_ = outDev;
  output_ = writer_.Actual();
  output_device_ = outDev;
  return true;
}

bool CameraPipeline::EnsureOutputOpen(const CameraPipelineConfig& cfg, std::string* error) {
  const bool w_set = cfg.width > 0;
  const bool h_set = cfg.height > 0;
  if (cfg.capture_mode == CaptureMode::requested) {
    if (!w_set || !h_set) {
      if (error) *error = "Invalid width/height.";
      return false;
    }
  } else {
    // In auto capture mode, width/height are optional (sentinel <= 0). If one is set, both must be set.
    if (w_set != h_set) {
      if (error) *error = "Invalid width/height (must both be >0 or both be <=0 in auto capture mode).";
      return false;
    }
  }
  if (cfg.fps <= 0 || cfg.fps > 240) {
    if (error) *error = "Invalid fps (1..240).";
    return false;
  }

  std::unique_lock<std::mutex> lock(mu_);
  if (running_ || starting_) {
    // Output is already open (or will be shortly) as part of Start().
    return true;
  }

  std::string outDev = cfg.output_device;
  if (outDev.empty()) {
    std::string e;
    outDev = ChooseDefaultOutputLoopback(&e);
    if (outDev.empty()) {
      if (error) *error = e.empty() ? "No output loopback found." : e;
      return false;
    }
  }

  // If capture is auto and there is no explicit output override, we can't know what output
  // dimensions to choose until capture is negotiated in ThreadMain(). Treat this as best-effort:
  // keep any already-open writer, otherwise defer without error.
  if (cfg.capture_mode == CaptureMode::auto_best && (!w_set || !h_set)) {
    if (writer_.IsOpen() && writer_device_ == outDev) {
      std::string refresh_err;
      (void)writer_.RefreshActual(&refresh_err);
      output_ = writer_.Actual();
      output_device_ = outDev;
    }
    last_error_.clear();
    return true;
  }

  bool opened_or_renegotiated = false;
  std::string oerr;
  if (!OpenOutputLocked(outDev, cfg.width, cfg.height, cfg.fps, false, &opened_or_renegotiated, &oerr)) {
    if (error) *error = oerr;
    return false;
  }

  last_error_.clear();

  // Seed one frame into the loopback so applications can successfully open it
  // as a capture device even before the heavy camera pipeline starts.
  //
  // This is particularly important with v4l2loopback 'exclusive_caps=1':
  // the device is typically only considered a "real" capture source once a producer
  // has set a format and provided at least one frame.
  // Only seed a frame when we actually opened/renegotiated the output. If we reuse an already-open
  // writer, re-seeding would overwrite the live stream and manifest as periodic black flashes.
  if (opened_or_renegotiated && writer_.IsOpen()) {
    const ActualFormat outA = output_;
    std::size_t size = outA.size_image;
    if (size == 0 && outA.bytes_per_line > 0 && outA.height > 0) {
      size = outA.bytes_per_line * static_cast<std::size_t>(outA.height);
    }

    if (size > 0) {
      std::vector<std::uint8_t> buf(size);

      if (outA.format == PixelFormat::rgb24) {
        std::fill(buf.begin(), buf.end(), 0);
      } else {
        // YUYV "black": Y=16, U=128, V=128 (limited range neutral chroma)
        for (std::size_t i = 0; i + 3 < buf.size(); i += 4) {
          buf[i + 0] = 16;
          buf[i + 1] = 128;
          buf[i + 2] = 16;
          buf[i + 3] = 128;
        }
      }

      std::string werr;
      if (!writer_.WriteFrame(buf.data(), buf.size(), &werr)) {
        if (error) *error = "Output opened, but failed to write initial frame: " + werr;
        return false;
      }
    }
  }

  return true;
}

void CameraPipeline::CloseOutput() {
  std::lock_guard<std::mutex> lock(mu_);
  if (running_ || starting_) return;
  writer_.Close();
  writer_device_.clear();
  output_device_.clear();
  output_ = ActualFormat{};
}

void CameraPipeline::Stop() {
  stop_.store(true);

  std::thread toJoin;
  {
    std::lock_guard<std::mutex> lock(mu_);
    if (th_.joinable()) {
      toJoin = std::move(th_);
    } else {
      running_ = false;
      starting_ = false;
      capture_ = CaptureFormat{};
      output_ = ActualFormat{};
      scaling_backend_active_.clear();
      scaling_from_ = CaptureFormat{};
      scaling_to_ = ActualFormat{};
      return;
    }
  }

  toJoin.join();

  std::lock_guard<std::mutex> lock(mu_);
  running_ = false;
  starting_ = false;
  capture_ = CaptureFormat{};
  output_ = ActualFormat{};
  scaling_backend_active_.clear();
  scaling_from_ = CaptureFormat{};
  scaling_to_ = ActualFormat{};
}

void CameraPipeline::ThreadMain(CameraPipelineConfig cfg) {
  // Open input capture.
  V4l2Capture cap;
  std::string inDev = cfg.input_device;
  std::ostringstream inAttempts;

  const bool auto_best = (cfg.capture_mode == CaptureMode::auto_best);

  if (!inDev.empty()) {
    std::string cerr;
    const bool opened = auto_best
                            ? cap.OpenBest(inDev, cfg.fps, cfg.prefer_mjpeg, &cerr)
                            : cap.Open(inDev,
                                       cfg.width,
                                       cfg.height,
                                       cfg.fps,
                                       CapturePixelFormat::yuyv,
                                       cfg.prefer_mjpeg,
                                       &cerr);

    if (!opened) {
      std::lock_guard<std::mutex> lock(mu_);
      last_error_ = "Failed to open capture device " + inDev + ":\n" + cerr;
      running_ = false;
      start_notified_ = true;
      cv_.notify_all();
      return;
    }
  } else {
    const auto candidates = ListCandidateCameras();
    if (candidates.empty()) {
      std::lock_guard<std::mutex> lock(mu_);
      last_error_ = "No readable camera device found (no non-loopback /dev/video* with read access).";
      running_ = false;
      start_notified_ = true;
      cv_.notify_all();
      return;
    }

    bool opened = false;
    for (const auto& d : candidates) {
      std::string cerr;
      const bool thisOpened = auto_best
                                  ? cap.OpenBest(d.dev_node, cfg.fps, cfg.prefer_mjpeg, &cerr)
                                  : cap.Open(d.dev_node,
                                             cfg.width,
                                             cfg.height,
                                             cfg.fps,
                                             CapturePixelFormat::yuyv,
                                             cfg.prefer_mjpeg,
                                             &cerr);

      if (thisOpened) {
        inDev = d.dev_node;
        opened = true;
        break;
      }

      inAttempts << "  - " << d.dev_node;
      if (!d.name.empty()) inAttempts << " (" << d.name << ")";
      inAttempts << ":\n";
      inAttempts << "    " << cerr << "\n";
    }

    if (!opened) {
      std::ostringstream oss;
      oss << "Failed to auto-select a usable camera.\n";
      oss << "Tried the following devices:\n";
      oss << inAttempts.str();
      oss << "\nTip: specify an input explicitly (e.g. studiocastd --input /dev/video0)\n";
      oss << "     or try a smaller resolution like 640x480 (many laptop webcams only expose YUYV at lower resolutions).\n";

      std::lock_guard<std::mutex> lock(mu_);
      last_error_ = oss.str();
      running_ = false;
      start_notified_ = true;
      cv_.notify_all();
      return;
    }
  }

  std::string outDev = cfg.output_device;
  if (outDev.empty()) {
    std::string e;
    outDev = ChooseDefaultOutputLoopback(&e);
    if (outDev.empty()) {
      std::lock_guard<std::mutex> lock(mu_);
      last_error_ = e.empty() ? "No output loopback found." : e;
      running_ = false;
      start_notified_ = true;
      cv_.notify_all();
      return;
    }
  }

  const auto capA = cap.Actual();

  // Open (or reuse) writer to v4l2loopback output.
  {
    std::lock_guard<std::mutex> lock(mu_);
    bool opened_or_renegotiated = false;
    std::string oerr;
    int out_w = cfg.width;
    int out_h = cfg.height;
    if (cfg.capture_mode == CaptureMode::auto_best && (out_w <= 0 || out_h <= 0)) {
      // In auto capture mode (without an explicit output override), align output
      // to the negotiated capture size.
      out_w = capA.width;
      out_h = capA.height;
    }

    // Keep output size aligned with an explicitly configured width/height.
    // Many webcams silently negotiate down when requesting YUYV at HD sizes; if
    // we switch the loopback format to that smaller negotiated size while a
    // consumer still requests the configured size, the frame can display in the
    // top-left quadrant.
    if (!OpenOutputLocked(outDev, out_w, out_h, cfg.fps, true, &opened_or_renegotiated, &oerr)) {
      last_error_ = oerr;
      running_ = false;
      start_notified_ = true;
      cv_.notify_all();
      return;
    }
  }

  const auto outA = writer_.Actual();

  // Scaling backend selection (cpu|gpu|auto).
  //
  // GPU scaling is optional and depends on runtime-loaded CUDA.
  //
  // Backends (fallback order):
  //   1) Maxine-compatible NvCV resize (NVCV)
  //   2) Pure CUDA resize kernel (OpenCUDA)
  //   3) None
  enum class GpuResizeBackend {
    none,
    maxine_nvcv,
    open_cuda,
  };

  auto GpuResizeBackendToActiveString = [](GpuResizeBackend b) -> std::string {
    switch (b) {
      case GpuResizeBackend::maxine_nvcv: return "gpu:maxine";
      case GpuResizeBackend::open_cuda: return "gpu:open_cuda";
      default: return "cpu";
    }
  };

  struct MaxineGpuScaler {
    bool initialized = false;
    std::string init_error;

    studiocast::maxine::NvcvApi nvcv;
    studiocast::maxine::CudaDriverApi cuda;
    studiocast::maxine::CudaBgrResizeBilinear resize;

    studiocast::maxine::NvCVImage gpu_in{};
    bool gpu_in_allocated = false;

    studiocast::maxine::NvCVImage gpu_scaled{};
    bool gpu_scaled_allocated = false;

    // Tight CPU-side staging buffers.
    std::vector<std::uint8_t> bgr_in;
    std::vector<std::uint8_t> bgr_out;
    studiocast::maxine::NvCVImage cpu_bgr_in{};
    studiocast::maxine::NvCVImage cpu_bgr_out{};

    ~MaxineGpuScaler() {
      if (gpu_scaled_allocated && nvcv.IsInitialized() && nvcv.f().NvCVImage_Dealloc) {
        (void)nvcv.f().NvCVImage_Dealloc(&gpu_scaled);
      }
      if (gpu_in_allocated && nvcv.IsInitialized() && nvcv.f().NvCVImage_Dealloc) {
        (void)nvcv.f().NvCVImage_Dealloc(&gpu_in);
      }
    }
  };

  struct OpenCudaGpuScaler {
    bool initialized = false;
    std::string init_error;

    studiocast::maxine::CudaDriverApi cuda;
    studiocast::maxine::CUstream stream = nullptr;
    bool stream_created = false;

    studiocast::cuda::CudaImage gpu_in{};
    studiocast::cuda::CudaImage gpu_scaled{};

    ~OpenCudaGpuScaler() {
      if (initialized) {
        std::string e;
        if (gpu_scaled.Valid()) (void)gpu_scaled.Free(&cuda, &e);
        if (gpu_in.Valid()) (void)gpu_in.Free(&cuda, &e);
        if (stream_created) (void)cuda.DestroyStream(stream, &e);
      }
    }

    bool EnsureInitialized(std::string* error_out) {
      if (error_out) error_out->clear();
      if (initialized) return true;
      std::string e;

      if (!cuda.Initialize(&e)) {
        init_error = e;
        if (error_out) *error_out = e;
        return false;
      }
      if (!cuda.EnsureContext(&e)) {
        init_error = e;
        if (error_out) *error_out = e;
        return false;
      }
      if (!studiocast::cuda::kernels::IsResizeBilinearAvailable(&e)) {
        init_error = e;
        if (error_out) *error_out = e;
        return false;
      }
      if (!cuda.CreateStream(&stream, &e)) {
        init_error = e;
        if (error_out) *error_out = e;
        return false;
      }
      stream_created = true;

      initialized = true;
      init_error.clear();
      return true;
    }
  };

  MaxineGpuScaler maxine_scaler;
  OpenCudaGpuScaler open_cuda_scaler;

  const bool want_gpu_scaling = (cfg.scaling_backend != ScalingBackendPreference::cpu);
  GpuResizeBackend gpu_backend = GpuResizeBackend::none;
  std::string scaling_backend_active = "cpu";
  std::string gpu_backend_init_note;

  if (want_gpu_scaling) {
    std::string maxine_err;
    bool maxine_ok = true;
    if (!maxine_scaler.cuda.Initialize(&maxine_err)) maxine_ok = false;
    if (maxine_ok && !maxine_scaler.nvcv.Initialize(studiocast::maxine::NvcvApi::Requirement::VfxCompat, &maxine_err)) {
      maxine_ok = false;
    }
    if (maxine_ok && !maxine_scaler.resize.Initialize(&maxine_scaler.cuda, &maxine_err)) maxine_ok = false;

    maxine_scaler.initialized = maxine_ok;
    maxine_scaler.init_error = maxine_err;

    if (maxine_ok) {
      gpu_backend = GpuResizeBackend::maxine_nvcv;
    } else {
      std::string open_err;
      if (open_cuda_scaler.EnsureInitialized(&open_err)) {
        gpu_backend = GpuResizeBackend::open_cuda;
      } else {
        // Keep some diagnostics for status/error reporting.
        std::ostringstream oss;
        if (!maxine_err.empty()) oss << "Maxine/NVCV init failed: " << maxine_err;
        if (!open_err.empty()) {
          if (oss.tellp() > 0) oss << " | ";
          oss << "OpenCUDA resize init failed: " << open_err;
        }
        gpu_backend_init_note = oss.str();
      }
    }

    scaling_backend_active = GpuResizeBackendToActiveString(gpu_backend);
  }

  {
    std::string policy_err;
    const bool gpu_resize_available = (gpu_backend != GpuResizeBackend::none);
    if (!CheckOutputResizeAllowed(capA.width,
                                  capA.height,
                                  outA.width,
                                  outA.height,
                                  gpu_resize_available,
                                  cfg.allow_cpu_resize,
                                  &policy_err)) {
      std::lock_guard<std::mutex> lock(mu_);
      last_error_ = policy_err;
      if (!gpu_backend_init_note.empty()) {
        last_error_ += "\nGPU resize diagnostics: " + gpu_backend_init_note;
      }
      running_ = false;
      start_notified_ = true;
      cv_.notify_all();
      return;
    }
  }

  studiocast::video::Rgb24Frame rgb;
  rgb.ResizeTight(capA.width, capA.height);
  std::size_t rgbStride = rgb.stride_bytes;

  std::vector<std::uint8_t> rgbScaled;

  std::vector<std::uint8_t> outBuf(outA.size_image);

  {
    std::lock_guard<std::mutex> lock(mu_);
    input_device_ = inDev;
    output_device_ = outDev;
    capture_ = capA;
    output_ = outA;
    scaling_backend_active_ = scaling_backend_active;
    scaling_from_ = capA;
    scaling_to_ = outA;
    frame_index_ = 0;
    last_error_.clear();
    running_ = true;

    start_notified_ = true;
    cv_.notify_all();
  }

  int frameIndex = 0;

  // Build a modular effect chain (Maxine-ready). This can be rebuilt live when
  // the GUI/daemon updates effect settings.
  studiocast::video::effects::EffectChain chain;
  studiocast::video::effects::BroadcastCameraEffects appliedFx{};
  studiocast::video::effects::BroadcastEffectsPlan appliedPlan{};

  // Optional deferred GPU output (used to avoid CPU resize when scaling is needed and no CPU tail effects are active).
  enum class DeferredGpuKind {
    none,
    nvcv_bgr,
    cuda_rgb,
  };

  struct DeferredGpuOut {
    DeferredGpuKind kind = DeferredGpuKind::none;

    // Maxine / NvCV (BGR)
    const studiocast::maxine::NvCVImage* nvcv_img = nullptr;  // non-owning
    studiocast::maxine::NvcvApi* nvcv = nullptr;              // non-owning

    // Open CUDA (RGB)
    const studiocast::cuda::CudaImage* cuda_img = nullptr;  // non-owning

    // Common
    studiocast::maxine::CUstream stream = nullptr;
    studiocast::maxine::CudaDriverApi* cuda = nullptr;  // non-owning
  };

  // GPU resize cache (allocated on demand when we can defer readback).
  studiocast::maxine::CudaBgrResizeBilinear gpu_resize_bilinear;
  studiocast::maxine::CudaDriverApi* gpu_resize_cuda = nullptr;  // non-owning
  studiocast::maxine::NvcvApi* gpu_resize_nvcv = nullptr;        // non-owning
  studiocast::maxine::NvCVImage gpu_bgr_scaled{};
  bool gpu_bgr_scaled_allocated = false;
  std::vector<std::uint8_t> bgr_scaled_out;
  studiocast::maxine::NvCVImage cpu_bgr_scaled{};

  // Open CUDA deferred GPU output (RGB) + resize cache.
  studiocast::cuda::CudaImage gpu_rgb_scaled;
  bool gpu_rgb_scaled_allocated = false;

  // Open CUDA per-frame GPU buffers (ping-pong) for GPU-only Open CUDA stages.
  // These persist across frames and are reallocated only when the capture size changes.
  studiocast::cuda::CudaImage open_cuda_frame_a;
  studiocast::cuda::CudaImage open_cuda_frame_b;
  const studiocast::cuda::CudaImage* open_cuda_curr = nullptr;
  studiocast::cuda::CudaImage* open_cuda_next = nullptr;
  bool open_cuda_uploaded_this_frame = false;

  struct MaxineBackgroundBlurContext {
    bool initialized = false;
    bool enabled = false;
    std::string last_error;

    studiocast::maxine::vfx::VfxApi vfx;
    studiocast::maxine::NvcvApi nvcv;
    studiocast::maxine::CudaDriverApi cuda;
    studiocast::maxine::CudaBgrVignette vignette;

    std::filesystem::path model_dir;

    std::unique_ptr<studiocast::maxine::effects::VfxGreenScreenEffect> greenscreen;
    std::unique_ptr<studiocast::maxine::effects::VfxBackgroundBlurEffect> blur;

    std::vector<std::uint8_t> bgr_in;
    std::vector<std::uint8_t> bgr_out;
    std::vector<std::uint8_t> bgr_bg;
    studiocast::maxine::NvCVImage cpu_bgr_in{};
    studiocast::maxine::NvCVImage cpu_bgr_out{};
    studiocast::maxine::NvCVImage cpu_bgr_bg{};

    studiocast::maxine::NvCVImage gpu_bgr{};
    bool gpu_bgr_allocated = false;

    studiocast::maxine::NvCVImage gpu_bgr_bg{};
    bool gpu_bgr_bg_allocated = false;

    studiocast::maxine::NvCVImage gpu_bgr_out_img{};
    bool gpu_bgr_out_allocated = false;

    // Background cache keys.
    std::uint32_t cached_bg_color_rgb = 0xFFFFFFFFu;  // invalid
    std::filesystem::path cached_bg_path;
    bool cached_bg_is_replace = false;

    // Temporary CPU-side buffers for replace image decoding/resizing.
    std::vector<std::uint8_t> tmp_replace_rgb_src;
    std::vector<std::uint8_t> tmp_replace_rgb_resized;

    ~MaxineBackgroundBlurContext() { Destroy(); }

    void Destroy() {
      greenscreen.reset();
      blur.reset();

      if (gpu_bgr_out_allocated && nvcv.IsInitialized() && nvcv.f().NvCVImage_Dealloc) {
        (void)nvcv.f().NvCVImage_Dealloc(&gpu_bgr_out_img);
      }
      gpu_bgr_out_img = studiocast::maxine::NvCVImage{};
      gpu_bgr_out_allocated = false;

      if (gpu_bgr_bg_allocated && nvcv.IsInitialized() && nvcv.f().NvCVImage_Dealloc) {
        (void)nvcv.f().NvCVImage_Dealloc(&gpu_bgr_bg);
      }
      gpu_bgr_bg = studiocast::maxine::NvCVImage{};
      gpu_bgr_bg_allocated = false;

      if (gpu_bgr_allocated && nvcv.IsInitialized() && nvcv.f().NvCVImage_Dealloc) {
        (void)nvcv.f().NvCVImage_Dealloc(&gpu_bgr);
      }
      gpu_bgr = studiocast::maxine::NvCVImage{};
      gpu_bgr_allocated = false;

      bgr_in.clear();
      bgr_out.clear();
      bgr_bg.clear();
      cpu_bgr_in = studiocast::maxine::NvCVImage{};
      cpu_bgr_out = studiocast::maxine::NvCVImage{};
      cpu_bgr_bg = studiocast::maxine::NvCVImage{};

      cached_bg_color_rgb = 0xFFFFFFFFu;
      cached_bg_path.clear();
      cached_bg_is_replace = false;

      initialized = false;
      enabled = false;
    }

    static std::filesystem::path InferModelsDirFromLibrary(const std::filesystem::path& lib) {
      std::error_code ec;
      std::filesystem::path p = lib.parent_path();
      for (int i = 0; i < 5 && !p.empty(); ++i) {
        const auto cand = p / "models";
        if (std::filesystem::exists(cand, ec) && std::filesystem::is_directory(cand, ec)) {
          return cand;
        }
        p = p.parent_path();
      }
      return {};
    }

    bool EnsureInitialized(int width,
                           int height,
                           const studiocast::video::effects::BroadcastCameraEffects& fx,
                           std::string* error) {
      last_error.clear();

      if (initialized) {
        // Effects can be reconfigured live.
        std::string cfg_err;
        if (greenscreen && !greenscreen->Configure(fx, &cfg_err)) {
          if (error) *error = cfg_err;
          return false;
        }
        if (blur && !blur->Configure(fx, &cfg_err)) {
          if (error) *error = cfg_err;
          return false;
        }
        return true;
      }

      std::string err;
      if (!vfx.Initialize(&err)) {
        last_error = "Maxine VFX runtime unavailable: " + err;
        if (error) *error = last_error;
        return false;
      }
      if (!nvcv.Initialize(studiocast::maxine::NvcvApi::Requirement::VfxCompat, &err)) {
        last_error = "NvCVImage runtime unavailable: " + err;
        if (error) *error = last_error;
        return false;
      }

      {
        std::string cuda_err;
        if (!cuda.Initialize(&cuda_err)) {
          last_error = "CUDA driver API unavailable: " + cuda_err;
          if (error) *error = last_error;
          return false;
        }
        std::string vig_err;
        if (!vignette.Initialize(&cuda, &vig_err)) {
          last_error = "CUDA vignette init failed: " + vig_err;
          if (error) *error = last_error;
          return false;
        }
      }
      if (model_dir.empty()) {
        model_dir = InferModelsDirFromLibrary(vfx.library_path());
      }

      // Allocate CPU-side BGR staging buffers and wrap with NvCVImage_Init.
      const std::size_t stride = static_cast<std::size_t>(width) * 3u;
      bgr_in.resize(stride * static_cast<std::size_t>(height));
      bgr_out.resize(stride * static_cast<std::size_t>(height));
      bgr_bg.resize(stride * static_cast<std::size_t>(height));

      if (!nvcv.f().NvCVImage_Init) {
        last_error = "NvCVImage_Init missing from NvCVImage runtime.";
        if (error) *error = last_error;
        return false;
      }

      auto st = nvcv.f().NvCVImage_Init(&cpu_bgr_in,
                                       static_cast<unsigned>(width),
                                       static_cast<unsigned>(height),
                                       static_cast<int>(stride),
                                       bgr_in.data(),
                                       studiocast::maxine::NVCV_BGR,
                                       studiocast::maxine::NVCV_U8,
                                       studiocast::maxine::NVCV_CHUNKY,
                                       studiocast::maxine::NVCV_CPU);
      if (st != studiocast::maxine::NVCV_SUCCESS) {
        last_error = "NvCVImage_Init(cpu BGR in) failed: " + std::to_string(st);
        if (error) *error = last_error;
        return false;
      }

      st = nvcv.f().NvCVImage_Init(&cpu_bgr_out,
                                  static_cast<unsigned>(width),
                                  static_cast<unsigned>(height),
                                  static_cast<int>(stride),
                                  bgr_out.data(),
                                  studiocast::maxine::NVCV_BGR,
                                  studiocast::maxine::NVCV_U8,
                                  studiocast::maxine::NVCV_CHUNKY,
                                  studiocast::maxine::NVCV_CPU);
      if (st != studiocast::maxine::NVCV_SUCCESS) {
        last_error = "NvCVImage_Init(cpu BGR out) failed: " + std::to_string(st);
        if (error) *error = last_error;
        return false;
      }

      st = nvcv.f().NvCVImage_Init(&cpu_bgr_bg,
                                  static_cast<unsigned>(width),
                                  static_cast<unsigned>(height),
                                  static_cast<int>(stride),
                                  bgr_bg.data(),
                                  studiocast::maxine::NVCV_BGR,
                                  studiocast::maxine::NVCV_U8,
                                  studiocast::maxine::NVCV_CHUNKY,
                                  studiocast::maxine::NVCV_CPU);
      if (st != studiocast::maxine::NVCV_SUCCESS) {
        last_error = "NvCVImage_Init(cpu BGR bg) failed: " + std::to_string(st);
        if (error) *error = last_error;
        return false;
      }

      // Allocate GPU input image.
      st = nvcv.f().NvCVImage_Alloc(&gpu_bgr,
                                   static_cast<unsigned>(width),
                                   static_cast<unsigned>(height),
                                   studiocast::maxine::NVCV_BGR,
                                   studiocast::maxine::NVCV_U8,
                                   studiocast::maxine::NVCV_CHUNKY,
                                   studiocast::maxine::NVCV_GPU,
                                   /*alignment=*/0);
      if (st != studiocast::maxine::NVCV_SUCCESS) {
        last_error = "NvCVImage_Alloc(gpu BGR) failed: " + std::to_string(st);
        if (error) *error = last_error;
        return false;
      }
      gpu_bgr_allocated = true;

      // Allocate GPU background image (BGRu8) and a GPU output image used by compositing.
      st = nvcv.f().NvCVImage_Alloc(&gpu_bgr_bg,
                                   static_cast<unsigned>(width),
                                   static_cast<unsigned>(height),
                                   studiocast::maxine::NVCV_BGR,
                                   studiocast::maxine::NVCV_U8,
                                   studiocast::maxine::NVCV_CHUNKY,
                                   studiocast::maxine::NVCV_GPU,
                                   /*alignment=*/0);
      if (st != studiocast::maxine::NVCV_SUCCESS) {
        last_error = "NvCVImage_Alloc(gpu BGR bg) failed: " + std::to_string(st);
        if (error) *error = last_error;
        return false;
      }
      gpu_bgr_bg_allocated = true;

      st = nvcv.f().NvCVImage_Alloc(&gpu_bgr_out_img,
                                   static_cast<unsigned>(width),
                                   static_cast<unsigned>(height),
                                   studiocast::maxine::NVCV_BGR,
                                   studiocast::maxine::NVCV_U8,
                                   studiocast::maxine::NVCV_CHUNKY,
                                   studiocast::maxine::NVCV_GPU,
                                   /*alignment=*/0);
      if (st != studiocast::maxine::NVCV_SUCCESS) {
        last_error = "NvCVImage_Alloc(gpu BGR out) failed: " + std::to_string(st);
        if (error) *error = last_error;
        return false;
      }
      gpu_bgr_out_allocated = true;

      greenscreen = std::make_unique<studiocast::maxine::effects::VfxGreenScreenEffect>(&vfx, &nvcv, model_dir);
      blur = std::make_unique<studiocast::maxine::effects::VfxBackgroundBlurEffect>(&vfx, &nvcv, model_dir);

      if (!greenscreen->Configure(fx, &err) || !blur->Configure(fx, &err)) {
        last_error = "Maxine effect Configure failed: " + err;
        if (error) *error = last_error;
        Destroy();
        return false;
      }
      if (!greenscreen->Initialize(&err)) {
        last_error = "Maxine green screen Initialize failed: " + err;
        if (error) *error = last_error;
        Destroy();
        return false;
      }
      if (!blur->Initialize(&err)) {
        last_error = "Maxine background blur Initialize failed: " + err;
        if (error) *error = last_error;
        Destroy();
        return false;
      }

      initialized = true;
      return true;
    }

    bool EnsureBackgroundGpu(int width,
                             int height,
                             const studiocast::video::effects::BroadcastCameraEffects& fx,
                             studiocast::maxine::CUstream stream,
                             std::string* error) {
      if (!initialized || !nvcv.IsInitialized()) {
        if (error) *error = "NvCVImage runtime not initialized.";
        return false;
      }
      if (!nvcv.f().NvCVImage_Transfer) {
        if (error) *error = "NvCVImage_Transfer unavailable.";
        return false;
      }

      const bool wantReplace = (fx.virtual_background.mode ==
                                studiocast::video::effects::VirtualBackgroundMode::replace);

      std::uint32_t remove_rgb = 0x000000u;
      if (!wantReplace) {
        // remove mode uses a solid color
        if (!ParseRgbHex(fx.virtual_background.remove_color, &remove_rgb)) {
          remove_rgb = 0x000000u;
        }
      }

      bool needsUpload = false;
      if (wantReplace) {
        const std::filesystem::path path = fx.virtual_background.replace_path;
        if (!cached_bg_is_replace || cached_bg_path != path) {
          needsUpload = true;
        }
      } else {
        // remove mode uses a solid color
        if (cached_bg_is_replace || cached_bg_color_rgb != (remove_rgb & 0xFFFFFFu)) {
          needsUpload = true;
        }
      }

      if (!needsUpload) {
        return true;
      }

      const std::size_t stride = static_cast<std::size_t>(width) * 3u;
      if (bgr_bg.size() != stride * static_cast<std::size_t>(height)) {
        // Should not happen for a running pipeline; fail safe.
        if (error) *error = "Background buffer size mismatch.";
        return false;
      }

      if (wantReplace) {
        if (fx.virtual_background.replace_path.empty()) {
          if (error) *error = "virtual_background.replace_path not set.";
          return false;
        }

        const std::filesystem::path img_path = fx.virtual_background.replace_path;

        int iw = 0, ih = 0;
        std::string img_err;
        if (!LoadImageRgb24(img_path, &iw, &ih, &tmp_replace_rgb_src, &img_err)) {
          if (error) *error = "Failed to load replace image: " + img_err;
          return false;
        }

        if (!ResizeRgb24Bilinear(tmp_replace_rgb_src.data(),
                                 iw,
                                 ih,
                                 static_cast<std::size_t>(iw) * 3u,
                                 width,
                                 height,
                                 &tmp_replace_rgb_resized,
                                 stride,
                                 &img_err)) {
          if (error) *error = "Failed to resize replace image: " + img_err;
          return false;
        }

        // RGB -> BGR into the staging buffer.
        for (int y = 0; y < height; ++y) {
          const auto* src_row = tmp_replace_rgb_resized.data() + static_cast<std::size_t>(y) * stride;
          auto* dst_row = bgr_bg.data() + static_cast<std::size_t>(y) * stride;
          for (int x = 0; x < width; ++x) {
            const std::size_t i = static_cast<std::size_t>(x) * 3u;
            dst_row[i + 0] = src_row[i + 2];
            dst_row[i + 1] = src_row[i + 1];
            dst_row[i + 2] = src_row[i + 0];
          }
        }

        cached_bg_is_replace = true;
        cached_bg_path = img_path;
      } else {
        const std::uint32_t rgb = remove_rgb & 0xFFFFFFu;
        const std::uint8_t r = static_cast<std::uint8_t>((rgb >> 16u) & 0xFFu);
        const std::uint8_t g = static_cast<std::uint8_t>((rgb >> 8u) & 0xFFu);
        const std::uint8_t b = static_cast<std::uint8_t>((rgb >> 0u) & 0xFFu);

        for (int y = 0; y < height; ++y) {
          auto* row = bgr_bg.data() + static_cast<std::size_t>(y) * stride;
          for (int x = 0; x < width; ++x) {
            const std::size_t i = static_cast<std::size_t>(x) * 3u;
            row[i + 0] = b;
            row[i + 1] = g;
            row[i + 2] = r;
          }
        }

        cached_bg_is_replace = false;
        cached_bg_color_rgb = rgb;
        cached_bg_path.clear();
      }

      const auto up = nvcv.f().NvCVImage_Transfer(&cpu_bgr_bg, &gpu_bgr_bg, 1.0f, stream, nullptr);
      if (up != studiocast::maxine::NVCV_SUCCESS) {
        if (error) *error = "NvCVImage_Transfer(bg cpu->gpu) failed: " + std::to_string(up);
        return false;
      }
      return true;
    }

    bool ApplyRgbInPlace(std::uint8_t* rgb,
                         int width,
                         int height,
                         std::size_t rgb_stride,
                         const studiocast::video::effects::BroadcastCameraEffects& fx,
                         bool apply_vignette,
                         float vignette_center_x_px,
                         float vignette_center_y_px,
                         std::string* error,
                         bool defer_readback,
                         DeferredGpuOut* deferred_out) {
      if (!initialized || !greenscreen || !blur) {
        if (error) *error = "Maxine virtual background not initialized.";
        return false;
      }
      if (!rgb || width <= 0 || height <= 0 || rgb_stride == 0) {
        if (error) *error = "Invalid RGB buffer.";
        return false;
      }

      // RGB -> BGR staging
      studiocast::video::Rgb24ToBgr24(rgb,
                                     bgr_in.data(),
                                     width,
                                     height,
                                     rgb_stride,
                                     rgb_stride);

      // Upload CPU->GPU.
      const auto up = nvcv.f().NvCVImage_Transfer(&cpu_bgr_in, &gpu_bgr, 1.0f, greenscreen->cuda_stream(), nullptr);
      if (up != studiocast::maxine::NVCV_SUCCESS) {
        if (error) *error = "NvCVImage_Transfer(cpu->gpu) failed: " + std::to_string(up);
        return false;
      }

      studiocast::video::GpuFrame frame;
      frame.width = width;
      frame.height = height;
      frame.nvcv_gpu = &gpu_bgr;
      frame.cuda_stream = greenscreen->cuda_stream();

      std::string cfg_err;
      if (!greenscreen->Configure(fx, &cfg_err)) {
        if (error) *error = cfg_err;
        return false;
      }

      const studiocast::maxine::NvCVImage* out_gpu = nullptr;
      const auto stream = greenscreen->cuda_stream();

      if (fx.virtual_background.mode == studiocast::video::effects::VirtualBackgroundMode::blur) {
        if (!blur->Configure(fx, &cfg_err)) {
          if (error) *error = cfg_err;
          return false;
        }
      }

      std::string proc_err;
      const auto st = greenscreen->Process(frame, &proc_err);
      if (st != studiocast::maxine::NVCV_SUCCESS) {
        if (error) *error = proc_err.empty() ? std::to_string(st) : proc_err;
        return false;
      }

      const auto* matte = greenscreen->MatteGpu();
      if (!matte) {
        if (error) *error = "Green Screen did not produce a matte.";
        return false;
      }

      frame.matte_gpu = matte;

      if (fx.virtual_background.mode == studiocast::video::effects::VirtualBackgroundMode::blur) {
        const auto st2 = blur->Process(frame, &proc_err);
        if (st2 != studiocast::maxine::NVCV_SUCCESS) {
          if (error) *error = proc_err.empty() ? std::to_string(st2) : proc_err;
          return false;
        }

        out_gpu = blur->OutputGpu();
        if (!out_gpu) {
          if (error) *error = "Background Blur did not produce an output image.";
          return false;
        }

        if (apply_vignette && fx.vignette.enabled) {
          const float vig_intensity = Clamp01FromPercent(fx.vignette.intensity);
          if (vig_intensity > 0.0001f) {
            std::string ve;
            if (!vignette.ApplyInPlace(const_cast<studiocast::maxine::NvCVImage*>(out_gpu),
                                      vig_intensity,
                                      vignette_center_x_px,
                                      vignette_center_y_px,
                                      blur->cuda_stream(),
                                      &ve)) {
              if (error) *error = ve;
              return false;
            }
          }
        }

        if (defer_readback) {
          if (!deferred_out) {
            if (error) *error = "defer_readback requires deferred_out.";
            return false;
          }
          deferred_out->kind = DeferredGpuKind::nvcv_bgr;
          deferred_out->nvcv_img = out_gpu;
          deferred_out->nvcv = &nvcv;
          deferred_out->cuda_img = nullptr;
          deferred_out->cuda = &cuda;
          deferred_out->stream = blur->cuda_stream();
          return true;
        }

        const auto down = nvcv.f().NvCVImage_Transfer(out_gpu, &cpu_bgr_out, 1.0f, blur->cuda_stream(), nullptr);
        if (down != studiocast::maxine::NVCV_SUCCESS) {
          if (error) *error = "NvCVImage_Transfer(gpu->cpu) failed: " + std::to_string(down);
          return false;
        }

        if (!SyncAfterGpuToCpuTransfer(cuda,
                                       blur->cuda_stream(),
                                       "Maxine VB blur: after gpu->cpu transfer",
                                       error)) {
          return false;
        }
      } else if (fx.virtual_background.mode == studiocast::video::effects::VirtualBackgroundMode::remove ||
                 fx.virtual_background.mode == studiocast::video::effects::VirtualBackgroundMode::replace) {
        if (!nvcv.f().NvCVImage_Composite) {
          if (error) *error = "NvCVImage_Composite unavailable.";
          return false;
        }

        std::string bg_err;
        if (!EnsureBackgroundGpu(width, height, fx, stream, &bg_err)) {
          if (error) *error = bg_err;
          return false;
        }

        const auto stc = nvcv.f().NvCVImage_Composite(&gpu_bgr, &gpu_bgr_bg, matte, &gpu_bgr_out_img, stream);
        if (stc != studiocast::maxine::NVCV_SUCCESS) {
          if (error) *error = "NvCVImage_Composite failed: " + std::to_string(stc);
          return false;
        }

        if (apply_vignette && fx.vignette.enabled) {
          const float vig_intensity = Clamp01FromPercent(fx.vignette.intensity);
          if (vig_intensity > 0.0001f) {
            std::string ve;
            if (!vignette.ApplyInPlace(&gpu_bgr_out_img,
                                      vig_intensity,
                                      vignette_center_x_px,
                                      vignette_center_y_px,
                                      stream,
                                      &ve)) {
              if (error) *error = ve;
              return false;
            }
          }
        }

        if (defer_readback) {
          if (!deferred_out) {
            if (error) *error = "defer_readback requires deferred_out.";
            return false;
          }
          deferred_out->kind = DeferredGpuKind::nvcv_bgr;
          deferred_out->nvcv_img = &gpu_bgr_out_img;
          deferred_out->nvcv = &nvcv;
          deferred_out->cuda_img = nullptr;
          deferred_out->cuda = &cuda;
          deferred_out->stream = stream;
          return true;
        }

        const auto down = nvcv.f().NvCVImage_Transfer(&gpu_bgr_out_img, &cpu_bgr_out, 1.0f, stream, nullptr);
        if (down != studiocast::maxine::NVCV_SUCCESS) {
          if (error) *error = "NvCVImage_Transfer(gpu->cpu) failed: " + std::to_string(down);
          return false;
        }

        if (!SyncAfterGpuToCpuTransfer(cuda,
                                       stream,
                                       "Maxine VB composite: after gpu->cpu transfer",
                                       error)) {
          return false;
        }
      } else {
        // Not a virtual background mode we handle.
        return true;
      }

      // BGR -> RGB back into the pipeline buffer.
      studiocast::video::Bgr24ToRgb24(bgr_out.data(),
                                     rgb,
                                     width,
                                     height,
                                     rgb_stride,
                                     rgb_stride);
      return true;
    }
  } maxine_bg_blur;

  struct OpenCudaVirtualBackgroundContext {
    bool initialized = false;
    bool enabled = false;
    std::string last_error;

    // The model pack currently active in the pipeline. This is used to hot-swap the
    // Open CUDA session when the user changes fx.virtual_background.model_id.
    std::string active_model_id;

    studiocast::maxine::CudaDriverApi cuda;

    // Persistent stream used for the entire Open CUDA virtual background pipeline.
    // This avoids accidental default-stream work and allows ORT (when supported) to
    // run on the same explicit stream via user_compute_stream.
    studiocast::maxine::CUstream vb_stream = nullptr;

    std::optional<studiocast::open_video::ModelPack> pack;
    std::unique_ptr<studiocast::open_cuda::OpenCudaMattingSession> session;

    // GPU buffers.
    studiocast::cuda::CudaImage frame_rgb;   // rgb_u8, WxH
    studiocast::cuda::CudaImage out_rgb;     // rgb_u8, WxH

    studiocast::cuda::CudaTensor alpha_tensor;          // 1x1xH_inxW_in (contiguous)
    studiocast::cuda::CudaImage alpha_model_view;       // f32_1 view over alpha_tensor (no ownership)

    // Matting inference cache. Multiple effects (VB, Auto Frame, Key Light) can consume the
    // same foreground matte in a single frame. Cache the model output keyed by capture
    // sequence so we don't re-run the ONNX model when multiple effects are enabled.
    std::uint64_t cached_matte_sequence = 0;
    bool cached_matte_valid = false;

    // Optional CPU-side cache of the alpha tensor (downloaded once per frame when needed).
    std::uint64_t cached_alpha_cpu_sequence = 0;
    bool cached_alpha_cpu_valid = false;
    std::vector<float> alpha_cpu;
    studiocast::cuda::CudaImage alpha_resized;          // f32_1, WxH
    studiocast::cuda::CudaImage alpha_tmp;              // f32_1, WxH
    studiocast::cuda::CudaImage alpha_feather;          // f32_1, WxH

    studiocast::cuda::CudaImage blur_tmp;  // rgb_u8, WxH
    studiocast::cuda::CudaImage blurred;   // rgb_u8, WxH

    studiocast::cuda::CudaImage bg_rgb;  // rgb_u8, WxH (replace mode)
    studiocast::cuda::CudaImage bg_src_rgb;  // rgb_u8, WxH_src (decoded image, cached allocation)

    // Replace-mode background cache.
    // Cache the decoded+uploaded source image at its native resolution, then GPU-resize
    // into bg_rgb for the current frame size.
    std::filesystem::path cached_bg_src_path;
    std::filesystem::file_time_type cached_bg_src_mtime{};
    int cached_bg_src_w = 0;
    int cached_bg_src_h = 0;
    bool cached_bg_src_valid = false;
    std::uint64_t cached_bg_src_gen = 0;

    int cached_bg_dst_w = 0;
    int cached_bg_dst_h = 0;
    bool cached_bg_dst_valid = false;
    std::uint64_t cached_bg_dst_src_gen = 0;

    std::vector<std::uint8_t> tmp_replace_rgb_src;

    ~OpenCudaVirtualBackgroundContext() { Destroy(); }

    void Destroy() {
      if (cuda.IsInitialized() && vb_stream != nullptr) {
        std::string serr;
        (void)cuda.StreamSynchronize(vb_stream, &serr);
      }

      if (cuda.IsInitialized()) {
        (void)frame_rgb.Free(&cuda, nullptr);
        (void)out_rgb.Free(&cuda, nullptr);
        (void)alpha_resized.Free(&cuda, nullptr);
        (void)alpha_tmp.Free(&cuda, nullptr);
        (void)alpha_feather.Free(&cuda, nullptr);
        (void)blur_tmp.Free(&cuda, nullptr);
        (void)blurred.Free(&cuda, nullptr);
        (void)bg_rgb.Free(&cuda, nullptr);
        (void)bg_src_rgb.Free(&cuda, nullptr);
        (void)alpha_tensor.Free(&cuda, nullptr);

        if (vb_stream != nullptr) {
          std::string derr;
          (void)cuda.DestroyStream(vb_stream, &derr);
          vb_stream = nullptr;
        }
      }

      alpha_model_view = studiocast::cuda::CudaImage{};
      session.reset();
      pack.reset();
      active_model_id.clear();

      cached_matte_sequence = 0;
      cached_matte_valid = false;
      cached_alpha_cpu_sequence = 0;
      cached_alpha_cpu_valid = false;
      alpha_cpu.clear();

      cached_bg_src_path.clear();
      cached_bg_src_mtime = {};
      cached_bg_src_w = 0;
      cached_bg_src_h = 0;
      cached_bg_src_valid = false;
      cached_bg_src_gen = 0;

      cached_bg_dst_w = 0;
      cached_bg_dst_h = 0;
      cached_bg_dst_valid = false;
      cached_bg_dst_src_gen = 0;
      tmp_replace_rgb_src.clear();

      initialized = false;
      enabled = false;
      last_error.clear();
    }

    bool EnsureInitialized(int frame_w,
                           int frame_h,
                           const studiocast::video::effects::BroadcastCameraEffects& fx,
                           std::string* error,
                           bool require_vb_buffers = true) {
      if (error) error->clear();
      if (frame_w <= 0 || frame_h <= 0) {
        last_error = "Open CUDA: invalid frame size.";
        if (error) *error = last_error;
        return false;
      }

      std::string err;
      if (!cuda.IsInitialized()) {
        if (!cuda.Initialize(&err)) {
          last_error = "Open CUDA: CUDA unavailable: " + err;
          if (error) *error = last_error;
          return false;
        }
      }
      if (!cuda.EnsureContext(&err)) {
        last_error = "Open CUDA: failed to ensure CUDA context: " + err;
        if (error) *error = last_error;
        return false;
      }

      if (vb_stream == nullptr) {
        std::string serr;
        if (!cuda.CreateStream(&vb_stream, &serr)) {
          last_error = "Open CUDA: failed to create CUDA stream: " + serr;
          if (error) *error = last_error;
          return false;
        }
      }

      // Resolve model pack.
      // Open CUDA models are selected per-effect, but remain an Open CUDA-only concern.
      // If fx.virtual_background.model_id is empty, preserve the existing deterministic default.
      const auto reg = studiocast::open_video::ModelPackRegistry::ScanDefault();
      std::string requested_model_id = fx.virtual_background.model_id;
      if (requested_model_id.empty()) {
        requested_model_id = reg.DefaultModelIdForTask("matting");
      }

      if (requested_model_id.empty()) {
        last_error =
            "Open CUDA: no usable model packs found (install under ~/.local/share/studiocast/models/open_video/<subject>/<pack_dir>/).";
        if (error) *error = last_error;
        return false;
      }

      auto DescribeInstalledModelIds = [&reg]() -> std::string {
        std::ostringstream oss;
        std::vector<std::string> ids;
        for (const auto& m : reg.ListModels()) {
          if (m.task != "matting") continue;
          ids.push_back(m.id);
        }
        if (ids.empty()) {
          oss << "<none>";
          return oss.str();
        }
        bool first = true;
        for (const auto& id : ids) {
          if (!first) oss << ", ";
          first = false;
          oss << id;
        }
        return oss.str();
      };

      if (requested_model_id != active_model_id) {
        // Model changed: reset model-dependent state so ORT session and model-sized buffers
        // are rebuilt cleanly.
        session.reset();
        pack.reset();
        (void)alpha_tensor.Free(&cuda, nullptr);
        alpha_model_view = studiocast::cuda::CudaImage{};

        cached_matte_sequence = 0;
        cached_matte_valid = false;
        cached_alpha_cpu_sequence = 0;
        cached_alpha_cpu_valid = false;
        alpha_cpu.clear();
      }

      if (!pack.has_value()) {
        const auto p = reg.Find("matting", requested_model_id);
        if (!p.has_value()) {
          last_error = "Open CUDA: selected model_id '" + requested_model_id + "' not found. Installed: " +
                       DescribeInstalledModelIds();
          if (error) *error = last_error;
          return false;
        }
        pack = *p;
        active_model_id = requested_model_id;

        if (pack->task != "matting") {
          last_error = "Open CUDA: selected model_id '" + requested_model_id + "' has task '" + pack->task +
                       "' (expected 'matting').";
          if (error) *error = last_error;
          return false;
        }
        if (!pack->matting.has_value()) {
          last_error = "Open CUDA: selected model_id '" + requested_model_id + "' is missing matting metadata.";
          if (error) *error = last_error;
          return false;
        }
      }

      if (!session) {
        session = std::make_unique<studiocast::open_cuda::OpenCudaMattingSession>(&cuda, *pack);
      }

      // Allocate alpha tensor at model resolution.
      {
        std::string aerr;
        if (!alpha_tensor.ReallocIfNeededNchwF32(&cuda, 1, 1, pack->matting->input.height, pack->matting->input.width, &aerr)) {
          last_error = "Open CUDA: failed to allocate alpha tensor: " + aerr;
          if (error) *error = last_error;
          return false;
        }

        // View alpha tensor as a 2D f32 image.
        alpha_model_view.ptr = alpha_tensor.ptr;
        alpha_model_view.pitch = static_cast<std::size_t>(pack->matting->input.width) * 4u;
        alpha_model_view.w = pack->matting->input.width;
        alpha_model_view.h = pack->matting->input.height;
        alpha_model_view.format = studiocast::cuda::PixelFormatGpu::f32_1;
        alpha_model_view.owns_memory = false;
      }

      // Per-frame-size buffers.
      {
        std::string berr;
        if (!frame_rgb.ReallocIfNeeded(&cuda, frame_w, frame_h, studiocast::cuda::PixelFormatGpu::rgb_u8, &berr) ||
            !out_rgb.ReallocIfNeeded(&cuda, frame_w, frame_h, studiocast::cuda::PixelFormatGpu::rgb_u8, &berr)) {
          last_error = "Open CUDA: failed to allocate GPU buffers: " + berr;
          if (error) *error = last_error;
          return false;
        }

        // Only allocate VB-specific buffers when the caller is actually going to run the
        // virtual background stage. Auto Frame / Key Light use the matte only.
        if (require_vb_buffers) {
          if (!alpha_resized.ReallocIfNeeded(&cuda, frame_w, frame_h, studiocast::cuda::PixelFormatGpu::f32_1, &berr) ||
              !alpha_tmp.ReallocIfNeeded(&cuda, frame_w, frame_h, studiocast::cuda::PixelFormatGpu::f32_1, &berr) ||
              !alpha_feather.ReallocIfNeeded(&cuda, frame_w, frame_h, studiocast::cuda::PixelFormatGpu::f32_1, &berr) ||
              !blur_tmp.ReallocIfNeeded(&cuda, frame_w, frame_h, studiocast::cuda::PixelFormatGpu::rgb_u8, &berr) ||
              !blurred.ReallocIfNeeded(&cuda, frame_w, frame_h, studiocast::cuda::PixelFormatGpu::rgb_u8, &berr)) {
            last_error = "Open CUDA: failed to allocate GPU buffers: " + berr;
            if (error) *error = last_error;
            return false;
          }
        }
      }

      // If the frame size changed, keep the uploaded source background but invalidate the
      // resized destination so we'll re-run the GPU resize.
      if (cached_bg_dst_w != frame_w || cached_bg_dst_h != frame_h) {
        cached_bg_dst_valid = false;
        cached_bg_dst_w = frame_w;
        cached_bg_dst_h = frame_h;
      }

      // Strength is the canonical knob; clamp once for deterministic behavior.
      (void)fx;

      initialized = true;
      enabled = true;
      return true;
    }

    bool EnsureReplaceBackgroundGpu(int width,
                                   int height,
                                   const std::filesystem::path& path,
                                   std::string* error) {
      if (error) error->clear();
      if (!initialized) {
        if (error) *error = "Open CUDA: not initialized.";
        return false;
      }
      if (path.empty()) {
        if (error) *error = "Open CUDA: virtual_background.replace_path not set.";
        return false;
      }

      std::error_code ec;
      const auto mtime = std::filesystem::last_write_time(path, ec);
      if (ec) {
        if (error) *error = "Open CUDA: failed to stat replace image: " + ec.message();
        return false;
      }

      const bool src_cache_hit = (cached_bg_src_valid &&
                                 cached_bg_src_path == path &&
                                 cached_bg_src_mtime == mtime &&
                                 bg_src_rgb.Valid());

      if (!src_cache_hit) {
        int iw = 0, ih = 0;
        std::string img_err;
        if (!studiocast::video::LoadImageRgb24(path, &iw, &ih, &tmp_replace_rgb_src, &img_err)) {
          if (error) *error = "Open CUDA: failed to load replace image: " + img_err;
          return false;
        }

        std::string berr;
        if (!bg_src_rgb.ReallocIfNeeded(&cuda, iw, ih, studiocast::cuda::PixelFormatGpu::rgb_u8, &berr)) {
          if (error) *error = "Open CUDA: failed to allocate bg_src_rgb: " + berr;
          return false;
        }
        if (!bg_src_rgb.UploadFromCpuRgb24(&cuda,
                                           tmp_replace_rgb_src.data(),
                                           static_cast<std::size_t>(iw) * 3u,
                                           vb_stream,
                                           &berr)) {
          if (error) *error = "Open CUDA: failed to upload bg_src_rgb: " + berr;
          return false;
        }

        cached_bg_src_path = path;
        cached_bg_src_mtime = mtime;
        cached_bg_src_w = iw;
        cached_bg_src_h = ih;
        cached_bg_src_valid = true;
        ++cached_bg_src_gen;

        // Source changed -> destination must be re-generated.
        cached_bg_dst_valid = false;
      }

      const bool dst_cache_hit = (cached_bg_dst_valid &&
                                 cached_bg_dst_w == width &&
                                 cached_bg_dst_h == height &&
                                 cached_bg_dst_src_gen == cached_bg_src_gen &&
                                 bg_rgb.Valid());
      if (dst_cache_hit) {
        return true;
      }

      std::string berr;
      if (!bg_rgb.ReallocIfNeeded(&cuda, width, height, studiocast::cuda::PixelFormatGpu::rgb_u8, &berr)) {
        if (error) *error = "Open CUDA: failed to allocate bg_rgb: " + berr;
        return false;
      }

      if (!studiocast::cuda::kernels::ResizeBilinear(bg_src_rgb, bg_rgb, vb_stream, &berr)) {
        if (error) *error = "Open CUDA: failed to resize replace image on GPU: " + berr;
        return false;
      }

      cached_bg_dst_w = width;
      cached_bg_dst_h = height;
      cached_bg_dst_src_gen = cached_bg_src_gen;
      cached_bg_dst_valid = true;
      return true;
    }

    void InvalidateMatteCache() {
      cached_matte_sequence = 0;
      cached_matte_valid = false;
      cached_alpha_cpu_sequence = 0;
      cached_alpha_cpu_valid = false;
      alpha_cpu.clear();
    }

    // Ensure matting inference has been run for this frame, using a GPU input frame.
    // If matting was already computed for the same capture sequence, this is a no-op.
    bool EnsureMatteForFrameGpu(const studiocast::cuda::CudaImage& in_rgb,
                               std::uint64_t capture_sequence,
                               int width,
                               int height,
                               const studiocast::video::effects::BroadcastCameraEffects& fx,
                               std::string* error) {
      if (error) error->clear();

      std::string init_err;
      if (!EnsureInitialized(width, height, fx, &init_err, /*require_vb_buffers=*/true)) {
        if (error) *error = init_err;
        return false;
      }

      if (cached_matte_valid && cached_matte_sequence == capture_sequence) {
        return true;
      }

      std::string matte_err;
      if (!session->Run(vb_stream, in_rgb, &alpha_tensor, &matte_err)) {
        if (error) *error = matte_err;
        return false;
      }

      cached_matte_valid = true;
      cached_matte_sequence = capture_sequence;
      cached_alpha_cpu_valid = false;
      return true;
    }

    // Ensure matting inference has been run for this frame, using a CPU RGB input frame.
    // If matting was already computed for the same capture sequence, this is a no-op.
    bool EnsureMatteForFrameCpu(const std::uint8_t* rgb,
                               std::size_t rgb_stride,
                               std::uint64_t capture_sequence,
                               int width,
                               int height,
                               const studiocast::video::effects::BroadcastCameraEffects& fx,
                               std::string* error) {
      if (error) error->clear();
      if (!rgb || rgb_stride == 0) {
        if (error) *error = "Open CUDA: invalid RGB buffer.";
        return false;
      }

      std::string init_err;
      if (!EnsureInitialized(width, height, fx, &init_err, /*require_vb_buffers=*/false)) {
        if (error) *error = init_err;
        return false;
      }

      if (cached_matte_valid && cached_matte_sequence == capture_sequence) {
        return true;
      }

      std::string up_err;
      if (!frame_rgb.UploadFromCpuRgb24(&cuda, rgb, rgb_stride, vb_stream, &up_err)) {
        if (error) *error = "Open CUDA: frame upload failed: " + up_err;
        return false;
      }

      std::string matte_err;
      if (!session->Run(vb_stream, frame_rgb, &alpha_tensor, &matte_err)) {
        if (error) *error = matte_err;
        return false;
      }

      cached_matte_valid = true;
      cached_matte_sequence = capture_sequence;
      cached_alpha_cpu_valid = false;
      return true;
    }

    // Download the alpha tensor (once per frame) for CPU-side consumers (Auto Frame / Key Light).
    bool GetAlphaCpuForFrame(const std::uint8_t* rgb,
                             std::size_t rgb_stride,
                             std::uint64_t capture_sequence,
                             int width,
                             int height,
                             const studiocast::video::effects::BroadcastCameraEffects& fx,
                             const std::vector<float>** out_alpha,
                             int* out_alpha_w,
                             int* out_alpha_h,
                             std::string* error) {
      if (error) error->clear();
      if (out_alpha) *out_alpha = nullptr;
      if (out_alpha_w) *out_alpha_w = 0;
      if (out_alpha_h) *out_alpha_h = 0;

      if (cached_alpha_cpu_valid && cached_alpha_cpu_sequence == capture_sequence) {
        if (out_alpha) *out_alpha = &alpha_cpu;
        if (pack.has_value()) {
          if (out_alpha_w) *out_alpha_w = pack->matting->input.width;
          if (out_alpha_h) *out_alpha_h = pack->matting->input.height;
        }
        return true;
      }

      // If we don't have a matte yet for this frame, compute it from the CPU input.
      if (!cached_matte_valid || cached_matte_sequence != capture_sequence) {
        std::string matte_err;
        if (!EnsureMatteForFrameCpu(rgb, rgb_stride, capture_sequence, width, height, fx, &matte_err)) {
          if (error) *error = matte_err;
          return false;
        }
      }

      if (!pack.has_value()) {
        if (error) *error = "Open CUDA: matting model pack not initialized.";
        return false;
      }

      std::string derr;
      if (!alpha_tensor.DownloadToCpuF32(&cuda, &alpha_cpu, vb_stream, &derr) ||
          !cuda.StreamSynchronize(vb_stream, &derr)) {
        if (error) *error = "Open CUDA: failed to download alpha tensor: " + derr;
        return false;
      }

      cached_alpha_cpu_valid = true;
      cached_alpha_cpu_sequence = capture_sequence;
      if (out_alpha) *out_alpha = &alpha_cpu;
      if (out_alpha_w) *out_alpha_w = pack->matting->input.width;
      if (out_alpha_h) *out_alpha_h = pack->matting->input.height;
      return true;
    }

    // GPU-only Open CUDA stage.
    //
    // - Expects input/output as GPU RGB images.
    // - Does not perform CPU<->GPU transfers.
    bool ApplyGpuStage(const studiocast::cuda::CudaImage& in_rgb,
                       studiocast::cuda::CudaImage* out_rgb_img,
                       int width,
                       int height,
                       std::uint64_t capture_sequence,
                       const studiocast::video::effects::BroadcastCameraEffects& fx,
                       std::string* error) {
      if (error) error->clear();

      using studiocast::video::effects::VirtualBackgroundMode;
      if (fx.virtual_background.mode == VirtualBackgroundMode::none) return true;

      if (!out_rgb_img) {
        if (error) *error = "Open CUDA: out_rgb is null.";
        return false;
      }
      if (!in_rgb.Valid() || in_rgb.w != width || in_rgb.h != height ||
          in_rgb.format != studiocast::cuda::PixelFormatGpu::rgb_u8) {
        if (error) *error = "Open CUDA: invalid input GPU RGB image.";
        return false;
      }

      std::string matte_err;
      if (!EnsureMatteForFrameGpu(in_rgb, capture_sequence, width, height, fx, &matte_err)) {
        if (error) *error = matte_err;
        return false;
      }

      std::string berr;
      if (!out_rgb_img->ReallocIfNeeded(&cuda, width, height, studiocast::cuda::PixelFormatGpu::rgb_u8, &berr)) {
        if (error) *error = "Open CUDA: failed to allocate output RGB image: " + berr;
        return false;
      }

      const int strength = std::max(studiocast::video::effects::contract::kVbStrengthMin,
                                    std::min(studiocast::video::effects::contract::kVbStrengthMax,
                                             fx.virtual_background.strength));

      // Resize alpha to frame size.
      std::string kerr;
      if (!studiocast::cuda::kernels::ResizeBilinearF32_1(alpha_model_view, alpha_resized, vb_stream, &kerr)) {
        if (error) *error = "Open CUDA: alpha resize failed: " + kerr;
        return false;
      }

      // Optional feathering: blur alpha with a small radius (scaled by strength).
      const int feather_radius = std::min(4, strength / 16);
      const studiocast::cuda::CudaImage* alpha_use = &alpha_resized;
      if (feather_radius > 0) {
        if (!studiocast::cuda::kernels::BoxBlurSeparableF32_1(alpha_resized,
                                                             alpha_tmp,
                                                             alpha_feather,
                                                             feather_radius,
                                                             vb_stream,
                                                             &kerr)) {
          if (error) *error = "Open CUDA: alpha feather blur failed: " + kerr;
          return false;
        }
        alpha_use = &alpha_feather;
      }

      // Build background and composite.
      if (fx.virtual_background.mode == VirtualBackgroundMode::blur) {
        if (!studiocast::cuda::kernels::BoxBlurSeparableU8x3(in_rgb, blur_tmp, blurred, /*radius=*/strength, vb_stream, &kerr)) {
          if (error) *error = "Open CUDA: background blur failed: " + kerr;
          return false;
        }
        if (!studiocast::cuda::kernels::CompositeAlphaU8x3(in_rgb, blurred, *alpha_use, *out_rgb_img, vb_stream, &kerr)) {
          if (error) *error = "Open CUDA: composite failed: " + kerr;
          return false;
        }
      } else if (fx.virtual_background.mode == VirtualBackgroundMode::remove) {
        std::uint32_t rgb_hex = 0x000000u;
        if (!ParseRgbHex(fx.virtual_background.remove_color, &rgb_hex)) {
          rgb_hex = 0x000000u;
        }
        const std::uint8_t r = static_cast<std::uint8_t>((rgb_hex >> 16) & 0xFFu);
        const std::uint8_t g = static_cast<std::uint8_t>((rgb_hex >> 8) & 0xFFu);
        const std::uint8_t b = static_cast<std::uint8_t>((rgb_hex) & 0xFFu);
        if (!studiocast::cuda::kernels::CompositeAlphaSolidU8x3(in_rgb, *alpha_use, r, g, b, *out_rgb_img, vb_stream, &kerr)) {
          if (error) *error = "Open CUDA: composite(solid) failed: " + kerr;
          return false;
        }
      } else if (fx.virtual_background.mode == VirtualBackgroundMode::replace) {
        std::string bg_err;
        if (!EnsureReplaceBackgroundGpu(width, height, fx.virtual_background.replace_path, &bg_err)) {
          if (error) *error = bg_err;
          return false;
        }
        if (!studiocast::cuda::kernels::CompositeAlphaU8x3(in_rgb, bg_rgb, *alpha_use, *out_rgb_img, vb_stream, &kerr)) {
          if (error) *error = "Open CUDA: composite(replace) failed: " + kerr;
          return false;
        }
      }

      return true;
    }

    // GPU-in / GPU-out entrypoint for Open CUDA VB.
    //
    // - Expects input/output as GPU RGB images.
    // - Does not perform CPU<->GPU transfers.
    // - Always populates DeferredGpuOut to describe the produced GPU frame.
    bool ApplyCudaRgb(const studiocast::cuda::CudaImage& in_rgb,
                      studiocast::cuda::CudaImage* out_rgb_img,
                      const studiocast::video::effects::BroadcastCameraEffects& fx,
                      std::uint64_t capture_sequence,
                      std::string* error,
                      DeferredGpuOut* deferred_out) {
      if (error) error->clear();

      using studiocast::video::effects::VirtualBackgroundMode;
      if (fx.virtual_background.mode == VirtualBackgroundMode::none) {
        if (deferred_out) {
          deferred_out->kind = DeferredGpuKind::none;
          deferred_out->nvcv_img = nullptr;
          deferred_out->nvcv = nullptr;
          deferred_out->cuda_img = nullptr;
          deferred_out->cuda = nullptr;
          deferred_out->stream = nullptr;
        }
        return true;
      }

      if (!deferred_out) {
        if (error) *error = "Open CUDA: ApplyCudaRgb requires deferred_out.";
        return false;
      }

      std::string stage_err;
      if (!ApplyGpuStage(in_rgb, out_rgb_img, in_rgb.w, in_rgb.h, capture_sequence, fx, &stage_err)) {
        if (error) *error = stage_err;
        return false;
      }

      deferred_out->kind = DeferredGpuKind::cuda_rgb;
      deferred_out->nvcv_img = nullptr;
      deferred_out->nvcv = nullptr;
      deferred_out->cuda_img = out_rgb_img;
      deferred_out->cuda = &cuda;
      deferred_out->stream = vb_stream;
      return true;
    }

    bool ApplyRgbInPlace(std::uint8_t* rgb,
                         int width,
                         int height,
                         std::size_t rgb_stride,
                         const studiocast::video::effects::BroadcastCameraEffects& fx,
                         std::string* error,
                         bool defer_readback,
                         DeferredGpuOut* deferred_out) {
      if (error) error->clear();

      using studiocast::video::effects::VirtualBackgroundMode;
      if (fx.virtual_background.mode == VirtualBackgroundMode::none) return true;
      if (!rgb || width <= 0 || height <= 0 || rgb_stride == 0) {
        if (error) *error = "Open CUDA: invalid RGB buffer.";
        return false;
      }

      std::string init_err;
      if (!EnsureInitialized(width, height, fx, &init_err)) {
        if (error) *error = init_err;
        return false;
      }

      // Upload CPU->GPU.
      std::string up_err;
      if (!frame_rgb.UploadFromCpuRgb24(&cuda, rgb, rgb_stride, vb_stream, &up_err)) {
        if (error) *error = "Open CUDA: frame upload failed: " + up_err;
        return false;
      }

      DeferredGpuOut tmp_deferred_out{};
      DeferredGpuOut* use_deferred_out = defer_readback ? deferred_out : &tmp_deferred_out;
      if (!use_deferred_out) {
        if (error) *error = "Open CUDA: defer_readback requires deferred_out.";
        return false;
      }

      std::string stage_err;
      if (!ApplyCudaRgb(frame_rgb, &out_rgb, fx, /*capture_sequence=*/0, &stage_err, use_deferred_out)) {
        if (error) *error = stage_err;
        return false;
      }

      if (defer_readback) {
        return true;
      }

      // Download GPU->CPU.
      std::string down_err;
      if (!out_rgb.DownloadToCpuRgb24(&cuda, rgb, rgb_stride, vb_stream, &down_err)) {
        if (error) *error = "Open CUDA: frame download failed: " + down_err;
        return false;
      }
      if (!cuda.StreamSynchronize(vb_stream, &down_err)) {
        if (error) *error = "Open CUDA: StreamSynchronize failed: " + down_err;
        return false;
      }
      return true;
    }
  } open_cuda_vb;

  struct OpenCudaAutoFrameContext {
    bool initialized = false;
    bool enabled = false;

    std::string last_error;
    std::string active_model_id;

    // Shared foreground matte provider (Open CUDA matting). We reuse the same matting
    // model pack and inference output as virtual background/key light when possible.
    OpenCudaVirtualBackgroundContext* matte = nullptr;
    int last_frame_w = 0;
    int last_frame_h = 0;

    std::vector<std::uint8_t> tmp_rgb;

    bool have_smoothed_crop = false;
    studiocast::maxine::effects::RectF crop_smoothed_px;
    bool last_had_detection = false;
    enum class TrackingKind {
      kNone = 0,
      kFace = 1,
      kMatte = 2,
    };
    TrackingKind last_tracking_kind = TrackingKind::kNone;

    ~OpenCudaAutoFrameContext() { Destroy(); }

    void Destroy() {
      initialized = false;
      enabled = false;
      last_error.clear();
      active_model_id.clear();
      have_smoothed_crop = false;
      last_had_detection = false;
      last_tracking_kind = TrackingKind::kNone;
      last_frame_w = 0;
      last_frame_h = 0;
    }

    static bool ComputeMatteBoxPxFromAlpha(const std::vector<float>& alpha,
                                          int alpha_w,
                                          int alpha_h,
                                          int frame_w,
                                          int frame_h,
                                          studiocast::maxine::effects::RectF* out_box_px) {
      if (!out_box_px) {
        return false;
      }
      if (alpha.empty() || alpha_w <= 0 || alpha_h <= 0) {
        return false;
      }

      // Threshold the matte and compute a tight bounding box around the foreground.
      constexpr float kAlphaThreshold = 0.35f;
      int min_x = alpha_w;
      int min_y = alpha_h;
      int max_x = -1;
      int max_y = -1;
      int count = 0;
      for (int y = 0; y < alpha_h; ++y) {
        const float* row = alpha.data() + (static_cast<size_t>(y) * static_cast<size_t>(alpha_w));
        for (int x = 0; x < alpha_w; ++x) {
          if (row[x] > kAlphaThreshold) {
            min_x = std::min(min_x, x);
            min_y = std::min(min_y, y);
            max_x = std::max(max_x, x);
            max_y = std::max(max_y, y);
            ++count;
          }
        }
      }

      // Reject tiny detections to avoid jitter when the matte is effectively empty.
      const int min_pixels = std::max(32, (alpha_w * alpha_h) / 500);  // ~0.2%
      if (count < min_pixels || max_x < min_x || max_y < min_y) {
        return false;
      }

      const float sx = static_cast<float>(frame_w) / static_cast<float>(alpha_w);
      const float sy = static_cast<float>(frame_h) / static_cast<float>(alpha_h);

      const float x0 = min_x * sx;
      const float y0 = min_y * sy;
      const float x1 = (max_x + 1) * sx;
      const float y1 = (max_y + 1) * sy;

      out_box_px->x = x0;
      out_box_px->y = y0;
      out_box_px->w = x1 - x0;
      out_box_px->h = y1 - y0;
      return true;
    }

    static void CropResizeRgb24Bilinear(const std::uint8_t* src,
                                        int src_w,
                                        int src_h,
                                        size_t src_stride,
                                        std::uint8_t* dst,
                                        int dst_w,
                                        int dst_h,
                                        size_t dst_stride,
                                        float crop_x,
                                        float crop_y,
                                        float crop_w,
                                        float crop_h) {
      if (!src || !dst) {
        return;
      }
      if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) {
        return;
      }
      crop_w = std::max(1.0f, crop_w);
      crop_h = std::max(1.0f, crop_h);

      const float scale_x = crop_w / static_cast<float>(dst_w);
      const float scale_y = crop_h / static_cast<float>(dst_h);

      for (int y = 0; y < dst_h; ++y) {
        const float src_y = crop_y + (static_cast<float>(y) + 0.5f) * scale_y - 0.5f;
        const float sy = std::clamp(src_y, 0.0f, static_cast<float>(src_h - 1));
        const int y0 = static_cast<int>(sy);
        const int y1 = std::min(y0 + 1, src_h - 1);
        const float ty = sy - static_cast<float>(y0);

        const std::uint8_t* row0 = src + static_cast<size_t>(y0) * src_stride;
        const std::uint8_t* row1 = src + static_cast<size_t>(y1) * src_stride;
        std::uint8_t* out = dst + static_cast<size_t>(y) * dst_stride;

        for (int x = 0; x < dst_w; ++x) {
          const float src_x = crop_x + (static_cast<float>(x) + 0.5f) * scale_x - 0.5f;
          const float sx = std::clamp(src_x, 0.0f, static_cast<float>(src_w - 1));
          const int x0 = static_cast<int>(sx);
          const int x1 = std::min(x0 + 1, src_w - 1);
          const float tx = sx - static_cast<float>(x0);

          const std::uint8_t* p00 = row0 + x0 * 3;
          const std::uint8_t* p01 = row0 + x1 * 3;
          const std::uint8_t* p10 = row1 + x0 * 3;
          const std::uint8_t* p11 = row1 + x1 * 3;

          for (int c = 0; c < 3; ++c) {
            const float v00 = static_cast<float>(p00[c]);
            const float v01 = static_cast<float>(p01[c]);
            const float v10 = static_cast<float>(p10[c]);
            const float v11 = static_cast<float>(p11[c]);

            const float v0 = v00 + (v01 - v00) * tx;
            const float v1 = v10 + (v11 - v10) * tx;
            const float v = v0 + (v1 - v0) * ty;

            out[x * 3 + c] = static_cast<std::uint8_t>(std::clamp(v, 0.0f, 255.0f));
          }
        }
      }
    }

    bool EnsureInitialized(int frame_w,
                           int frame_h,
                           const studiocast::video::effects::BroadcastCameraEffects& fx,
                           bool require_matte_tracking,
                           std::string* error) {
      if (error) error->clear();

      if (frame_w <= 0 || frame_h <= 0) {
        last_error = "Open CUDA Auto Frame: invalid frame size.";
        if (error) *error = last_error;
        return false;
      }
      std::string current_model_id;
      if (require_matte_tracking) {
        if (!matte) {
          last_error = "Open CUDA Auto Frame: matte provider not set.";
          if (error) *error = last_error;
          return false;
        }

        // Ensure the shared matting model/session exists, but don't allocate the heavy
        // virtual-background buffers if the VB stage isn't scheduled.
        std::string matte_init_err;
        if (!matte->EnsureInitialized(frame_w, frame_h, fx, &matte_init_err, /*require_vb_buffers=*/false)) {
          last_error = "Open CUDA Auto Frame: " + matte_init_err;
          if (error) *error = last_error;
          return false;
        }
        current_model_id = matte->active_model_id;
      }

      // Track model/frame changes to reset smoothing state.
      if (initialized && enabled && current_model_id == active_model_id && last_frame_w == frame_w && last_frame_h == frame_h) {
        return true;
      }

      have_smoothed_crop = false;
      last_had_detection = false;
      last_tracking_kind = TrackingKind::kNone;
      active_model_id = current_model_id;
      last_frame_w = frame_w;
      last_frame_h = frame_h;

      initialized = true;
      enabled = true;
      return true;
    }

    bool ApplyRgbInPlace(std::uint64_t capture_sequence,
                         std::uint8_t* rgb,
                         int width,
                         int height,
                         size_t stride,
                         const studiocast::video::effects::BroadcastCameraEffects& fx,
                         const std::vector<studiocast::open_video::FaceDetection>* face_detections,
                         std::string* error) {
      if (!fx.auto_frame.enabled) {
        return true;
      }
      if (!rgb) {
        if (error) {
          *error = "Open CUDA Auto Frame: null RGB buffer.";
        }
        return false;
      }

      const bool have_faces = (face_detections && !face_detections->empty());

      std::string err;
      if (!EnsureInitialized(width, height, fx, /*require_matte_tracking=*/!have_faces, &err)) {
        // Best-effort: if we can't initialize tracking, just bypass auto frame.
        if (error && !err.empty()) {
          *error = err;
        }
        return true;
      }

      studiocast::maxine::effects::RectF box_px;
      bool found = false;
      TrackingKind kind = TrackingKind::kNone;

      if (have_faces) {
        // Pick the largest detected face (most stable for framing).
        const studiocast::open_video::FaceDetection* best = &face_detections->front();
        float best_area = best->w * best->h;
        for (const auto& f : *face_detections) {
          const float a = f.w * f.h;
          if (a > best_area) {
            best = &f;
            best_area = a;
          }
        }
        // Expand the face box to approximate "upper body" framing.
        const float fx0 = best->x;
        const float fy0 = best->y;
        const float fw = best->w;
        const float fh = best->h;
        const float cx = fx0 + fw * 0.5f;

        const float expand_w = fw * 1.8f;
        const float expand_h = fh * 2.6f;
        const float new_x = cx - expand_w * 0.5f;
        const float new_y = fy0 - fh * 0.4f;  // headroom

        box_px = studiocast::maxine::effects::RectF{new_x, new_y, expand_w, expand_h};
        found = true;
        kind = TrackingKind::kFace;
      } else if (matte) {
        const std::vector<float>* alpha_cpu = nullptr;
        int alpha_w = 0;
        int alpha_h = 0;
        std::string alpha_err;
        if (matte->GetAlphaCpuForFrame(rgb,
                                       stride,
                                       capture_sequence,
                                       width,
                                       height,
                                       fx,
                                       &alpha_cpu,
                                       &alpha_w,
                                       &alpha_h,
                                       &alpha_err) &&
            alpha_cpu) {
          found = ComputeMatteBoxPxFromAlpha(*alpha_cpu,
                                             alpha_w,
                                             alpha_h,
                                             width,
                                             height,
                                             &box_px);
          kind = found ? TrackingKind::kMatte : TrackingKind::kNone;
        } else {
          if (error && !alpha_err.empty()) {
            *error = "Open CUDA Auto Frame: " + alpha_err;
          }
          return true;
        }
      }

      if (kind != last_tracking_kind) {
        have_smoothed_crop = false;
        last_tracking_kind = kind;
      }

      // Use the same crop math as the Maxine auto-frame tracker for consistency.
      studiocast::maxine::effects::AutoFrameKnobs knobs;
      knobs.strength = fx.auto_frame.strength;
      knobs.smoothing = fx.auto_frame.smoothing;
      knobs.headroom = fx.auto_frame.headroom;

      const float out_aspect = static_cast<float>(width) / static_cast<float>(height);
      studiocast::maxine::effects::RectF target_crop = {};
      if (found) {
        target_crop = studiocast::maxine::effects::ArAutoFrameTracker::ComputeTargetCropFromBoxPx(
            box_px, width, height, out_aspect, knobs);
      } else {
        const float strength01 = std::clamp(knobs.strength / 100.0f, 0.0f, 1.0f);
        const float zoom = 1.0f + strength01 * 0.5f;  // up to ~1.5x
        target_crop = studiocast::maxine::effects::ArAutoFrameTracker::CenterCrop(width, height, out_aspect, zoom);
      }

      auto clamp_crop = [&](studiocast::maxine::effects::RectF* r) {
        if (!r) {
          return;
        }
        r->w = std::clamp(r->w, 1.0f, static_cast<float>(width));
        r->h = std::clamp(r->h, 1.0f, static_cast<float>(height));
        r->x = std::clamp(r->x, 0.0f, static_cast<float>(width) - r->w);
        r->y = std::clamp(r->y, 0.0f, static_cast<float>(height) - r->h);
      };

      if (!have_smoothed_crop) {
        crop_smoothed_px = target_crop;
        have_smoothed_crop = true;
      } else {
        const float a = studiocast::maxine::effects::ArAutoFrameTracker::SmoothingAlpha(knobs.smoothing);
        crop_smoothed_px = studiocast::maxine::effects::ArAutoFrameTracker::Lerp(crop_smoothed_px, target_crop, a);
      }
      clamp_crop(&crop_smoothed_px);
      last_had_detection = found;

      // Early-out if crop is effectively full-frame.
      constexpr float kEpsPx = 0.5f;
      if (std::abs(crop_smoothed_px.x) <= kEpsPx && std::abs(crop_smoothed_px.y) <= kEpsPx &&
          std::abs(crop_smoothed_px.w - width) <= kEpsPx && std::abs(crop_smoothed_px.h - height) <= kEpsPx) {
        return true;
      }

      const size_t min_row_bytes = static_cast<size_t>(width) * 3;
      if (stride < min_row_bytes) {
        if (error) {
          *error = "Open CUDA Auto Frame: unexpected RGB stride.";
        }
        return false;
      }

      tmp_rgb.resize(static_cast<size_t>(height) * stride);
      CropResizeRgb24Bilinear(rgb,
                              width,
                              height,
                              stride,
                              tmp_rgb.data(),
                              width,
                              height,
                              stride,
                              crop_smoothed_px.x,
                              crop_smoothed_px.y,
                              crop_smoothed_px.w,
                              crop_smoothed_px.h);

      for (int y = 0; y < height; ++y) {
        std::memcpy(rgb + (static_cast<size_t>(y) * stride),
                    tmp_rgb.data() + (static_cast<size_t>(y) * stride),
                    min_row_bytes);
      }

      return true;
    }
  } open_cuda_auto_frame;

  // Open-source (Open CUDA) Virtual Key Light.
  //
  // Implementation approach:
  //  - Use the same Open CUDA matting model packs (foreground matte) used by virtual background.
  //  - Apply a lightweight, masked "key light" as an RGB interpolation toward a tinted
  //    highlight color on the CPU.
  //
  // This keeps dependencies minimal (no custom CUDA kernels/ptx updates) while still
  // producing a Broadcast-like "lift" on the subject.
  struct OpenCudaKeyLightContext {
    bool initialized = false;
    bool enabled = false;
    std::string last_error;
    std::string active_model_id;
    OpenCudaVirtualBackgroundContext* matte = nullptr;  // shared Open CUDA matting + cache
    int last_frame_w = 0;
    int last_frame_h = 0;

    void Destroy() {
      initialized = false;
      enabled = false;
      last_error.clear();
      active_model_id.clear();
      last_frame_w = 0;
      last_frame_h = 0;
    }

    static void ResolveTargetColorFromTemperaturePreset(int preset,
                                                        float* out_r,
                                                        float* out_g,
                                                        float* out_b) {
      // Simple perceptual targets (sRGB-ish). These are not physically accurate; they are
      // tuned to "feel" like Broadcast's warm/cool key light in a subtle way.
      // preset: 0=neutral, 1=warm, 2=cool
      float r = 255.0f;
      float g = 255.0f;
      float b = 255.0f;
      if (preset == 1) {
        // Warm.
        r = 255.0f;
        g = 242.0f;
        b = 228.0f;
      } else if (preset == 2) {
        // Cool.
        r = 228.0f;
        g = 242.0f;
        b = 255.0f;
      }
      if (out_r) *out_r = r;
      if (out_g) *out_g = g;
      if (out_b) *out_b = b;
    }

    bool EnsureInitialized(int frame_w,
                           int frame_h,
                           const studiocast::video::effects::BroadcastCameraEffects& fx,
                           std::string* error) {
      if (error) error->clear();

      if (frame_w <= 0 || frame_h <= 0) {
        last_error = "Open CUDA Virtual Key Light: invalid frame size.";
        if (error) *error = last_error;
        return false;
      }
      if (!matte) {
        last_error = "Open CUDA Virtual Key Light: matte provider not set.";
        if (error) *error = last_error;
        return false;
      }

      // Ensure the shared matting model/session exists, but don't allocate the heavy
      // virtual-background buffers if the VB stage isn't scheduled.
      std::string matte_init_err;
      if (!matte->EnsureInitialized(frame_w, frame_h, fx, &matte_init_err, /*require_vb_buffers=*/false)) {
        last_error = "Open CUDA Virtual Key Light: " + matte_init_err;
        if (error) *error = last_error;
        return false;
      }

      const std::string current_model_id = matte->active_model_id;
      if (initialized && enabled && current_model_id == active_model_id && last_frame_w == frame_w && last_frame_h == frame_h) {
        return true;
      }

      active_model_id = current_model_id;
      last_frame_w = frame_w;
      last_frame_h = frame_h;
      initialized = true;
      enabled = true;
      return true;
    }

    bool ApplyRgbInPlace(std::uint64_t capture_sequence,
                         std::uint8_t* rgb,
                         int width,
                         int height,
                         size_t stride,
                         const studiocast::video::effects::BroadcastCameraEffects& fx,
                         std::string* error) {
      if (!fx.virtual_key_light.enabled) {
        return true;
      }
      if (!rgb) {
        if (error) {
          *error = "Open CUDA Virtual Key Light: null RGB buffer.";
        }
        return false;
      }

      const float intensity01 = std::clamp(fx.virtual_key_light.intensity / 100.0f, 0.0f, 1.0f);
      if (intensity01 <= 0.0001f) {
        return true;
      }

      std::string err;
      if (!EnsureInitialized(width, height, fx, &err)) {
        if (error) *error = err;
        return false;
      }
      if (!matte) {
        if (error) *error = "Open CUDA Virtual Key Light: matte provider not set.";
        return false;
      }

      const std::vector<float>* alpha_cpu = nullptr;
      int alpha_w = 0;
      int alpha_h = 0;
      std::string alpha_err;
      if (!matte->GetAlphaCpuForFrame(rgb,
                                      stride,
                                      capture_sequence,
                                      width,
                                      height,
                                      fx,
                                      &alpha_cpu,
                                      &alpha_w,
                                      &alpha_h,
                                      &alpha_err)) {
        if (error) {
          *error = "Open CUDA Virtual Key Light: " + alpha_err;
        }
        return false;
      }
      if (!alpha_cpu) {
        if (error) {
          *error = "Open CUDA Virtual Key Light: alpha CPU buffer is null.";
        }
        return false;
      }

      // Compute a matte bounding box so we only process pixels near the subject.
      studiocast::maxine::effects::RectF box_px;
      const bool found = OpenCudaAutoFrameContext::ComputeMatteBoxPxFromAlpha(*alpha_cpu,
                                                                              alpha_w,
                                                                              alpha_h,
                                                                              width,
                                                                              height,
                                                                              &box_px);
      if (!found) {
        return true;  // no subject detected
      }

      // Expand ROI slightly.
      const float margin_x = std::max(16.0f, box_px.w * 0.15f);
      const float margin_y = std::max(16.0f, box_px.h * 0.15f);
      float x0f = std::clamp(box_px.x - margin_x, 0.0f, static_cast<float>(width));
      float y0f = std::clamp(box_px.y - margin_y, 0.0f, static_cast<float>(height));
      float x1f = std::clamp(box_px.x + box_px.w + margin_x, 0.0f, static_cast<float>(width));
      float y1f = std::clamp(box_px.y + box_px.h + margin_y, 0.0f, static_cast<float>(height));
      const int x0 = static_cast<int>(std::floor(x0f));
      const int y0 = static_cast<int>(std::floor(y0f));
      const int x1 = static_cast<int>(std::ceil(x1f));
      const int y1 = static_cast<int>(std::ceil(y1f));

      // Directional weighting (left/right).
      const float pan = std::clamp(static_cast<float>(fx.virtual_key_light.direction_pan_degrees), -90.0f, 90.0f);
      const float dir = std::clamp(pan / 90.0f, -1.0f, 1.0f);
      const float cx = static_cast<float>(width) * 0.5f;
      const float inv_cx = (cx > 1.0f) ? (1.0f / cx) : 0.0f;

      float target_r = 255.0f, target_g = 255.0f, target_b = 255.0f;
      ResolveTargetColorFromTemperaturePreset(fx.virtual_key_light.temperature_preset, &target_r, &target_g, &target_b);

      // Nearest-neighbor matte sampling using fixed-point mapping.
      if (alpha_w <= 0 || alpha_h <= 0 || alpha_cpu->empty()) {
        return true;
      }

      const int step_x_fp = (alpha_w << 16) / std::max(1, width);
      const int step_y_fp = (alpha_h << 16) / std::max(1, height);
      int ay_fp = y0 * step_y_fp;
      for (int y = y0; y < y1; ++y) {
        const int ay = std::clamp(ay_fp >> 16, 0, alpha_h - 1);
        const float* alpha_row = alpha_cpu->data() + static_cast<size_t>(ay) * static_cast<size_t>(alpha_w);
        std::uint8_t* row = rgb + static_cast<size_t>(y) * stride;
        int ax_fp = x0 * step_x_fp;
        for (int x = x0; x < x1; ++x) {
          const int ax = std::clamp(ax_fp >> 16, 0, alpha_w - 1);
          const float a = std::clamp(alpha_row[ax], 0.0f, 1.0f);
          if (a > 0.02f) {
            // Compute per-pixel blend factor.
            const float x_norm = (static_cast<float>(x) - cx) * inv_cx;  // [-1..1]
            float field = 1.0f + dir * x_norm * 0.35f;
            field = std::clamp(field, 0.65f, 1.35f);

            float t = intensity01 * a * field;
            t = std::clamp(t, 0.0f, 1.0f);

            std::uint8_t* px = row + x * 3;
            const float in_r = static_cast<float>(px[0]);
            const float in_g = static_cast<float>(px[1]);
            const float in_b = static_cast<float>(px[2]);

            const float out_r = in_r + (target_r - in_r) * t;
            const float out_g = in_g + (target_g - in_g) * t;
            const float out_b = in_b + (target_b - in_b) * t;

            px[0] = static_cast<std::uint8_t>(std::clamp(out_r, 0.0f, 255.0f));
            px[1] = static_cast<std::uint8_t>(std::clamp(out_g, 0.0f, 255.0f));
            px[2] = static_cast<std::uint8_t>(std::clamp(out_b, 0.0f, 255.0f));
          }
          ax_fp += step_x_fp;
        }
        ay_fp += step_y_fp;
      }

      return true;
    }
  } open_cuda_key_light;

  // Share the same Open CUDA matting model/session (and per-frame matte cache) across
  // all open-source video effects that need a foreground matte.
  // This avoids re-running the matting network multiple times per frame.
  //
  // NOTE: The provider lives in open_cuda_vb even if the virtual background stage is not
  // scheduled; other effects request matte-only initialization.
  open_cuda_auto_frame.matte = &open_cuda_vb;
  open_cuda_key_light.matte = &open_cuda_vb;

  // Open Video analysis cache (per-frame) and face detection (YuNet).
  //
  // The cache avoids duplicated ML inference when multiple effects need the same
  // frame-level analysis.
  studiocast::open_video::FrameAnalysisCache open_video_cache;
  studiocast::open_video::YunetFaceDetector open_video_yunet;
  studiocast::open_video::FastDvdnetDenoiser open_video_fastdvdnet;
  studiocast::open_video::GazeCorrectionEyeContact open_video_eye_contact;

  // Open-source Video Noise Removal (Open CUDA backend). This implementation is a lightweight
  // temporal denoiser intended to be real-time and dependency-free. It does not require model packs.
  struct OpenCudaVideoNoiseRemovalContext {
    bool last_enabled = false;
    int prev_w = 0;
    int prev_h = 0;
    size_t prev_stride = 0;
    std::vector<uint8_t> prev_rgb;

    void Reset() {
      last_enabled = false;
      prev_w = 0;
      prev_h = 0;
      prev_stride = 0;
      prev_rgb.clear();
    }

    bool ApplyRgbInPlace(uint8_t* rgb,
                         int width,
                         int height,
                         size_t stride,
                         const video::effects::BroadcastCameraEffects& fx,
                         std::string* error_out) {
      (void)error_out;

      const bool enabled = fx.video_noise_removal.enabled;
      const int strength = std::clamp(fx.video_noise_removal.strength, 0, 100);

      if (!enabled || strength <= 0) {
        // When toggled off, don't keep stale temporal state.
        last_enabled = false;
        return true;
      }

      // (Re)initialize on first frame after enable, or when the image geometry changes.
      const size_t bytes = static_cast<size_t>(height) * stride;
      if (!last_enabled || prev_rgb.size() != bytes || width != prev_w || height != prev_h || stride != prev_stride) {
        prev_w = width;
        prev_h = height;
        prev_stride = stride;
        prev_rgb.assign(rgb, rgb + bytes);
        last_enabled = true;
        return true;
      }

      last_enabled = true;

      // Strength -> smoothing weight and motion threshold.
      // We use a squared curve for weight so low strengths do something subtle.
      const float t = static_cast<float>(strength) / 100.0f;
      const float t2 = t * t;
      const int w_prev = std::clamp(static_cast<int>(t2 * 220.0f + 0.5f), 0, 255);
      const int w_curr = 256 - w_prev;

      // Motion threshold: lower threshold at higher strengths to reduce ghosting.
      const int thr_chan = std::clamp(static_cast<int>(20.0f - t * 12.0f + 0.5f), 6, 24);
      const int thr_sum = thr_chan * 3;

      uint8_t* prev = prev_rgb.data();
      for (int y = 0; y < height; ++y) {
        uint8_t* row = rgb + static_cast<size_t>(y) * stride;
        uint8_t* prow = prev + static_cast<size_t>(y) * stride;
        for (int x = 0; x < width; ++x) {
          const int i = x * 3;

          const int cr = row[i + 0];
          const int cg = row[i + 1];
          const int cb = row[i + 2];

          const int pr = prow[i + 0];
          const int pg = prow[i + 1];
          const int pb = prow[i + 2];

          const int diff = std::abs(cr - pr) + std::abs(cg - pg) + std::abs(cb - pb);
          if (diff > thr_sum || w_prev == 0) {
            // Motion (or effectively disabled smoothing): take current and refresh temporal state.
            prow[i + 0] = static_cast<uint8_t>(cr);
            prow[i + 1] = static_cast<uint8_t>(cg);
            prow[i + 2] = static_cast<uint8_t>(cb);
            continue;
          }

          const int or_ = (cr * w_curr + pr * w_prev + 128) >> 8;
          const int og_ = (cg * w_curr + pg * w_prev + 128) >> 8;
          const int ob_ = (cb * w_curr + pb * w_prev + 128) >> 8;

          row[i + 0] = static_cast<uint8_t>(or_);
          row[i + 1] = static_cast<uint8_t>(og_);
          row[i + 2] = static_cast<uint8_t>(ob_);

          // Store filtered output as the next frame's reference.
          prow[i + 0] = row[i + 0];
          prow[i + 1] = row[i + 1];
          prow[i + 2] = row[i + 2];
        }
      }

      return true;
    }
  } open_cuda_video_denoise;

  struct MaxineRelightingContext {
    bool initialized = false;
    std::string last_error;

    studiocast::maxine::vfx::VfxApi vfx;
    studiocast::maxine::NvcvApi nvcv;
    studiocast::maxine::CudaDriverApi cuda;
    studiocast::maxine::CudaBgrVignette vignette;

    std::filesystem::path model_dir;
    std::filesystem::path features_dir;

    std::unique_ptr<studiocast::maxine::effects::VfxGreenScreenEffect> greenscreen;
    std::unique_ptr<studiocast::maxine::effects::VfxRelightingEffect> relight;

    std::vector<std::uint8_t> bgr_in;
    std::vector<std::uint8_t> bgr_out;
    studiocast::maxine::NvCVImage cpu_bgr_in{};
    studiocast::maxine::NvCVImage cpu_bgr_out{};

    studiocast::maxine::NvCVImage gpu_bgr{};
    bool gpu_bgr_allocated = false;

    studiocast::maxine::NvCVImage gpu_bgr_out_img{};
    bool gpu_bgr_out_allocated = false;

    studiocast::maxine::NvCVImage gpu_matte_scaled{};
    bool gpu_matte_scaled_allocated = false;

    int cached_hdri_preset = -1;
    std::filesystem::path cached_hdri_path;

    ~MaxineRelightingContext() { Destroy(); }

    void Destroy() {
      greenscreen.reset();
      relight.reset();

      if (gpu_matte_scaled_allocated && nvcv.IsInitialized() && nvcv.f().NvCVImage_Dealloc) {
        (void)nvcv.f().NvCVImage_Dealloc(&gpu_matte_scaled);
      }
      gpu_matte_scaled = studiocast::maxine::NvCVImage{};
      gpu_matte_scaled_allocated = false;

      if (gpu_bgr_out_allocated && nvcv.IsInitialized() && nvcv.f().NvCVImage_Dealloc) {
        (void)nvcv.f().NvCVImage_Dealloc(&gpu_bgr_out_img);
      }
      gpu_bgr_out_img = studiocast::maxine::NvCVImage{};
      gpu_bgr_out_allocated = false;

      if (gpu_bgr_allocated && nvcv.IsInitialized() && nvcv.f().NvCVImage_Dealloc) {
        (void)nvcv.f().NvCVImage_Dealloc(&gpu_bgr);
      }
      gpu_bgr = studiocast::maxine::NvCVImage{};
      gpu_bgr_allocated = false;

      bgr_in.clear();
      bgr_out.clear();
      cpu_bgr_in = studiocast::maxine::NvCVImage{};
      cpu_bgr_out = studiocast::maxine::NvCVImage{};

      cached_hdri_preset = -1;
      cached_hdri_path.clear();

      initialized = false;
    }

    static std::filesystem::path InferModelsDirFromLibrary(const std::filesystem::path& lib) {
      std::error_code ec;
      std::filesystem::path p = lib.parent_path();
      for (int i = 0; i < 5 && !p.empty(); ++i) {
        const auto cand = p / "models";
        if (std::filesystem::exists(cand, ec) && std::filesystem::is_directory(cand, ec)) {
          return cand;
        }
        p = p.parent_path();
      }
      return {};
    }

    static std::filesystem::path InferFeaturesDirFromLibrary(const std::filesystem::path& lib) {
      std::error_code ec;
      std::filesystem::path p = lib.parent_path();
      for (int i = 0; i < 5 && !p.empty(); ++i) {
        const auto cand = p / "features";
        if (std::filesystem::exists(cand, ec) && std::filesystem::is_directory(cand, ec)) {
          return cand;
        }
        p = p.parent_path();
      }
      return {};
    }

    static int ScoreHdriName(const std::string& nameLower, int preset) {
      // preset: 0 neutral, 1 warm, 2 cool
      int score = 0;
      if (preset == 1) {
        if (nameLower.find("warm") != std::string::npos) score += 10;
        if (nameLower.find("sun") != std::string::npos) score += 3;
      } else if (preset == 2) {
        if (nameLower.find("cool") != std::string::npos) score += 10;
        if (nameLower.find("blue") != std::string::npos) score += 3;
      } else {
        if (nameLower.find("neutral") != std::string::npos) score += 10;
        if (nameLower.find("studio") != std::string::npos) score += 3;
        if (nameLower.find("white") != std::string::npos) score += 2;
      }
      // Prefer "hdri" named files in general.
      if (nameLower.find("hdri") != std::string::npos) score += 1;
      return score;
    }

    static std::filesystem::path FindDefaultHdriInDir(const std::filesystem::path& dir, int preset) {
      if (dir.empty()) return {};

      std::error_code ec;
      if (!std::filesystem::exists(dir, ec) || !std::filesystem::is_directory(dir, ec)) return {};

      std::filesystem::path best;
      int bestScore = -9999;

      std::size_t visited = 0;
      for (std::filesystem::recursive_directory_iterator it(dir, ec), end; it != end && !ec; ++it) {
        if (it.depth() > 5) {
          it.disable_recursion_pending();
          continue;
        }
        if (++visited > 2000) break;
        if (!it->is_regular_file(ec)) continue;

        const auto ext = ToLowerAscii(it->path().extension().string());
        if (ext != ".hdr" && ext != ".exr") continue;

        const auto nameLower = ToLowerAscii(it->path().filename().string());
        const int sc = ScoreHdriName(nameLower, preset);
        if (sc > bestScore) {
          bestScore = sc;
          best = it->path();
        }
      }

      return best;
    }

    bool ResolveHdriPath(const studiocast::video::effects::BroadcastCameraEffects& fx,
                         std::filesystem::path* out,
                         std::string* error) {
      if (!out) return false;
      out->clear();

      // User override.
      if (!fx.virtual_key_light.hdri_path.empty()) {
        std::error_code ec;
        const std::filesystem::path p = fx.virtual_key_light.hdri_path;
        if (std::filesystem::exists(p, ec)) {
          *out = p;
          return true;
        }
        if (error) *error = "Virtual Key Light HDRI path does not exist: " + p.string();
        return false;
      }

      const int preset = fx.virtual_key_light.temperature_preset;
      if (cached_hdri_preset == preset && !cached_hdri_path.empty()) {
        *out = cached_hdri_path;
        return true;
      }

      // Prefer HDRI assets installed by the relighting feature (if present), otherwise search models.
      auto hdri = FindDefaultHdriInDir(features_dir, preset);
      if (hdri.empty()) {
        hdri = FindDefaultHdriInDir(model_dir, preset);
      }

      if (hdri.empty()) {
        if (error) {
          *error = "No default HDRI found for relighting. Provide an HDRI via video.virtual_key_light_hdri (daemon.conf) "
                   "or install the VFX relighting feature assets (install_feature.sh for relighting).";
        }
        return false;
      }

      cached_hdri_preset = preset;
      cached_hdri_path = hdri;
      *out = hdri;
      return true;
    }

    bool EnsureInitialized(int width,
                           int height,
                           const studiocast::video::effects::BroadcastCameraEffects& fx,
                           std::string* error) {
      last_error.clear();

      if (initialized) {
        // Live reconfigure.
        std::filesystem::path hdri;
        std::string hdri_err;
        if (!ResolveHdriPath(fx, &hdri, &hdri_err)) {
          last_error = hdri_err;
          if (error) *error = last_error;
          return false;
        }

        auto tmp = fx;
        tmp.virtual_key_light.hdri_path = hdri.string();

        std::string cfg_err;
        if (greenscreen && !greenscreen->Configure(tmp, &cfg_err)) {
          if (error) *error = cfg_err;
          return false;
        }
        if (relight && !relight->Configure(tmp, &cfg_err)) {
          if (error) *error = cfg_err;
          return false;
        }
        return true;
      }

      std::string err;
      if (!vfx.Initialize(&err)) {
        last_error = "Maxine VFX runtime unavailable: " + err;
        if (error) *error = last_error;
        return false;
      }
      if (!nvcv.Initialize(studiocast::maxine::NvcvApi::Requirement::VfxCompat, &err)) {
        last_error = "NvCVImage runtime unavailable: " + err;
        if (error) *error = last_error;
        return false;
      }

      {
        std::string cuda_err;
        if (!cuda.Initialize(&cuda_err)) {
          last_error = "CUDA driver API unavailable: " + cuda_err;
          if (error) *error = last_error;
          return false;
        }
        std::string vig_err;
        if (!vignette.Initialize(&cuda, &vig_err)) {
          last_error = "CUDA vignette init failed: " + vig_err;
          if (error) *error = last_error;
          return false;
        }
      }
      if (model_dir.empty()) {
        model_dir = InferModelsDirFromLibrary(vfx.library_path());
      }
      if (features_dir.empty()) {
        features_dir = InferFeaturesDirFromLibrary(vfx.library_path());
      }

      std::filesystem::path hdri;
      std::string hdri_err;
      if (!ResolveHdriPath(fx, &hdri, &hdri_err)) {
        last_error = hdri_err;
        if (error) *error = last_error;
        return false;
      }

      // Allocate CPU-side BGR staging buffers and wrap with NvCVImage_Init.
      const std::size_t stride = static_cast<std::size_t>(width) * 3u;
      bgr_in.resize(stride * static_cast<std::size_t>(height));
      bgr_out.resize(stride * static_cast<std::size_t>(height));

      if (!nvcv.f().NvCVImage_Init) {
        last_error = "NvCVImage_Init missing from NvCVImage runtime.";
        if (error) *error = last_error;
        return false;
      }

      auto st = nvcv.f().NvCVImage_Init(&cpu_bgr_in,
                                       static_cast<unsigned>(width),
                                       static_cast<unsigned>(height),
                                       static_cast<int>(stride),
                                       bgr_in.data(),
                                       studiocast::maxine::NVCV_BGR,
                                       studiocast::maxine::NVCV_U8,
                                       studiocast::maxine::NVCV_CHUNKY,
                                       studiocast::maxine::NVCV_CPU);
      if (st != studiocast::maxine::NVCV_SUCCESS) {
        last_error = "NvCVImage_Init(cpu BGR in) failed: " + std::to_string(st);
        if (error) *error = last_error;
        return false;
      }

      st = nvcv.f().NvCVImage_Init(&cpu_bgr_out,
                                  static_cast<unsigned>(width),
                                  static_cast<unsigned>(height),
                                  static_cast<int>(stride),
                                  bgr_out.data(),
                                  studiocast::maxine::NVCV_BGR,
                                  studiocast::maxine::NVCV_U8,
                                  studiocast::maxine::NVCV_CHUNKY,
                                  studiocast::maxine::NVCV_CPU);
      if (st != studiocast::maxine::NVCV_SUCCESS) {
        last_error = "NvCVImage_Init(cpu BGR out) failed: " + std::to_string(st);
        if (error) *error = last_error;
        return false;
      }

      // Allocate GPU input, output, and matte-scaled images.
      st = nvcv.f().NvCVImage_Alloc(&gpu_bgr,
                                   static_cast<unsigned>(width),
                                   static_cast<unsigned>(height),
                                   studiocast::maxine::NVCV_BGR,
                                   studiocast::maxine::NVCV_U8,
                                   studiocast::maxine::NVCV_CHUNKY,
                                   studiocast::maxine::NVCV_GPU,
                                   /*alignment=*/0);
      if (st != studiocast::maxine::NVCV_SUCCESS) {
        last_error = "NvCVImage_Alloc(gpu BGR) failed: " + std::to_string(st);
        if (error) *error = last_error;
        return false;
      }
      gpu_bgr_allocated = true;

      st = nvcv.f().NvCVImage_Alloc(&gpu_bgr_out_img,
                                   static_cast<unsigned>(width),
                                   static_cast<unsigned>(height),
                                   studiocast::maxine::NVCV_BGR,
                                   studiocast::maxine::NVCV_U8,
                                   studiocast::maxine::NVCV_CHUNKY,
                                   studiocast::maxine::NVCV_GPU,
                                   /*alignment=*/0);
      if (st != studiocast::maxine::NVCV_SUCCESS) {
        last_error = "NvCVImage_Alloc(gpu BGR out) failed: " + std::to_string(st);
        if (error) *error = last_error;
        return false;
      }
      gpu_bgr_out_allocated = true;

      st = nvcv.f().NvCVImage_Alloc(&gpu_matte_scaled,
                                   static_cast<unsigned>(width),
                                   static_cast<unsigned>(height),
                                   studiocast::maxine::NVCV_A,
                                   studiocast::maxine::NVCV_U8,
                                   studiocast::maxine::NVCV_CHUNKY,
                                   studiocast::maxine::NVCV_GPU,
                                   /*alignment=*/0);
      if (st != studiocast::maxine::NVCV_SUCCESS) {
        last_error = "NvCVImage_Alloc(gpu matte scaled) failed: " + std::to_string(st);
        if (error) *error = last_error;
        return false;
      }
      gpu_matte_scaled_allocated = true;

      greenscreen = std::make_unique<studiocast::maxine::effects::VfxGreenScreenEffect>(&vfx, &nvcv, model_dir);
      relight = std::make_unique<studiocast::maxine::effects::VfxRelightingEffect>(&vfx, &nvcv, model_dir);

      auto tmp = fx;
      tmp.virtual_key_light.hdri_path = hdri.string();

      if (!greenscreen->Configure(tmp, &err) || !relight->Configure(tmp, &err)) {
        last_error = "Maxine effect Configure failed: " + err;
        if (error) *error = last_error;
        Destroy();
        return false;
      }
      if (!greenscreen->Initialize(&err)) {
        last_error = "Maxine green screen Initialize failed: " + err;
        if (error) *error = last_error;
        Destroy();
        return false;
      }
      if (!relight->Initialize(&err)) {
        last_error = "Maxine relighting Initialize failed: " + err;
        if (error) *error = last_error;
        Destroy();
        return false;
      }

      initialized = true;
      return true;
    }

    bool ApplyRgbInPlace(std::uint8_t* rgb,
                         int width,
                         int height,
                         std::size_t rgb_stride,
                         const studiocast::video::effects::BroadcastCameraEffects& fx,
                         bool apply_vignette,
                         float vignette_center_x_px,
                         float vignette_center_y_px,
                         std::string* error,
                         bool defer_readback,
                         DeferredGpuOut* deferred_out) {
      if (!initialized || !greenscreen || !relight) {
        if (error) *error = "Maxine relighting not initialized.";
        return false;
      }
      if (!rgb || width <= 0 || height <= 0 || rgb_stride == 0) {
        if (error) *error = "Invalid RGB buffer.";
        return false;
      }

      const float intensity = Clamp01FromPercent(fx.virtual_key_light.intensity);
      if (intensity <= 0.0001f) {
        // No-op.
        return true;
      }

      std::filesystem::path hdri;
      std::string hdri_err;
      if (!ResolveHdriPath(fx, &hdri, &hdri_err)) {
        if (error) *error = hdri_err;
        return false;
      }

      auto tmp = fx;
      tmp.virtual_key_light.hdri_path = hdri.string();

      // RGB -> BGR staging
      studiocast::video::Rgb24ToBgr24(rgb,
                                     bgr_in.data(),
                                     width,
                                     height,
                                     rgb_stride,
                                     rgb_stride);

      // Upload CPU->GPU.
      const auto up = nvcv.f().NvCVImage_Transfer(&cpu_bgr_in, &gpu_bgr, 1.0f, greenscreen->cuda_stream(), nullptr);
      if (up != studiocast::maxine::NVCV_SUCCESS) {
        if (error) *error = "NvCVImage_Transfer(cpu->gpu) failed: " + std::to_string(up);
        return false;
      }

      studiocast::video::GpuFrame frame;
      frame.width = width;
      frame.height = height;
      frame.nvcv_gpu = &gpu_bgr;
      frame.cuda_stream = greenscreen->cuda_stream();

      std::string cfg_err;
      if (!greenscreen->Configure(tmp, &cfg_err) || !relight->Configure(tmp, &cfg_err)) {
        if (error) *error = cfg_err;
        return false;
      }

      std::string proc_err;
      auto st = greenscreen->Process(frame, &proc_err);
      if (st != studiocast::maxine::NVCV_SUCCESS) {
        if (error) *error = proc_err.empty() ? std::to_string(st) : proc_err;
        return false;
      }

      const auto* matte = greenscreen->MatteGpu();
      if (!matte) {
        if (error) *error = "Green Screen did not produce a matte.";
        return false;
      }

      frame.matte_gpu = matte;

      st = relight->Process(frame, &proc_err);
      if (st != studiocast::maxine::NVCV_SUCCESS) {
        if (error) *error = proc_err.empty() ? std::to_string(st) : proc_err;
        return false;
      }

      const auto* relit_fg = relight->OutputGpu();
      if (!relit_fg) {
        if (error) *error = "Relighting did not produce an output image.";
        return false;
      }

      if (!nvcv.f().NvCVImage_Composite || !nvcv.f().NvCVImage_Transfer) {
        if (error) *error = "NvCVImage_Composite/Transfer unavailable.";
        return false;
      }

      const auto stream = greenscreen->cuda_stream();

      // Intensity: scale matte alpha before compositing.
      const studiocast::maxine::NvCVImage* mat_for_comp = matte;
      if (intensity < 0.999f) {
        const auto sc = nvcv.f().NvCVImage_Transfer(matte,
                                                   &gpu_matte_scaled,
                                                   intensity,
                                                   stream,
                                                   nullptr);
        if (sc != studiocast::maxine::NVCV_SUCCESS) {
          if (error) *error = "NvCVImage_Transfer(matte scale) failed: " + std::to_string(sc);
          return false;
        }
        mat_for_comp = &gpu_matte_scaled;
      }

      const auto stc = nvcv.f().NvCVImage_Composite(relit_fg, &gpu_bgr, mat_for_comp, &gpu_bgr_out_img, stream);
      if (stc != studiocast::maxine::NVCV_SUCCESS) {
        if (error) *error = "NvCVImage_Composite failed: " + std::to_string(stc);
        return false;
      }

      if (apply_vignette && fx.vignette.enabled) {
        const float vig_intensity = Clamp01FromPercent(fx.vignette.intensity);
        if (vig_intensity > 0.0001f) {
          std::string ve;
          if (!vignette.ApplyInPlace(&gpu_bgr_out_img,
                                    vig_intensity,
                                    vignette_center_x_px,
                                    vignette_center_y_px,
                                    stream,
                                    &ve)) {
            if (error) *error = ve;
            return false;
          }
        }
      }

      if (defer_readback) {
        if (!deferred_out) {
          if (error) *error = "defer_readback requires deferred_out.";
          return false;
        }
        deferred_out->kind = DeferredGpuKind::nvcv_bgr;
        deferred_out->nvcv_img = &gpu_bgr_out_img;
        deferred_out->nvcv = &nvcv;
        deferred_out->cuda_img = nullptr;
        deferred_out->cuda = &cuda;
        deferred_out->stream = stream;
        return true;
      }

      const auto down = nvcv.f().NvCVImage_Transfer(&gpu_bgr_out_img, &cpu_bgr_out, 1.0f, stream, nullptr);
      if (down != studiocast::maxine::NVCV_SUCCESS) {
        if (error) *error = "NvCVImage_Transfer(gpu->cpu) failed: " + std::to_string(down);
        return false;
      }

      if (!SyncAfterGpuToCpuTransfer(cuda, stream, "Maxine relight: after gpu->cpu transfer", error)) {
        return false;
      }

      // BGR -> RGB back into the pipeline buffer.
      studiocast::video::Bgr24ToRgb24(bgr_out.data(),
                                     rgb,
                                     width,
                                     height,
                                     rgb_stride,
                                     rgb_stride);
      return true;
    }
  } maxine_relight;

  struct MaxineDenoiseContext {
    bool initialized = false;
    std::string last_error;

    studiocast::maxine::vfx::VfxApi vfx;
    studiocast::maxine::NvcvApi nvcv;
    studiocast::maxine::CudaDriverApi cuda;

    std::filesystem::path model_dir;
    std::unique_ptr<studiocast::maxine::effects::VfxDenoiseEffect> denoise;

    std::vector<std::uint8_t> bgr_in;
    std::vector<std::uint8_t> bgr_out;
    studiocast::maxine::NvCVImage cpu_bgr_in{};
    studiocast::maxine::NvCVImage cpu_bgr_out{};

    // Denoise expects BGRf32 planar normalized on GPU (see VfxDenoiseEffect docs).
    studiocast::maxine::NvCVImage gpu_bgr_f32_in{};
    bool gpu_bgr_f32_in_allocated = false;

    ~MaxineDenoiseContext() { Destroy(); }

    void Destroy() {
      denoise.reset();

      if (gpu_bgr_f32_in_allocated && nvcv.IsInitialized() && nvcv.f().NvCVImage_Dealloc) {
        (void)nvcv.f().NvCVImage_Dealloc(&gpu_bgr_f32_in);
      }
      gpu_bgr_f32_in = studiocast::maxine::NvCVImage{};
      gpu_bgr_f32_in_allocated = false;

      bgr_in.clear();
      bgr_out.clear();
      cpu_bgr_in = studiocast::maxine::NvCVImage{};
      cpu_bgr_out = studiocast::maxine::NvCVImage{};

      model_dir.clear();

      initialized = false;
    }

    static std::filesystem::path InferModelsDirFromLibrary(const std::filesystem::path& lib) {
      std::error_code ec;
      std::filesystem::path p = lib.parent_path();
      for (int i = 0; i < 5 && !p.empty(); ++i) {
        const auto cand = p / "models";
        if (std::filesystem::exists(cand, ec) && std::filesystem::is_directory(cand, ec)) {
          return cand;
        }
        p = p.parent_path();
      }
      return {};
    }

    bool EnsureInitialized(int width,
                           int height,
                           const studiocast::video::effects::BroadcastCameraEffects& fx,
                           std::string* error) {
      last_error.clear();

      if (initialized) {
        // Live reconfiguration (strength).
        std::string cfg_err;
        if (denoise && !denoise->Configure(fx, &cfg_err)) {
          if (error) *error = cfg_err;
          return false;
        }
        return true;
      }

      std::string err;
      if (!vfx.Initialize(&err)) {
        last_error = "Maxine VFX runtime unavailable: " + err;
        if (error) *error = last_error;
        return false;
      }
      if (!nvcv.Initialize(studiocast::maxine::NvcvApi::Requirement::VfxCompat, &err)) {
        last_error = "NvCVImage runtime unavailable: " + err;
        if (error) *error = last_error;
        return false;
      }

      {
        std::string cuda_err;
        if (!cuda.Initialize(&cuda_err)) {
          last_error = "CUDA driver API unavailable: " + cuda_err;
          if (error) *error = last_error;
          return false;
        }
      }

      // Model directory discovery.
      {
        const char* env = std::getenv("STUDIOCAST_MAXINE_MODELS_DIR");
        if (env && *env) {
          model_dir = std::filesystem::path(env);
        }
      }
      if (model_dir.empty()) {
        model_dir = InferModelsDirFromLibrary(vfx.library_path());
      }

      // Allocate CPU BGR staging buffers.
      const std::size_t stride = static_cast<std::size_t>(width) * 3u;
      bgr_in.resize(stride * static_cast<std::size_t>(height));
      bgr_out.resize(stride * static_cast<std::size_t>(height));

      if (!nvcv.f().NvCVImage_Init) {
        last_error = "NvCVImage_Init missing from NvCVImage runtime.";
        if (error) *error = last_error;
        return false;
      }

      auto st = nvcv.f().NvCVImage_Init(&cpu_bgr_in,
                                       static_cast<unsigned>(width),
                                       static_cast<unsigned>(height),
                                       static_cast<int>(stride),
                                       bgr_in.data(),
                                       studiocast::maxine::NVCV_BGR,
                                       studiocast::maxine::NVCV_U8,
                                       studiocast::maxine::NVCV_CHUNKY,
                                       studiocast::maxine::NVCV_CPU);
      if (st != studiocast::maxine::NVCV_SUCCESS) {
        last_error = "NvCVImage_Init(cpu BGR in) failed: " + std::to_string(st);
        if (error) *error = last_error;
        return false;
      }

      st = nvcv.f().NvCVImage_Init(&cpu_bgr_out,
                                  static_cast<unsigned>(width),
                                  static_cast<unsigned>(height),
                                  static_cast<int>(stride),
                                  bgr_out.data(),
                                  studiocast::maxine::NVCV_BGR,
                                  studiocast::maxine::NVCV_U8,
                                  studiocast::maxine::NVCV_CHUNKY,
                                  studiocast::maxine::NVCV_CPU);
      if (st != studiocast::maxine::NVCV_SUCCESS) {
        last_error = "NvCVImage_Init(cpu BGR out) failed: " + std::to_string(st);
        if (error) *error = last_error;
        return false;
      }

      // Allocate GPU input (BGRf32 planar).
      st = nvcv.f().NvCVImage_Alloc(&gpu_bgr_f32_in,
                                   static_cast<unsigned>(width),
                                   static_cast<unsigned>(height),
                                   studiocast::maxine::NVCV_BGR,
                                   studiocast::maxine::NVCV_F32,
                                   studiocast::maxine::NVCV_PLANAR,
                                   studiocast::maxine::NVCV_GPU,
                                   /*alignment=*/0);
      if (st != studiocast::maxine::NVCV_SUCCESS) {
        last_error = "NvCVImage_Alloc(gpu BGRf32 planar) failed: " + std::to_string(st);
        if (error) *error = last_error;
        return false;
      }
      gpu_bgr_f32_in_allocated = true;

      denoise = std::make_unique<studiocast::maxine::effects::VfxDenoiseEffect>(&vfx, &nvcv, model_dir);

      std::string cfg_err;
      if (!denoise->Configure(fx, &cfg_err)) {
        last_error = cfg_err.empty() ? "Maxine denoise Configure failed." : cfg_err;
        if (error) *error = last_error;
        return false;
      }

      std::string init_err;
      if (!denoise->Initialize(&init_err)) {
        last_error = init_err.empty() ? "Maxine denoise Initialize failed." : init_err;
        if (error) *error = last_error;
        return false;
      }

      initialized = true;
      return true;
    }

    bool ApplyRgbInPlace(std::uint8_t* rgb,
                         int width,
                         int height,
                         std::size_t rgb_stride,
                         const studiocast::video::effects::BroadcastCameraEffects& fx,
                         std::string* error) {
      if (!rgb) {
        if (error) *error = "rgb buffer is null.";
        return false;
      }

      if (!EnsureInitialized(width, height, fx, error)) {
        return false;
      }
      if (!denoise) {
        if (error) *error = "Maxine denoise not initialized.";
        return false;
      }

      // RGB -> BGR (CPU staging).
      const std::size_t bgr_stride = static_cast<std::size_t>(width) * 3u;
      studiocast::video::Rgb24ToBgr24(rgb,
                                     bgr_in.data(),
                                     width,
                                     height,
                                     rgb_stride,
                                     bgr_stride);

      // CPU(BGRu8) -> GPU(BGRf32 planar)
      const auto stream = denoise->cuda_stream();
      auto st = nvcv.f().NvCVImage_Transfer(&cpu_bgr_in, &gpu_bgr_f32_in, 1.0f, stream, nullptr);
      if (st != studiocast::maxine::NVCV_SUCCESS) {
        if (error) *error = "NvCVImage_Transfer(cpu->gpu BGRf32) failed: " + std::to_string(st);
        return false;
      }

      studiocast::video::GpuFrame frame;
      frame.width = width;
      frame.height = height;
      frame.nvcv_gpu = &gpu_bgr_f32_in;
      frame.cuda_stream = stream;

      std::string mx_err;
      st = denoise->Process(frame, &mx_err);
      if (st != studiocast::maxine::NVCV_SUCCESS) {
        if (error) *error = mx_err.empty() ? "Maxine denoise Process failed." : mx_err;
        return false;
      }

      const auto* out_gpu = denoise->OutputGpu();
      if (!out_gpu) {
        if (error) *error = "Maxine denoise did not produce an output image.";
        return false;
      }

      // GPU(BGRf32 planar) -> CPU(BGRu8)
      st = nvcv.f().NvCVImage_Transfer(out_gpu, &cpu_bgr_out, 1.0f, stream, nullptr);
      if (st != studiocast::maxine::NVCV_SUCCESS) {
        if (error) *error = "NvCVImage_Transfer(gpu->cpu) failed: " + std::to_string(st);
        return false;
      }

      if (!SyncAfterGpuToCpuTransfer(cuda, stream, "Maxine denoise: after gpu->cpu transfer", error)) {
        return false;
      }

      // BGR -> RGB
      studiocast::video::Bgr24ToRgb24(bgr_out.data(),
                                     rgb,
                                     width,
                                     height,
                                     bgr_stride,
                                     rgb_stride);
      return true;
    }
  } maxine_denoise;

  struct MaxineEyeContactContext {
    bool initialized = false;
    std::string last_error;

    studiocast::maxine::ar::ArApi ar;
    studiocast::maxine::NvcvApi nvcv;
    studiocast::maxine::CudaDriverApi cuda;
    studiocast::maxine::CudaBgrVignette vignette;

    std::unique_ptr<studiocast::maxine::effects::ArEyeContactEffect> eye_contact;

    std::vector<std::uint8_t> bgr_in;
    std::vector<std::uint8_t> bgr_out;
    studiocast::maxine::NvCVImage cpu_bgr_in{};
    studiocast::maxine::NvCVImage cpu_bgr_out{};

    studiocast::maxine::NvCVImage gpu_bgr_in{};
    bool gpu_bgr_in_allocated = false;

    studiocast::maxine::NvCVImage gpu_bgr_out{};
    bool gpu_bgr_out_allocated = false;

    studiocast::maxine::CUstream stream = nullptr;
    bool stream_owned = false;

    ~MaxineEyeContactContext() { Destroy(); }

    void Destroy() {
      eye_contact.reset();

      if (stream_owned && stream && ar.IsInitialized() && ar.f().NvAR_CudaStreamDestroy) {
        (void)ar.f().NvAR_CudaStreamDestroy(stream);
      }
      stream = nullptr;
      stream_owned = false;

      if (gpu_bgr_out_allocated && nvcv.IsInitialized() && nvcv.f().NvCVImage_Dealloc) {
        (void)nvcv.f().NvCVImage_Dealloc(&gpu_bgr_out);
      }
      gpu_bgr_out = studiocast::maxine::NvCVImage{};
      gpu_bgr_out_allocated = false;

      if (gpu_bgr_in_allocated && nvcv.IsInitialized() && nvcv.f().NvCVImage_Dealloc) {
        (void)nvcv.f().NvCVImage_Dealloc(&gpu_bgr_in);
      }
      gpu_bgr_in = studiocast::maxine::NvCVImage{};
      gpu_bgr_in_allocated = false;

      bgr_in.clear();
      bgr_out.clear();
      cpu_bgr_in = studiocast::maxine::NvCVImage{};
      cpu_bgr_out = studiocast::maxine::NvCVImage{};

      initialized = false;
      last_error.clear();
    }

    bool EnsureInitialized(int width,
                           int height,
                           const studiocast::video::effects::BroadcastCameraEffects& fx,
                           std::string* error) {
      if (initialized) {
        std::string cfg_err;
        if (eye_contact && !eye_contact->Configure(fx, &cfg_err)) {
          if (error) *error = cfg_err;
          return false;
        }
        return true;
      }

      unsigned w = 0;
      unsigned h = 0;
      {
        std::string dim_err;
        if (!CheckedPositiveDims(width, height, &w, &h, &dim_err, "Maxine eye contact:")) {
          if (error) *error = dim_err;
          last_error = dim_err;
          return false;
        }
      }

      std::string ar_err;
      if (!ar.Initialize(&ar_err)) {
        if (error) *error = ar_err;
        last_error = ar_err;
        return false;
      }

      std::string nvcv_err;
      if (!nvcv.Initialize(studiocast::maxine::NvcvApi::Requirement::VfxCompat, &nvcv_err)) {
        if (error) *error = nvcv_err;
        last_error = nvcv_err;
        return false;
      }

      {
        std::string cuda_err;
        if (!cuda.Initialize(&cuda_err)) {
          const std::string e = "CUDA driver API unavailable: " + cuda_err;
          if (error) *error = e;
          last_error = e;
          return false;
        }
        std::string vig_err;
        if (!vignette.Initialize(&cuda, &vig_err)) {
          const std::string e = "CUDA vignette init failed: " + vig_err;
          if (error) *error = e;
          last_error = e;
          return false;
        }
      }

      if (!nvcv.f().NvCVImage_Init || !nvcv.f().NvCVImage_Alloc || !nvcv.f().NvCVImage_Transfer) {
        const std::string e = "NvCVImage_Init/Alloc/Transfer unavailable.";
        if (error) *error = e;
        last_error = e;
        return false;
      }

      // Stream: create one so transfers and AR run on a consistent stream.
      if (ar.f().NvAR_CudaStreamCreate) {
        const auto st = ar.f().NvAR_CudaStreamCreate(&stream);
        if (st != studiocast::maxine::NVCV_SUCCESS) {
          const std::string e = "NvAR_CudaStreamCreate failed: " + std::to_string(st);
          if (error) *error = e;
          last_error = e;
          return false;
        }
        stream_owned = true;
      }

      bgr_in.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 3u);
      bgr_out.resize(bgr_in.size());

      const auto init_in = nvcv.f().NvCVImage_Init(&cpu_bgr_in,
                                                  w,
                                                  h,
                                                  static_cast<int>(static_cast<std::size_t>(w) * 3u),
                                                  bgr_in.data(),
                                                  studiocast::maxine::NVCV_BGR,
                                                  studiocast::maxine::NVCV_U8,
                                                  studiocast::maxine::NVCV_CHUNKY,
                                                  studiocast::maxine::NVCV_CPU);
      if (init_in != studiocast::maxine::NVCV_SUCCESS) {
        const std::string e = "NvCVImage_Init(cpu input) failed: " + std::to_string(init_in);
        if (error) *error = e;
        last_error = e;
        return false;
      }

      const auto init_out = nvcv.f().NvCVImage_Init(&cpu_bgr_out,
                                                   w,
                                                   h,
                                                   static_cast<int>(static_cast<std::size_t>(w) * 3u),
                                                   bgr_out.data(),
                                                   studiocast::maxine::NVCV_BGR,
                                                   studiocast::maxine::NVCV_U8,
                                                   studiocast::maxine::NVCV_CHUNKY,
                                                   studiocast::maxine::NVCV_CPU);
      if (init_out != studiocast::maxine::NVCV_SUCCESS) {
        const std::string e = "NvCVImage_Init(cpu output) failed: " + std::to_string(init_out);
        if (error) *error = e;
        last_error = e;
        return false;
      }

      const auto alloc_in = nvcv.f().NvCVImage_Alloc(&gpu_bgr_in,
                                                    w,
                                                    h,
                                                    studiocast::maxine::NVCV_BGR,
                                                    studiocast::maxine::NVCV_U8,
                                                    studiocast::maxine::NVCV_CHUNKY,
                                                    studiocast::maxine::NVCV_GPU,
                                                    1);
      if (alloc_in != studiocast::maxine::NVCV_SUCCESS) {
        const std::string e = "NvCVImage_Alloc(gpu input) failed: " + std::to_string(alloc_in);
        if (error) *error = e;
        last_error = e;
        return false;
      }
      gpu_bgr_in_allocated = true;

      const auto alloc_out = nvcv.f().NvCVImage_Alloc(&gpu_bgr_out,
                                                     w,
                                                     h,
                                                     studiocast::maxine::NVCV_BGR,
                                                     studiocast::maxine::NVCV_U8,
                                                     studiocast::maxine::NVCV_CHUNKY,
                                                     studiocast::maxine::NVCV_GPU,
                                                     1);
      if (alloc_out != studiocast::maxine::NVCV_SUCCESS) {
        const std::string e = "NvCVImage_Alloc(gpu output) failed: " + std::to_string(alloc_out);
        if (error) *error = e;
        last_error = e;
        return false;
      }
      gpu_bgr_out_allocated = true;

      eye_contact = std::make_unique<studiocast::maxine::effects::ArEyeContactEffect>(&ar);

      std::string cfg_err;
      if (!eye_contact->Configure(fx, &cfg_err)) {
        if (error) *error = cfg_err;
        last_error = cfg_err;
        return false;
      }

      initialized = true;
      return true;
    }

    bool ApplyRgbInPlace(std::uint8_t* rgb,
                         int width,
                         int height,
                         std::size_t rgb_stride,
                         const studiocast::video::effects::BroadcastCameraEffects& fx,
                         bool apply_vignette,
                         float vignette_center_x_px,
                         float vignette_center_y_px,
                         std::string* error,
                         bool defer_readback,
                         DeferredGpuOut* deferred_out) {
      if (!rgb || width <= 0 || height <= 0) return true;
      std::string init_err;
      if (!EnsureInitialized(width, height, fx, &init_err)) {
        if (error) *error = init_err;
        return false;
      }
      if (!eye_contact) return true;

      // RGB -> BGR
      studiocast::video::Rgb24ToBgr24(rgb,
                                     bgr_in.data(),
                                     width,
                                     height,
                                     rgb_stride,
                                     static_cast<std::size_t>(width) * 3u);

      // CPU -> GPU
      const auto up = nvcv.f().NvCVImage_Transfer(&cpu_bgr_in, &gpu_bgr_in, 1.0f, stream, nullptr);
      if (up != studiocast::maxine::NVCV_SUCCESS) {
        if (error) *error = "NvCVImage_Transfer(cpu->gpu) failed: " + std::to_string(up);
        return false;
      }

      studiocast::video::GpuFrame frame;
      frame.width = width;
      frame.height = height;
      frame.nvcv_gpu = &gpu_bgr_in;
      frame.nvcv_tmp = &gpu_bgr_out;
      frame.cuda_stream = stream;

      std::string proc_err;
      const auto st = eye_contact->Process(frame, &proc_err);
      if (st != studiocast::maxine::NVCV_SUCCESS) {
        if (error) *error = proc_err.empty() ? std::to_string(st) : proc_err;
        return false;
      }

      if (apply_vignette && fx.vignette.enabled) {
        const float vig_intensity = Clamp01FromPercent(fx.vignette.intensity);
        if (vig_intensity > 0.0001f) {
          std::string ve;
          if (!vignette.ApplyInPlace(&gpu_bgr_out,
                                    vig_intensity,
                                    vignette_center_x_px,
                                    vignette_center_y_px,
                                    stream,
                                    &ve)) {
            if (error) *error = ve;
            return false;
          }
        }
      }

      if (defer_readback) {
        if (!deferred_out) {
          if (error) *error = "defer_readback requires deferred_out.";
          return false;
        }
        deferred_out->kind = DeferredGpuKind::nvcv_bgr;
        deferred_out->nvcv_img = &gpu_bgr_out;
        deferred_out->nvcv = &nvcv;
        deferred_out->cuda_img = nullptr;
        deferred_out->cuda = &cuda;
        deferred_out->stream = stream;
        return true;
      }

      // GPU -> CPU
      const auto down = nvcv.f().NvCVImage_Transfer(&gpu_bgr_out, &cpu_bgr_out, 1.0f, stream, nullptr);
      if (down != studiocast::maxine::NVCV_SUCCESS) {
        if (error) *error = "NvCVImage_Transfer(gpu->cpu) failed: " + std::to_string(down);
        return false;
      }

      if (!SyncAfterGpuToCpuTransfer(cuda, stream, "Maxine eye contact: after gpu->cpu transfer", error)) {
        return false;
      }

      // BGR -> RGB
      studiocast::video::Bgr24ToRgb24(bgr_out.data(),
                                     rgb,
                                     width,
                                     height,
                                     static_cast<std::size_t>(width) * 3u,
                                     rgb_stride);
      return true;
    }
  } maxine_eye_contact;

  struct MaxineAutoFrameContext {
    bool initialized = false;
    std::string last_error;

    studiocast::maxine::ar::ArApi ar;
    studiocast::maxine::NvcvApi nvcv;
    studiocast::maxine::CudaDriverApi cuda;
    studiocast::maxine::CudaBgrCropScale crop_scale;
    studiocast::maxine::CudaBgrVignette vignette;

    std::unique_ptr<studiocast::maxine::effects::ArAutoFrameTracker> tracker;

    std::vector<std::uint8_t> bgr_in;
    std::vector<std::uint8_t> bgr_out;
    studiocast::maxine::NvCVImage cpu_bgr_in{};
    studiocast::maxine::NvCVImage cpu_bgr_out{};

    studiocast::maxine::NvCVImage gpu_bgr_in{};
    bool gpu_bgr_in_allocated = false;
    studiocast::maxine::NvCVImage gpu_bgr_out{};
    bool gpu_bgr_out_allocated = false;

    studiocast::maxine::CUstream stream = nullptr;
    bool stream_owned = false;

    ~MaxineAutoFrameContext() { Destroy(); }

    void Destroy() {
      tracker.reset();

      if (stream_owned && stream && ar.IsInitialized() && ar.f().NvAR_CudaStreamDestroy) {
        (void)ar.f().NvAR_CudaStreamDestroy(stream);
      }
      stream = nullptr;
      stream_owned = false;

      if (gpu_bgr_out_allocated && nvcv.IsInitialized() && nvcv.f().NvCVImage_Dealloc) {
        (void)nvcv.f().NvCVImage_Dealloc(&gpu_bgr_out);
      }
      gpu_bgr_out = studiocast::maxine::NvCVImage{};
      gpu_bgr_out_allocated = false;

      if (gpu_bgr_in_allocated && nvcv.IsInitialized() && nvcv.f().NvCVImage_Dealloc) {
        (void)nvcv.f().NvCVImage_Dealloc(&gpu_bgr_in);
      }
      gpu_bgr_in = studiocast::maxine::NvCVImage{};
      gpu_bgr_in_allocated = false;

      bgr_in.clear();
      bgr_out.clear();
      cpu_bgr_in = studiocast::maxine::NvCVImage{};
      cpu_bgr_out = studiocast::maxine::NvCVImage{};

      initialized = false;
      last_error.clear();
    }

    bool EnsureInitialized(int width,
                           int height,
                           const studiocast::video::effects::BroadcastCameraEffects& fx,
                           std::string* error) {
      if (initialized) return true;

      unsigned w = 0;
      unsigned h = 0;
      {
        std::string dim_err;
        if (!CheckedPositiveDims(width, height, &w, &h, &dim_err, "Maxine auto frame:")) {
          if (error) *error = dim_err;
          last_error = dim_err;
          return false;
        }
      }

      std::string ar_err;
      if (!ar.Initialize(&ar_err)) {
        if (error) *error = ar_err;
        last_error = ar_err;
        return false;
      }

      std::string nvcv_err;
      if (!nvcv.Initialize(studiocast::maxine::NvcvApi::Requirement::VfxCompat, &nvcv_err)) {
        if (error) *error = nvcv_err;
        last_error = nvcv_err;
        return false;
      }

      std::string cuda_err;
      if (!cuda.Initialize(&cuda_err)) {
        if (error) *error = cuda_err;
        last_error = cuda_err;
        return false;
      }

      if (!nvcv.f().NvCVImage_Init || !nvcv.f().NvCVImage_Alloc || !nvcv.f().NvCVImage_Transfer) {
        const std::string e = "NvCVImage_Init/Alloc/Transfer unavailable.";
        if (error) *error = e;
        last_error = e;
        return false;
      }

      // Stream: create one so transfers, AR, and kernel launch share a stream.
      if (ar.f().NvAR_CudaStreamCreate) {
        const auto st = ar.f().NvAR_CudaStreamCreate(&stream);
        if (st != studiocast::maxine::NVCV_SUCCESS) {
          const std::string e = "NvAR_CudaStreamCreate failed: " + std::to_string(st);
          if (error) *error = e;
          last_error = e;
          return false;
        }
        stream_owned = true;
      }

      bgr_in.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 3u);
      bgr_out.resize(bgr_in.size());

      const auto init_in = nvcv.f().NvCVImage_Init(&cpu_bgr_in,
                                                  w,
                                                  h,
                                                  static_cast<int>(static_cast<std::size_t>(w) * 3u),
                                                  bgr_in.data(),
                                                  studiocast::maxine::NVCV_BGR,
                                                  studiocast::maxine::NVCV_U8,
                                                  studiocast::maxine::NVCV_CHUNKY,
                                                  studiocast::maxine::NVCV_CPU);
      if (init_in != studiocast::maxine::NVCV_SUCCESS) {
        const std::string e = "NvCVImage_Init(cpu input) failed: " + std::to_string(init_in);
        if (error) *error = e;
        last_error = e;
        return false;
      }

      const auto init_out = nvcv.f().NvCVImage_Init(&cpu_bgr_out,
                                                   w,
                                                   h,
                                                   static_cast<int>(static_cast<std::size_t>(w) * 3u),
                                                   bgr_out.data(),
                                                   studiocast::maxine::NVCV_BGR,
                                                   studiocast::maxine::NVCV_U8,
                                                   studiocast::maxine::NVCV_CHUNKY,
                                                   studiocast::maxine::NVCV_CPU);
      if (init_out != studiocast::maxine::NVCV_SUCCESS) {
        const std::string e = "NvCVImage_Init(cpu output) failed: " + std::to_string(init_out);
        if (error) *error = e;
        last_error = e;
        return false;
      }

      const auto alloc_in = nvcv.f().NvCVImage_Alloc(&gpu_bgr_in,
                                                    w,
                                                    h,
                                                    studiocast::maxine::NVCV_BGR,
                                                    studiocast::maxine::NVCV_U8,
                                                    studiocast::maxine::NVCV_CHUNKY,
                                                    studiocast::maxine::NVCV_GPU,
                                                    1);
      if (alloc_in != studiocast::maxine::NVCV_SUCCESS) {
        const std::string e = "NvCVImage_Alloc(gpu input) failed: " + std::to_string(alloc_in);
        if (error) *error = e;
        last_error = e;
        return false;
      }
      gpu_bgr_in_allocated = true;

      const auto alloc_out = nvcv.f().NvCVImage_Alloc(&gpu_bgr_out,
                                                     w,
                                                     h,
                                                     studiocast::maxine::NVCV_BGR,
                                                     studiocast::maxine::NVCV_U8,
                                                     studiocast::maxine::NVCV_CHUNKY,
                                                     studiocast::maxine::NVCV_GPU,
                                                     1);
      if (alloc_out != studiocast::maxine::NVCV_SUCCESS) {
        const std::string e = "NvCVImage_Alloc(gpu output) failed: " + std::to_string(alloc_out);
        if (error) *error = e;
        last_error = e;
        return false;
      }
      gpu_bgr_out_allocated = true;

      std::string crop_err;
      if (!crop_scale.Initialize(&cuda, &crop_err)) {
        if (error) *error = crop_err;
        last_error = crop_err;
        return false;
      }

      {
        std::string vig_err;
        if (!vignette.Initialize(&cuda, &vig_err)) {
          if (error) *error = vig_err;
          last_error = vig_err;
          return false;
        }
      }

      tracker = std::make_unique<studiocast::maxine::effects::ArAutoFrameTracker>(&ar);
      tracker->SetOutputAspect(static_cast<float>(width) / static_cast<float>(height));
      studiocast::maxine::effects::AutoFrameKnobs k;
      k.strength = fx.auto_frame.strength;
      k.smoothing = fx.auto_frame.smoothing;
      k.headroom = fx.auto_frame.headroom;
      tracker->SetKnobs(k);

      std::string tr_err;
      if (!tracker->EnsureInitialized(&gpu_bgr_in, &tr_err)) {
        if (error) *error = tr_err;
        last_error = tr_err;
        return false;
      }

      initialized = true;
      return true;
    }

    bool ApplyRgbInPlace(std::uint8_t* rgb,
                         int width,
                         int height,
                         std::size_t rgb_stride,
                         const studiocast::video::effects::BroadcastCameraEffects& fx,
                         bool apply_vignette,
                         float vignette_center_x_px,
                         float vignette_center_y_px,
                         std::string* error,
                         bool defer_readback,
                         DeferredGpuOut* deferred_out) {
      if (!rgb || width <= 0 || height <= 0) return true;

      std::string init_err;
      if (!EnsureInitialized(width, height, fx, &init_err)) {
        if (error) *error = init_err;
        return false;
      }
      if (!tracker) return true;

      // Update knobs without requiring re-init.
      tracker->SetOutputAspect(static_cast<float>(width) / static_cast<float>(height));
      studiocast::maxine::effects::AutoFrameKnobs k;
      k.strength = fx.auto_frame.strength;
      k.smoothing = fx.auto_frame.smoothing;
      k.headroom = fx.auto_frame.headroom;
      tracker->SetKnobs(k);

      // RGB -> BGR
      studiocast::video::Rgb24ToBgr24(rgb,
                                     bgr_in.data(),
                                     width,
                                     height,
                                     rgb_stride,
                                     static_cast<std::size_t>(width) * 3u);

      // CPU -> GPU
      const auto up = nvcv.f().NvCVImage_Transfer(&cpu_bgr_in, &gpu_bgr_in, 1.0f, stream, nullptr);
      if (up != studiocast::maxine::NVCV_SUCCESS) {
        if (error) *error = "NvCVImage_Transfer(cpu->gpu) failed: " + std::to_string(up);
        return false;
      }

      std::string tr_err;
      if (!tracker->Update(width, height, &tr_err)) {
        if (error) *error = tr_err;
        return false;
      }

      const auto crop = tracker->SmoothedCropPx();

      std::string cs_err;
      if (!crop_scale.CropScale(gpu_bgr_in,
                                &gpu_bgr_out,
                                crop.x,
                                crop.y,
                                crop.w,
                                crop.h,
                                stream,
                                &cs_err)) {
        if (error) *error = cs_err;
        return false;
      }

      if (apply_vignette && fx.vignette.enabled) {
        const float vig_intensity = Clamp01FromPercent(fx.vignette.intensity);
        if (vig_intensity > 0.0001f) {
          float cx = vignette_center_x_px;
          float cy = vignette_center_y_px;
          if (fx.vignette.center_on_tracked_face && tracker->last_had_detection()) {
            cx = static_cast<float>(crop.x) + static_cast<float>(crop.w) * 0.5f;
            cy = static_cast<float>(crop.y) + static_cast<float>(crop.h) * 0.5f;
          }

          std::string ve;
          if (!vignette.ApplyInPlace(&gpu_bgr_out, vig_intensity, cx, cy, stream, &ve)) {
            if (error) *error = ve;
            return false;
          }
        }
      }

      if (defer_readback) {
        if (!deferred_out) {
          if (error) *error = "defer_readback requires deferred_out.";
          return false;
        }
        deferred_out->kind = DeferredGpuKind::nvcv_bgr;
        deferred_out->nvcv_img = &gpu_bgr_out;
        deferred_out->nvcv = &nvcv;
        deferred_out->cuda_img = nullptr;
        deferred_out->cuda = &cuda;
        deferred_out->stream = stream;
        return true;
      }

      // GPU -> CPU
      const auto down = nvcv.f().NvCVImage_Transfer(&gpu_bgr_out, &cpu_bgr_out, 1.0f, stream, nullptr);
      if (down != studiocast::maxine::NVCV_SUCCESS) {
        if (error) *error = "NvCVImage_Transfer(gpu->cpu) failed: " + std::to_string(down);
        return false;
      }

      if (!SyncAfterGpuToCpuTransfer(cuda, stream, "Maxine auto frame: after gpu->cpu transfer", error)) {
        return false;
      }

      // BGR -> RGB
      studiocast::video::Bgr24ToRgb24(bgr_out.data(),
                                     rgb,
                                     width,
                                     height,
                                     static_cast<std::size_t>(width) * 3u,
                                     rgb_stride);
      return true;
    }
  } maxine_auto_frame;

  // Standalone GPU vignette stage.
  //
  // Used when vignette is enabled but no other Maxine GPU stage is active. This keeps
  // vignette GPU-only and avoids a silent CPU fallback.
  struct VignetteOnlyContext {
    bool initialized = false;
    std::string last_error;

    studiocast::maxine::NvcvApi nvcv;
    studiocast::maxine::CudaDriverApi cuda;
    studiocast::maxine::CudaBgrVignette vignette;

    std::vector<std::uint8_t> bgr_in;
    std::vector<std::uint8_t> bgr_out;
    studiocast::maxine::NvCVImage cpu_bgr_in{};
    studiocast::maxine::NvCVImage cpu_bgr_out{};

    studiocast::maxine::NvCVImage gpu_bgr{};
    bool gpu_bgr_allocated = false;

    ~VignetteOnlyContext() { Destroy(); }

    void Destroy() {
      if (gpu_bgr_allocated && nvcv.IsInitialized() && nvcv.f().NvCVImage_Dealloc) {
        (void)nvcv.f().NvCVImage_Dealloc(&gpu_bgr);
      }
      gpu_bgr = studiocast::maxine::NvCVImage{};
      gpu_bgr_allocated = false;

      bgr_in.clear();
      bgr_out.clear();
      cpu_bgr_in = studiocast::maxine::NvCVImage{};
      cpu_bgr_out = studiocast::maxine::NvCVImage{};

      initialized = false;
      last_error.clear();
    }

    bool EnsureInitialized(int width, int height, std::string* error) {
      last_error.clear();
      if (initialized) return true;

      unsigned w = 0;
      unsigned h = 0;
      {
        std::string dim_err;
        if (!CheckedPositiveDims(width, height, &w, &h, &dim_err, "Vignette:")) {
          last_error = dim_err;
          if (error) *error = last_error;
          return false;
        }
      }

      std::string nvcv_err;
      if (!nvcv.Initialize(studiocast::maxine::NvcvApi::Requirement::VfxCompat, &nvcv_err)) {
        last_error = "NvCVImage runtime unavailable: " + nvcv_err;
        if (error) *error = last_error;
        return false;
      }

      std::string cuda_err;
      if (!cuda.Initialize(&cuda_err)) {
        last_error = "CUDA driver API unavailable: " + cuda_err;
        if (error) *error = last_error;
        return false;
      }

      std::string vig_err;
      if (!vignette.Initialize(&cuda, &vig_err)) {
        last_error = "CUDA vignette init failed: " + vig_err;
        if (error) *error = last_error;
        return false;
      }

      if (!nvcv.f().NvCVImage_Init || !nvcv.f().NvCVImage_Alloc || !nvcv.f().NvCVImage_Transfer) {
        last_error = "NvCVImage_Init/Alloc/Transfer unavailable.";
        if (error) *error = last_error;
        return false;
      }

      bgr_in.resize(static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 3u);
      bgr_out.resize(bgr_in.size());

      const auto init_in = nvcv.f().NvCVImage_Init(&cpu_bgr_in,
                                                  w,
                                                  h,
                                                  static_cast<int>(static_cast<std::size_t>(w) * 3u),
                                                  bgr_in.data(),
                                                  studiocast::maxine::NVCV_BGR,
                                                  studiocast::maxine::NVCV_U8,
                                                  studiocast::maxine::NVCV_CHUNKY,
                                                  studiocast::maxine::NVCV_CPU);
      if (init_in != studiocast::maxine::NVCV_SUCCESS) {
        last_error = "NvCVImage_Init(cpu input) failed: " + std::to_string(init_in);
        if (error) *error = last_error;
        return false;
      }

      const auto init_out = nvcv.f().NvCVImage_Init(&cpu_bgr_out,
                                                   w,
                                                   h,
                                                   static_cast<int>(static_cast<std::size_t>(w) * 3u),
                                                   bgr_out.data(),
                                                   studiocast::maxine::NVCV_BGR,
                                                   studiocast::maxine::NVCV_U8,
                                                   studiocast::maxine::NVCV_CHUNKY,
                                                   studiocast::maxine::NVCV_CPU);
      if (init_out != studiocast::maxine::NVCV_SUCCESS) {
        last_error = "NvCVImage_Init(cpu output) failed: " + std::to_string(init_out);
        if (error) *error = last_error;
        return false;
      }

      const auto alloc = nvcv.f().NvCVImage_Alloc(&gpu_bgr,
                                                 w,
                                                 h,
                                                 studiocast::maxine::NVCV_BGR,
                                                 studiocast::maxine::NVCV_U8,
                                                 studiocast::maxine::NVCV_CHUNKY,
                                                 studiocast::maxine::NVCV_GPU,
                                                 1);
      if (alloc != studiocast::maxine::NVCV_SUCCESS) {
        last_error = "NvCVImage_Alloc(gpu bgr) failed: " + std::to_string(alloc);
        if (error) *error = last_error;
        return false;
      }
      gpu_bgr_allocated = true;

      initialized = true;
      return true;
    }

    bool ApplyRgbInPlace(std::uint8_t* rgb,
                         int width,
                         int height,
                         std::size_t rgb_stride,
                         const studiocast::video::effects::BroadcastCameraEffects& fx,
                         float vignette_center_x_px,
                         float vignette_center_y_px,
                         std::string* error,
                         bool defer_readback,
                         DeferredGpuOut* deferred_out) {
      if (!rgb || width <= 0 || height <= 0) return true;
      if (!fx.vignette.enabled) return true;

      const float vig_intensity = Clamp01FromPercent(fx.vignette.intensity);
      if (vig_intensity <= 0.0001f) return true;

      std::string init_err;
      if (!EnsureInitialized(width, height, &init_err)) {
        if (error) *error = init_err;
        return false;
      }

      // RGB -> BGR
      studiocast::video::Rgb24ToBgr24(rgb,
                                     bgr_in.data(),
                                     width,
                                     height,
                                     rgb_stride,
                                     static_cast<std::size_t>(width) * 3u);

      // CPU -> GPU
      const auto up = nvcv.f().NvCVImage_Transfer(&cpu_bgr_in, &gpu_bgr, 1.0f, /*stream=*/nullptr, nullptr);
      if (up != studiocast::maxine::NVCV_SUCCESS) {
        if (error) *error = "NvCVImage_Transfer(cpu->gpu) failed: " + std::to_string(up);
        return false;
      }

      std::string ve;
      if (!vignette.ApplyInPlace(&gpu_bgr,
                                vig_intensity,
                                vignette_center_x_px,
                                vignette_center_y_px,
                                /*stream=*/nullptr,
                                &ve)) {
        if (error) *error = ve;
        return false;
      }

      if (defer_readback) {
        if (!deferred_out) {
          if (error) *error = "defer_readback requires deferred_out.";
          return false;
        }
        deferred_out->kind = DeferredGpuKind::nvcv_bgr;
        deferred_out->nvcv_img = &gpu_bgr;
        deferred_out->nvcv = &nvcv;
        deferred_out->cuda_img = nullptr;
        deferred_out->cuda = &cuda;
        deferred_out->stream = nullptr;
        return true;
      }

      // GPU -> CPU
      const auto down = nvcv.f().NvCVImage_Transfer(&gpu_bgr, &cpu_bgr_out, 1.0f, /*stream=*/nullptr, nullptr);
      if (down != studiocast::maxine::NVCV_SUCCESS) {
        if (error) *error = "NvCVImage_Transfer(gpu->cpu) failed: " + std::to_string(down);
        return false;
      }

      if (!SyncAfterGpuToCpuTransfer(cuda,
                                     /*stream=*/nullptr,
                                     "Vignette-only: after gpu->cpu transfer",
                                     error)) {
        return false;
      }

      // BGR -> RGB
      studiocast::video::Bgr24ToRgb24(bgr_out.data(),
                                     rgb,
                                     width,
                                     height,
                                     static_cast<std::size_t>(width) * 3u,
                                     rgb_stride);
      return true;
    }
  } vignette_only;

  bool want_maxine_auto_frame = false;
  bool have_maxine_auto_frame = false;

  bool want_maxine_eye_contact = false;
  bool have_maxine_eye_contact = false;

  bool want_open_video_eye_contact = false;
  bool have_open_video_eye_contact = false;

  bool want_maxine_relight = false;
  bool have_maxine_relight = false;

  bool want_maxine_denoise = false;
  bool have_maxine_denoise = false;

  bool want_open_video_video_denoise = false;
  bool have_open_video_video_denoise = false;

  bool want_open_cuda_video_denoise = false;
  bool have_open_cuda_video_denoise = false;

  bool want_maxine_bg_blur = false;
  bool have_maxine_bg_blur = false;

  bool want_open_cuda_vb = false;
  bool have_open_cuda_vb = false;

  bool want_open_cuda_auto_frame = false;
  bool have_open_cuda_auto_frame = false;

  bool want_open_cuda_key_light = false;
  bool have_open_cuda_key_light = false;

  bool want_maxine_vignette_only = false;
  bool have_maxine_vignette_only = false;

  auto rebuildChain = [&](const studiocast::video::effects::BroadcastCameraEffects& fx) {
    chain.Clear();

    auto plan = studiocast::video::effects::BuildBroadcastEffectsPlan(fx);

    std::string note;

    std::optional<studiocast::maxine::MaxineDiagnostics> maxine_diag;
    auto get_maxine_diag = [&]() -> const studiocast::maxine::MaxineDiagnostics& {
      if (!maxine_diag.has_value()) {
        studiocast::maxine::MaxineManager mgr;
        maxine_diag = mgr.Diagnose(false);
      }
      return *maxine_diag;
    };
    auto append_canonical_maxine_blocked = [&](studiocast::maxine::MaxineNeed need) {
      const auto& d = get_maxine_diag();
      const auto c = studiocast::maxine::BuildCanonicalMaxineBlockedCopy(d, need);
      std::string s = studiocast::maxine::FormatCanonicalMaxineBlockedCopy(c);
      if (s.empty()) s = c.summary;
      if (!s.empty()) {
        if (!note.empty()) note += "\n";
        note += s;
      }
    };

    auto append_rule_notes = [&] {
      if (plan.disabled.empty()) return;
      if (!note.empty()) note += "\n";
      note += "Effect rules:";
      for (const auto& d : plan.disabled) {
        note += "\n - " + d.id + ": " + d.reason;
      }
    };

    want_maxine_bg_blur = false;
    have_maxine_bg_blur = false;

    want_open_cuda_vb = false;
    have_open_cuda_vb = false;

    want_open_cuda_auto_frame = false;
    have_open_cuda_auto_frame = false;

    want_open_cuda_key_light = false;
    have_open_cuda_key_light = false;

    want_maxine_auto_frame = false;
    have_maxine_auto_frame = false;

    want_maxine_eye_contact = false;
    have_maxine_eye_contact = false;

    want_open_video_eye_contact = false;
    have_open_video_eye_contact = false;

    want_maxine_relight = false;
    have_maxine_relight = false;

    want_maxine_denoise = false;
    have_maxine_denoise = false;

    want_open_video_video_denoise = false;
    have_open_video_video_denoise = false;

    want_open_cuda_video_denoise = false;
    have_open_cuda_video_denoise = false;

    want_maxine_vignette_only = false;
    have_maxine_vignette_only = false;

    auto planned = [&] {
      std::set<std::string> s;
      for (const auto& id : plan.ordered_effect_ids) s.insert(id);
      return s;
    }();

    const auto has = [&](std::string_view id) {
      return planned.count(std::string(id)) != 0;
    };

    const bool vb_requested =
        has(studiocast::video::effects::contract::kEffectIdVirtualBackgroundBlur) ||
        has(studiocast::video::effects::contract::kEffectIdVirtualBackgroundRemove) ||
        has(studiocast::video::effects::contract::kEffectIdVirtualBackgroundReplace);

    const bool engine_maxine = (fx.engine == studiocast::video::effects::EffectsEnginePreference::maxine);
    const bool engine_open_cuda = (fx.engine == studiocast::video::effects::EffectsEnginePreference::open_cuda);

    bool maxine_strict_blocked = false;

    std::unordered_map<std::string, std::string> backend_for_effect;
    auto set_backend = [&](std::string_view effect_id, std::string_view backend) {
      backend_for_effect[std::string(effect_id)] = std::string(backend);
    };

    auto remove_stage_from_plan = [&](std::string_view effect_id) {
      const std::string id(effect_id);
      plan.ordered_effect_ids.erase(std::remove(plan.ordered_effect_ids.begin(),
                                                plan.ordered_effect_ids.end(),
                                                id),
                                   plan.ordered_effect_ids.end());
      if (plan.vignette_attach_to_effect_id == id) {
        plan.vignette_attach_to_effect_id.clear();
      }
    };

    // Video Noise Removal (Maxine VFX).
    if (has(studiocast::video::effects::contract::kEffectIdVideoNoiseRemoval)) {
      const std::string stage_id = std::string(studiocast::video::effects::contract::kEffectIdVideoNoiseRemoval);

      if (engine_open_cuda) {
        // Open-source: prefer Open Video FastDVDnet when installed, otherwise
        // fall back to the lightweight Open CUDA temporal denoiser.
        want_open_video_video_denoise = true;

        std::string ov_err;
        if (open_video_fastdvdnet.EnsureInitialized(capA.width, capA.height, fx.video_noise_removal.model_id, &ov_err)) {
          have_open_video_video_denoise = true;
          set_backend(stage_id, "open_video");
          if (!note.empty()) note += "\n";
          note += "Open Video: Video Noise Removal (FastDVDnet).";
        } else {
          want_open_cuda_video_denoise = true;
          have_open_cuda_video_denoise = true;
          set_backend(stage_id, "open_cuda");
          if (!note.empty()) note += "\n";
          note += "Open CUDA: Video Noise Removal (temporal denoise).";
          if (!ov_err.empty()) {
            note += "\n";
            note += ov_err;
          }
        }
      } else if (!maxine_strict_blocked) {
        // AUTO / MAXINE: prefer Maxine VFX when available.
        want_maxine_denoise = true;

        std::string mx_err;
        if (maxine_denoise.EnsureInitialized(capA.width, capA.height, fx, &mx_err)) {
          have_maxine_denoise = true;
          set_backend(stage_id, "maxine");
          if (!note.empty()) note += "\n";
          note += "Maxine VFX: Video Noise Removal.";
        } else {
          if (engine_maxine) {
            append_canonical_maxine_blocked(studiocast::maxine::MaxineNeed::vfx);
            maxine_strict_blocked = true;
          } else {
            // AUTO: prefer Open Video FastDVDnet when installed; fall back to the
            // lightweight Open CUDA temporal denoiser.
            want_open_video_video_denoise = true;
            std::string ov_err;
            if (open_video_fastdvdnet.EnsureInitialized(capA.width, capA.height, fx.video_noise_removal.model_id, &ov_err)) {
              have_open_video_video_denoise = true;
              set_backend(stage_id, "open_video");
              if (!note.empty()) note += "\n";
              note += "Open Video: Video Noise Removal (FastDVDnet) — Maxine VFX unavailable.";
              if (!mx_err.empty()) {
                note += "\n";
                note += mx_err;
              }
            } else {
              want_open_cuda_video_denoise = true;
              have_open_cuda_video_denoise = true;
              set_backend(stage_id, "open_cuda");
              if (!note.empty()) note += "\n";
              note += "Open CUDA: Video Noise Removal (temporal denoise) — Maxine VFX unavailable.";
              if (!mx_err.empty()) {
                note += "\n";
                note += mx_err;
              }
              if (!ov_err.empty()) {
                note += "\n";
                note += ov_err;
              }
            }
          }
        }
      }
    }

    // If Open CUDA denoise isn't active, reset the temporal state so a future enable/fallback
    // doesn't blend against stale frames.
    if (!have_open_cuda_video_denoise) {
      open_cuda_video_denoise.last_enabled = false;
    }

    // Likewise, if Open Video FastDVDnet isn't active, reset its temporal history.
    if (!have_open_video_video_denoise) {
      open_video_fastdvdnet.ResetTemporalState();
    }

    // Eye Contact (AR)
    if (has(studiocast::video::effects::contract::kEffectIdEyeContact)) {
      const std::string stage_id = std::string(studiocast::video::effects::contract::kEffectIdEyeContact);
      const bool engine_auto = !engine_maxine && !engine_open_cuda;

      auto detach_vignette_from_eye_contact = [&] {
        if (plan.vignette_attach_to_effect_id == stage_id) {
          plan.vignette_attach_to_effect_id.clear();
        }
      };

      auto try_open_video = [&](std::string* out_err) -> bool {
        want_open_video_eye_contact = true;
        std::string ov_err;
        if (open_video_eye_contact.EnsureInitialized(fx.eye_contact.model_id, &ov_err)) {
          have_open_video_eye_contact = true;
          set_backend(stage_id, "open_video");
          detach_vignette_from_eye_contact();
          return true;
        }
        if (out_err) *out_err = ov_err;
        return false;
      };

      if (engine_open_cuda) {
        std::string ov_err;
        if (try_open_video(&ov_err)) {
          if (!note.empty()) note += "\n";
          note += "Open Video: Eye Contact (gaze correction).";
        } else {
          if (!note.empty()) note += "\n";
          note += "Open Video: Eye Contact unavailable: " + ov_err;
          remove_stage_from_plan(stage_id);
        }
      } else {
        // Maxine preferred when allowed; Open Video fallback in AUTO.
        std::string mx_err;
        bool maxine_ok = false;

        if (!maxine_strict_blocked) {
          want_maxine_eye_contact = true;
          maxine_ok = maxine_eye_contact.EnsureInitialized(capA.width, capA.height, fx, &mx_err);
        }

        if (maxine_ok) {
          have_maxine_eye_contact = true;
          set_backend(stage_id, "maxine_ar");
          if (!note.empty()) note += "\n";
          note += "Maxine AR: Eye Contact.";
        } else if (engine_auto) {
          std::string ov_err;
          if (try_open_video(&ov_err)) {
            if (!note.empty()) note += "\n";
            note += "Open Video: Eye Contact (gaze correction) — Maxine AR unavailable.";
            if (!mx_err.empty()) {
              note += "\n";
              note += mx_err;
            }
          } else {
            if (!note.empty()) note += "\n";
            if (!mx_err.empty()) {
              note += mx_err;
              note += "\n";
            }
            note += "Open Video: Eye Contact unavailable: " + ov_err;
            remove_stage_from_plan(stage_id);
          }
        } else {
          // MAXINE engine: no open-source fallback.
          if (!note.empty()) note += "\n";
          note += mx_err;
          append_canonical_maxine_blocked(studiocast::maxine::MaxineNeed::ar);
          maxine_strict_blocked = true;
          remove_stage_from_plan(stage_id);
        }
      }
    }

    // Background effects.
    if (vb_requested) {
      const auto vb_mode = fx.virtual_background.mode;

      std::optional<std::string_view> vb_effect_id;
      if (has(studiocast::video::effects::contract::kEffectIdVirtualBackgroundBlur)) {
        vb_effect_id = studiocast::video::effects::contract::kEffectIdVirtualBackgroundBlur;
      } else if (has(studiocast::video::effects::contract::kEffectIdVirtualBackgroundRemove)) {
        vb_effect_id = studiocast::video::effects::contract::kEffectIdVirtualBackgroundRemove;
      } else if (has(studiocast::video::effects::contract::kEffectIdVirtualBackgroundReplace)) {
        vb_effect_id = studiocast::video::effects::contract::kEffectIdVirtualBackgroundReplace;
      }

      auto append_backend_note = [&](const char* backend) {
        if (!note.empty()) note += "\n";
        if (vb_mode == studiocast::video::effects::VirtualBackgroundMode::blur) {
          note += std::string(backend) + ": Virtual Background (blur).";
        } else if (vb_mode == studiocast::video::effects::VirtualBackgroundMode::remove) {
          note += std::string(backend) + ": Virtual Background (remove).";
        } else {
          note += std::string(backend) + ": Virtual Background (replace).";
        }
      };

      if (fx.engine == studiocast::video::effects::EffectsEnginePreference::open_cuda) {
        want_open_cuda_vb = true;
        std::string oc_err;
        if (open_cuda_vb.EnsureInitialized(capA.width, capA.height, fx, &oc_err)) {
          have_open_cuda_vb = true;
          if (vb_effect_id.has_value()) set_backend(*vb_effect_id, "open_cuda");
          append_backend_note("Open CUDA");
        } else {
          if (!note.empty()) note += "\n";
          note += oc_err;
          if (vb_effect_id.has_value()) remove_stage_from_plan(*vb_effect_id);
        }
      } else if (fx.engine == studiocast::video::effects::EffectsEnginePreference::maxine) {
        if (!maxine_strict_blocked) {
          want_maxine_bg_blur = true;
          std::string mx_err;
          if (maxine_bg_blur.EnsureInitialized(capA.width, capA.height, fx, &mx_err)) {
            have_maxine_bg_blur = true;
            if (vb_effect_id.has_value()) set_backend(*vb_effect_id, "maxine");
            append_backend_note("Maxine VFX");
          } else {
            append_canonical_maxine_blocked(studiocast::maxine::MaxineNeed::vfx);
            maxine_strict_blocked = true;
            if (vb_effect_id.has_value()) remove_stage_from_plan(*vb_effect_id);
          }
        } else {
          if (vb_effect_id.has_value()) remove_stage_from_plan(*vb_effect_id);
        }
      } else {
        // auto_select: prefer Maxine when available; otherwise try Open CUDA.
        want_maxine_bg_blur = true;
        std::string mx_err;
        if (maxine_bg_blur.EnsureInitialized(capA.width, capA.height, fx, &mx_err)) {
          have_maxine_bg_blur = true;
          if (vb_effect_id.has_value()) set_backend(*vb_effect_id, "maxine");
          append_backend_note("Maxine VFX");
        } else {
          want_open_cuda_vb = true;
          std::string oc_err;
          if (open_cuda_vb.EnsureInitialized(capA.width, capA.height, fx, &oc_err)) {
            have_open_cuda_vb = true;
            if (vb_effect_id.has_value()) set_backend(*vb_effect_id, "open_cuda");
            append_backend_note("Open CUDA");
          } else {
            if (!note.empty()) note += "\n";
            note += mx_err;
            if (!note.empty()) note += "\n";
            note += oc_err;
            if (vb_effect_id.has_value()) remove_stage_from_plan(*vb_effect_id);
          }
        }
      }

      // Prevent silent vignette skip: vignette attachment is only implemented on Maxine/CUDA(BGR)
      // paths today.
      if (have_open_cuda_vb && has(studiocast::video::effects::contract::kEffectIdVignette)) {
        const std::string& attach = plan.vignette_attach_to_effect_id;
        if (attach == std::string(studiocast::video::effects::contract::kEffectIdVirtualBackgroundBlur) ||
            attach == std::string(studiocast::video::effects::contract::kEffectIdVirtualBackgroundRemove) ||
            attach == std::string(studiocast::video::effects::contract::kEffectIdVirtualBackgroundReplace)) {
          if (!note.empty()) note += "\n";
          note += "Vignette unavailable: Open CUDA virtual background does not support vignette yet.";
          remove_stage_from_plan(studiocast::video::effects::contract::kEffectIdVignette);
          plan.vignette_attach_to_effect_id.clear();
        }
      }

    } else if (has(studiocast::video::effects::contract::kEffectIdAutoFrame)) {
      const std::string stage_id = std::string(studiocast::video::effects::contract::kEffectIdAutoFrame);

      // Vignette attachment is only supported on the Maxine GPU paths. If we end up using the
      // Open CUDA auto-frame path (or auto-frame is unavailable), we run vignette as a standalone
      // stage by clearing the attachment.
      const auto detach_vignette_from_auto_frame = [&]() {
        if (has(studiocast::video::effects::contract::kEffectIdVignette) &&
            plan.vignette_attach_to_effect_id == stage_id) {
          plan.vignette_attach_to_effect_id.clear();
        }
      };

      if (engine_open_cuda) {
        // Open source: prefer Open Video (YuNet) face tracking when available, otherwise
        // fall back to the foreground matte path.
        want_open_cuda_auto_frame = true;

        bool have_open_video_face_detection = false;
        std::string fd_err;
        if (open_video_yunet.EnsureInitialized(fx.auto_frame.model_id, &fd_err)) {
          have_open_video_face_detection = true;
        }

        std::string oc_err;
        const bool require_matte_tracking = !have_open_video_face_detection;
        if (open_cuda_auto_frame.EnsureInitialized(capA.width,
                                                   capA.height,
                                                   fx,
                                                   /*require_matte_tracking=*/require_matte_tracking,
                                                   &oc_err)) {
          have_open_cuda_auto_frame = true;
          set_backend(stage_id, "open_cuda");
          if (!note.empty()) {
            note += "\n";
          }
          if (have_open_video_face_detection) {
            note += "Open Video (YuNet): Auto Frame (face tracking + CPU crop/scale).";
          } else {
            note += "Open CUDA: Auto Frame (foreground matte + CPU crop/scale).";
          }
          detach_vignette_from_auto_frame();
        } else {
          if (!note.empty()) {
            note += "\n";
          }
          if (!fd_err.empty() && !have_open_video_face_detection) {
            note += fd_err;
            if (!note.empty()) note += "\n";
          }
          note += oc_err;
          remove_stage_from_plan(stage_id);
          detach_vignette_from_auto_frame();
        }
      } else if (!maxine_strict_blocked) {
        // Maxine AR preferred; Open CUDA fallback when engine_preference=AUTO.
        want_maxine_auto_frame = true;
        std::string mx_err;
        if (maxine_auto_frame.EnsureInitialized(capA.width,
                                                capA.height,
                                                fx,
                                                &mx_err)) {
          have_maxine_auto_frame = true;
          set_backend(stage_id, "maxine_ar_cuda");
        } else if (engine_maxine) {
          if (!note.empty()) {
            note += "\n";
          }
          note += mx_err;
          note += "\nMaxine backend required due to engine_preference=maxine.\n";
          append_canonical_maxine_blocked(studiocast::maxine::MaxineNeed::ar);
          maxine_strict_blocked = true;

          // Auto-frame isn't available; don't leave vignette attached to a non-existent GPU stage.
          detach_vignette_from_auto_frame();
        } else {
          // AUTO: fall back to Open CUDA.
          want_open_cuda_auto_frame = true;
          bool have_open_video_face_detection = false;
          std::string fd_err;
          if (open_video_yunet.EnsureInitialized(fx.auto_frame.model_id, &fd_err)) {
            have_open_video_face_detection = true;
          }

          std::string oc_err;
          const bool require_matte_tracking = !have_open_video_face_detection;
          if (open_cuda_auto_frame.EnsureInitialized(capA.width,
                                                     capA.height,
                                                     fx,
                                                     /*require_matte_tracking=*/require_matte_tracking,
                                                     &oc_err)) {
            have_open_cuda_auto_frame = true;
            set_backend(stage_id, "open_cuda");
            if (!note.empty()) {
              note += "\n";
            }
            note += "Maxine AR unavailable; ";
            if (have_open_video_face_detection) {
              note += "Open Video (YuNet): Auto Frame (face tracking + CPU crop/scale).";
            } else {
              note += "Open CUDA: Auto Frame (foreground matte + CPU crop/scale).";
            }
            detach_vignette_from_auto_frame();
          } else {
            if (!note.empty()) {
              note += "\n";
            }
            note += mx_err;
            if (!fd_err.empty() && !have_open_video_face_detection) {
              if (!note.empty()) note += "\n";
              note += fd_err;
            }
            if (!note.empty()) {
              note += "\n";
            }
            note += oc_err;

            remove_stage_from_plan(stage_id);
            detach_vignette_from_auto_frame();
          }
        }
      }

      // If Auto Frame couldn't be initialized on any backend, make sure vignette isn't left attached.
      if (!have_maxine_auto_frame && !have_open_cuda_auto_frame) {
        detach_vignette_from_auto_frame();
      }
    }

    // Virtual Key Light.
    if (has(studiocast::video::effects::contract::kEffectIdVirtualKeyLight)) {
      const std::string stage_id = std::string(studiocast::video::effects::contract::kEffectIdVirtualKeyLight);

      // Vignette attachment is only supported on the Maxine GPU paths. If we end up using the
      // Open CUDA key light path (or key light is unavailable), run vignette as a standalone stage
      // by clearing the attachment.
      const auto detach_vignette_from_key_light = [&]() {
        if (has(studiocast::video::effects::contract::kEffectIdVignette) &&
            plan.vignette_attach_to_effect_id == stage_id) {
          plan.vignette_attach_to_effect_id.clear();
        }
      };

      if (engine_open_cuda) {
        // Open CUDA: key light via matting + masked CPU lift.
        want_open_cuda_key_light = true;
        std::string oc_err;
        if (open_cuda_key_light.EnsureInitialized(capA.width, capA.height, fx, &oc_err)) {
          have_open_cuda_key_light = true;
          set_backend(stage_id, "open_cuda");
          if (!note.empty()) {
            note += "\n";
          }
          note += "Open CUDA: Virtual Key Light (foreground matte + CPU relight).";
          detach_vignette_from_key_light();
        } else {
          if (!note.empty()) {
            note += "\n";
          }
          note += oc_err;
          remove_stage_from_plan(stage_id);
          detach_vignette_from_key_light();
        }
      } else if (!maxine_strict_blocked) {
        // Maxine VFX preferred; Open CUDA fallback when engine_preference=AUTO.
        want_maxine_relight = true;
        std::string mx_err;
        if (maxine_relight.EnsureInitialized(capA.width, capA.height, fx, &mx_err)) {
          have_maxine_relight = true;
          set_backend(stage_id, "maxine");
          if (!note.empty()) note += " ";
          note += "Maxine VFX: Video Relighting (Virtual Key Light).";
        } else if (engine_maxine) {
          append_canonical_maxine_blocked(studiocast::maxine::MaxineNeed::vfx);
          maxine_strict_blocked = true;
          detach_vignette_from_key_light();
        } else {
          // AUTO: fall back to Open CUDA.
          want_open_cuda_key_light = true;
          std::string oc_err;
          if (open_cuda_key_light.EnsureInitialized(capA.width, capA.height, fx, &oc_err)) {
            have_open_cuda_key_light = true;
            set_backend(stage_id, "open_cuda");
            if (!note.empty()) {
              note += "\n";
            }
            note += "Open CUDA: Virtual Key Light (foreground matte + CPU relight).";
            detach_vignette_from_key_light();
          } else {
            if (!note.empty()) note += " ";
            note += mx_err;
            if (!note.empty()) {
              note += "\n";
            }
            note += oc_err;

            remove_stage_from_plan(stage_id);
            detach_vignette_from_key_light();
          }
        }
      }

      // If key light couldn't be initialized on any backend, make sure vignette isn't left attached.
      if (!have_maxine_relight && !have_open_cuda_key_light) {
        detach_vignette_from_key_light();
      }
    }

    // Vignette-only stage.
    // If no other GPU stage is active, run a minimal GPU upload -> vignette kernel -> download.
    if (has(studiocast::video::effects::contract::kEffectIdVignette) &&
        plan.vignette_attach_to_effect_id.empty()) {
      const bool have_any_maxine_gpu_stage =
          have_maxine_eye_contact || have_maxine_bg_blur || have_maxine_relight || have_maxine_auto_frame;

      if (!have_any_maxine_gpu_stage) {
        want_maxine_vignette_only = true;
        std::string mx_err;
        if (vignette_only.EnsureInitialized(capA.width, capA.height, &mx_err)) {
          have_maxine_vignette_only = true;
          set_backend(studiocast::video::effects::contract::kEffectIdVignette, "cuda");
          if (!note.empty()) note += "\n";
          note += "CUDA: Vignette.";
        } else {
          if (!note.empty()) note += "\n";
          note += "Vignette unavailable: " + mx_err;
        }
      }
    }

    {
      append_rule_notes();

      std::lock_guard<std::mutex> lock(mu_);
      std::string backends;
      for (const auto& id : plan.ordered_effect_ids) {
        const auto it = backend_for_effect.find(id);
        if (it == backend_for_effect.end()) continue;
        if (!backends.empty()) backends += ",";
        backends += id + ":" + it->second;
      }

      effects_backends_ = backends;
      effects_note_ = note;
      if (!note.empty()) {
        last_error_ = note;
      }
    }

    appliedPlan = plan;
    appliedFx = fx;
  };

  // Initial chain based on config at pipeline start.
  {
    studiocast::video::effects::BroadcastCameraEffects fx;
    {
      std::lock_guard<std::mutex> fxLock(effects_mu_);
      fx = effects_;
    }
    rebuildChain(fx);
  }

  struct Ema {
    double alpha = 0.05;
    bool initialized = false;
    double v = 0.0;

    void Add(double x) {
      if (!initialized) {
        v = x;
        initialized = true;
        return;
      }
      v = (1.0 - alpha) * v + alpha * x;
    }

    double ValueOrZero() const { return initialized ? v : 0.0; }
  };

  using Clock = std::chrono::steady_clock;
  const auto ToMs = [](Clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
  };

  Ema ema_capture;
  Ema ema_scale;
  Ema ema_effects;
  Ema ema_write;
  Ema ema_period;

  // Optional end-to-end latency estimate and capture backlog stats.
  Ema ema_latency;
  std::uint64_t last_capture_sequence = 0;
  int dropped_capture_frames_total = 0;

  auto NowNs = [](clockid_t clock_id) -> std::uint64_t {
    timespec ts{};
    if (::clock_gettime(clock_id, &ts) != 0) return 0;
    return (static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ULL) +
           static_cast<std::uint64_t>(ts.tv_nsec);
  };
  auto NowNsMonotonic = [&]() -> std::uint64_t { return NowNs(CLOCK_MONOTONIC); };
  auto NowNsRealtime = [&]() -> std::uint64_t { return NowNs(CLOCK_REALTIME); };

  Clock::time_point last_frame_end{};
  bool have_last_frame_end = false;
  int perf_frames = 0;

  const bool debug_open_cuda_transfers = (std::getenv("STUDIOCAST_DEBUG_OPEN_CUDA_TRANSFERS") != nullptr) ||
                                         (std::getenv("STUDIOCAST_DEBUG_CUDA_UPLOADS") != nullptr);
  std::uint64_t open_cuda_active_frames = 0;
  std::uint64_t pipeline_open_cuda_upload_calls = 0;
  std::uint64_t pipeline_open_cuda_download_calls = 0;

  while (!stop_.load()) {
    CapturedFrameView f{};
    std::string ferr;
    if (!cap.AcquireFrame(&f, 1000, &ferr)) {
      // Timeouts and transient interruptions can happen on some devices/drivers.
      // Treat timeouts as recoverable to avoid pipeline flapping (which can manifest as
      // periodic black frames when the loopback output is kept alive).
      if (stop_.load()) break;

      if (IsRecoverableCaptureAcquireFailure(ferr)) continue;

      std::lock_guard<std::mutex> lock(mu_);
      last_error_ = "Capture acquire failed: " + ferr;
      break;
    }

    // Reduce end-to-end latency by always processing the newest frame available.
    //
    // If our loop ever runs behind (or the driver buffers aggressively), multiple frames
    // can be queued by the time we wake up. Drain any queued frames and keep only the most
    // recent, dropping older frames to avoid "living in the past".
    int dropped_this_frame = 0;
    for (;;) {
      if (stop_.load()) break;

      CapturedFrameView newer{};
      std::string nerr;
      if (!cap.AcquireFrame(&newer, 0, &nerr)) {
        // No additional frame ready.
        break;
      }

      // Release the older frame and keep the newer one.
      std::string rerr;
      (void)cap.ReleaseFrame(f, &rerr);
      f = newer;
      ++dropped_this_frame;
    }
    if (dropped_this_frame > 0) {
      dropped_capture_frames_total += dropped_this_frame;
    }
    last_capture_sequence = f.sequence;

    const auto t_capture_start = Clock::now();

    // Convert capture -> internal RGB (tight stride)
    if (capA.format == CapturePixelFormat::yuyv) {
      YuyvToRgb24(f.data, capA.width, capA.height, capA.bytes_per_line, rgb.data(), rgbStride);
    } else if (capA.format == CapturePixelFormat::mjpeg) {
      if (f.bytes == 0) {
        std::string rerr;
        (void)cap.ReleaseFrame(f, &rerr);
        std::lock_guard<std::mutex> lock(mu_);
        last_error_ = "MJPEG frame was empty (bytesused=0).";
        break;
      }
      int decW = 0;
      int decH = 0;
      std::string decErr;
      if (!DecodeMjpegToRgb24(f.data, f.bytes, rgb, decW, decH, &decErr)) {
        std::string rerr;
        (void)cap.ReleaseFrame(f, &rerr);
        std::lock_guard<std::mutex> lock(mu_);
        last_error_ = "MJPEG decode failed";
        if (!decErr.empty()) last_error_ += ": " + decErr;
        last_error_ += ".";
        break;
      }

      if (decW != capA.width || decH != capA.height) {
        std::string rerr;
        (void)cap.ReleaseFrame(f, &rerr);
        std::lock_guard<std::mutex> lock(mu_);
        std::ostringstream oss;
        oss << "MJPEG decode size mismatch: got " << decW << "x" << decH << ", expected " << capA.width << "x"
            << capA.height << ".";
        last_error_ = oss.str();
        break;
      }

      // The decoder is allowed to resize the RGB frame; keep our cached stride in sync.
      rgbStride = rgb.stride_bytes;
    } else {
      std::string rerr;
      (void)cap.ReleaseFrame(f, &rerr);
      std::lock_guard<std::mutex> lock(mu_);
      last_error_ = "Unsupported negotiated capture format: " + capA.pixfmt;
      break;
    }

    std::string rerr;
    (void)cap.ReleaseFrame(f, &rerr);

    const auto t_capture_end = Clock::now();
    const double capture_ms = ToMs(t_capture_end - t_capture_start);

    DeferredGpuOut deferred_gpu_out{};
    bool have_deferred_gpu_out = false;
    bool open_cuda_active_this_frame = false;

    // Reset per-frame Open CUDA ping-pong state.
    open_cuda_curr = nullptr;
    open_cuda_next = nullptr;
    open_cuda_uploaded_this_frame = false;

    // Open CUDA transfer invariant (pipeline contract)
    //
    // Define “Open CUDA active for this frame” as:
    //   fx.engine == EffectsEnginePreference::open_cuda
    //   AND at least one Open CUDA stage is planned+enabled and will actually run.
    //   (Today that effectively means: the Virtual Background stage is present in
    //    appliedPlan.ordered_effect_ids and have_open_cuda_vb == true.)
    //
    // Invariant we are moving toward:
    //   - If Open CUDA is active this frame: exactly one CPU->GPU upload at the
    //     start of the Open CUDA section, then GPU-only model/effect stages,
    //     then exactly one GPU->CPU download at the end (including any resize).
    //   - If Open CUDA is NOT active this frame: zero GPU uploads/downloads.
    //
    // Current transfer points (pre-refactor):
    //   - OpenCudaVirtualBackgroundContext::ApplyRgbInPlace does UploadFromCpuRgb24(...)
    //     and, unless defer_readback is true, does DownloadToCpuRgb24(...) inside the stage.
    //   - allow_defer_readback below is currently gated by scaling_needed + “last stage”
    //     logic so that GPU resize can be combined with a single final readback.
    //   - OpenCudaGpuScaler can upload+download for resize even when no Open CUDA
    //     effect stages ran.
    //
    // Avoid mixed modes where an Open CUDA stage performs an internal readback
    // while the pipeline also expects DeferredGpuOut; when Open CUDA stages are
    // active the pipeline must have exactly one coherent upload/readback strategy.

    // Apply effects (in-place on RGB buffer).
    const auto t_effects_start = Clock::now();
    bool fx_failed = false;
    {
      studiocast::video::effects::BroadcastCameraEffects fx;
      {
        std::lock_guard<std::mutex> fxLock(effects_mu_);
        fx = effects_;
      }
      if (fx != appliedFx) {
        rebuildChain(fx);
      }

      studiocast::video::effects::Rgb24FrameView view;
      view.data = rgb.data();
      view.width = capA.width;
      view.height = capA.height;
      view.stride_bytes = rgbStride;

      const float vignette_center_x_px = static_cast<float>(capA.width) * 0.5f;
      const float vignette_center_y_px = static_cast<float>(capA.height) * 0.5f;

      const auto& plan = appliedPlan;

      const auto has_stage = [&](std::string_view id) {
        return std::find(plan.ordered_effect_ids.begin(),
                         plan.ordered_effect_ids.end(),
                         std::string(id)) != plan.ordered_effect_ids.end();
      };

      const bool vignette_requested = has_stage(studiocast::video::effects::contract::kEffectIdVignette);
      const std::string& vig_attach = plan.vignette_attach_to_effect_id;

      // Defer GPU->CPU readback for the last GPU stage only when:
      // - output scaling is needed
      // - no CPU tail effects are active
      // This allows us to resize on GPU and perform a single GPU->CPU transfer for output.
      std::string last_stage_for_defer;
      for (auto it = plan.ordered_effect_ids.rbegin(); it != plan.ordered_effect_ids.rend(); ++it) {
        if (*it == studiocast::video::effects::contract::kEffectIdVideoNoiseRemoval) continue;
        last_stage_for_defer = *it;
        break;
      }
      const bool scaling_needed = (capA.width != outA.width || capA.height != outA.height);
      const bool allow_defer_readback =
          scaling_needed && (gpu_backend != GpuResizeBackend::none) && !last_stage_for_defer.empty();

      // Define whether any Open CUDA stage is enabled for this frame.
      // Open CUDA stages can optionally use the deferred strategy (no internal readback) when
      // the pipeline can carry a GPU output to final scaling/output without CPU-side continuation.
      const bool open_cuda_any_stage_enabled =
          have_open_cuda_vb &&
          (has_stage(studiocast::video::effects::contract::kEffectIdVirtualBackgroundBlur) ||
           has_stage(studiocast::video::effects::contract::kEffectIdVirtualBackgroundRemove) ||
           has_stage(studiocast::video::effects::contract::kEffectIdVirtualBackgroundReplace));

      const auto is_open_cuda_stage_id = [&](const std::string& stage_id) {
        if (!open_cuda_any_stage_enabled) return false;
        return stage_id == studiocast::video::effects::contract::kEffectIdVirtualBackgroundBlur ||
               stage_id == studiocast::video::effects::contract::kEffectIdVirtualBackgroundRemove ||
               stage_id == studiocast::video::effects::contract::kEffectIdVirtualBackgroundReplace;
      };

      // Apply vignette exactly once, either attached to the planned last GPU stage,
      // or as a standalone GPU stage when no other GPU stage is active.
      const bool apply_vignette_on_eye_contact =
          vignette_requested && (vig_attach == studiocast::video::effects::contract::kEffectIdEyeContact);
      const bool apply_vignette_on_relight =
          vignette_requested && (vig_attach == studiocast::video::effects::contract::kEffectIdVirtualKeyLight);
      const bool apply_vignette_on_bg =
          vignette_requested && (vig_attach == studiocast::video::effects::contract::kEffectIdVirtualBackgroundBlur ||
                                 vig_attach == studiocast::video::effects::contract::kEffectIdVirtualBackgroundRemove ||
                                 vig_attach == studiocast::video::effects::contract::kEffectIdVirtualBackgroundReplace);
      const bool apply_vignette_on_auto_frame =
          vignette_requested && (vig_attach == studiocast::video::effects::contract::kEffectIdAutoFrame);

      const std::uint64_t capture_sequence = f.sequence;

      // Reset per-frame analysis cache (Open Video). This allows effects to share ML outputs
      // without re-running inference multiple times per frame.
      open_video_cache.BeginFrame(capture_sequence);

      // Open CUDA optimization: When both Virtual Key Light and Open CUDA Virtual Background
      // are enabled, the key light stage (CPU) would normally run first and trigger an extra
      // CPU->GPU upload for matting. We can avoid that duplicate upload by deferring the key
      // light stage until after the Open CUDA VB stage has already uploaded and computed the matte.
      bool pending_open_cuda_key_light = false;

      auto apply_stage = [&](const std::string& stage_id) {
        bool defer_readback = allow_defer_readback && (stage_id == last_stage_for_defer);
        if (is_open_cuda_stage_id(stage_id) && pending_open_cuda_key_light) {
          // Force immediate readback when we know a CPU stage (key light) must run after this
          // Open CUDA stage.
          defer_readback = false;
        }

        if (stage_id == studiocast::video::effects::contract::kEffectIdVideoNoiseRemoval) {
          if (have_maxine_denoise) {
            std::string mx_err;
            if (!maxine_denoise.ApplyRgbInPlace(rgb.data(),
                                                capA.width,
                                                capA.height,
                                                rgbStride,
                                                fx,
                                                &mx_err)) {
              {
                std::lock_guard<std::mutex> lock(mu_);
                last_error_ = "Maxine video noise removal failed: " + mx_err;
              }
              fx_failed = true;
            }
            return;
          }

          if (have_open_video_video_denoise) {
            std::string ov_err;
            if (!open_video_fastdvdnet.ApplyRgbInPlace(capture_sequence,
                                                       rgb.data(),
                                                       capA.width,
                                                       capA.height,
                                                       rgbStride,
                                                       fx.video_noise_removal.strength,
                                                       fx.video_noise_removal.model_id,
                                                       &ov_err)) {
              if (!ov_err.empty()) {
                std::lock_guard<std::mutex> lock(mu_);
                last_error_ = "Open Video video noise removal failed: " + ov_err;
              }

              // Best-effort fallback: run the lightweight Open CUDA temporal denoiser.
              std::string oc_err;
              (void)open_cuda_video_denoise.ApplyRgbInPlace(rgb.data(), capA.width, capA.height, rgbStride, fx, &oc_err);
            }
            return;
          }

          if (have_open_cuda_video_denoise) {
            std::string oc_err;
            if (!open_cuda_video_denoise.ApplyRgbInPlace(rgb.data(), capA.width, capA.height, rgbStride, fx, &oc_err)) {
              // Best-effort: keep the pipeline running and simply bypass this stage.
              if (!oc_err.empty()) {
                std::lock_guard<std::mutex> lock(mu_);
                last_error_ = "Open CUDA video noise removal failed: " + oc_err;
              }
            }
          }
          return;
        }

        if (stage_id == studiocast::video::effects::contract::kEffectIdEyeContact) {
          if (have_maxine_eye_contact) {
            std::string mx_err;
            if (!maxine_eye_contact.ApplyRgbInPlace(rgb.data(),
                                                    capA.width,
                                                    capA.height,
                                                    rgbStride,
                                                    fx,
                                                    apply_vignette_on_eye_contact,
                                                    vignette_center_x_px,
                                                    vignette_center_y_px,
                                                    &mx_err,
                                                    defer_readback,
                                                    &deferred_gpu_out)) {
              {
                std::lock_guard<std::mutex> lock(mu_);
                last_error_ = "Maxine eye contact failed: " + mx_err;
              }
              fx_failed = true;
            }
            if (defer_readback && deferred_gpu_out.kind != DeferredGpuKind::none) have_deferred_gpu_out = true;
            return;
          }

          if (have_open_video_eye_contact) {
            std::string ov_err;
            if (!open_video_eye_contact.ApplyRgbInPlace(capture_sequence,
                                                        rgb.data(),
                                                        capA.width,
                                                        capA.height,
                                                        rgbStride,
                                                        fx.eye_contact.strength,
                                                        fx.eye_contact.look_away_enabled,
                                                        fx.auto_frame.model_id,
                                                        fx.eye_contact.model_id,
                                                        &open_video_yunet,
                                                        &open_video_cache,
                                                        &ov_err)) {
              // Best-effort: keep the pipeline running and simply bypass this stage.
              if (!ov_err.empty()) {
                std::lock_guard<std::mutex> lock(mu_);
                last_error_ = "Open Video eye contact failed: " + ov_err;
              }
            }
            return;
          }
          return;
        }

        if (stage_id == studiocast::video::effects::contract::kEffectIdVirtualKeyLight) {
          if (have_maxine_relight) {
            std::string mx_err;
            if (!maxine_relight.ApplyRgbInPlace(rgb.data(),
                                                capA.width,
                                                capA.height,
                                                rgbStride,
                                                fx,
                                                apply_vignette_on_relight,
                                                vignette_center_x_px,
                                                vignette_center_y_px,
                                                &mx_err,
                                                defer_readback,
                                                &deferred_gpu_out)) {
              {
                std::lock_guard<std::mutex> lock(mu_);
                last_error_ = "Maxine relighting failed: " + mx_err;
              }
              fx_failed = true;
            }
            if (defer_readback && deferred_gpu_out.kind != DeferredGpuKind::none) have_deferred_gpu_out = true;
            return;
          }

          if (have_open_cuda_key_light) {
            // If Open CUDA VB is also enabled, defer key light until after VB so the VB upload
            // can be reused for matte computation (avoids a redundant upload in key light).
            if (open_cuda_any_stage_enabled) {
              pending_open_cuda_key_light = true;
              return;
            }

            std::string oc_err;
            if (!open_cuda_key_light.ApplyRgbInPlace(
                    capture_sequence, rgb.data(), capA.width, capA.height, rgbStride, fx, &oc_err)) {
              {
                std::lock_guard<std::mutex> lock(mu_);
                last_error_ = "Open CUDA virtual key light failed: " + oc_err;
              }

              // Keep the pipeline alive; bypass on failures.
              if (studiocast::video::effects::ShouldAbortPipelineOnOpenCudaVbApplyFailure()) {
                fx_failed = true;
              }
            }
          }
          return;
        }

        if (stage_id == studiocast::video::effects::contract::kEffectIdVirtualBackgroundBlur ||
            stage_id == studiocast::video::effects::contract::kEffectIdVirtualBackgroundRemove ||
            stage_id == studiocast::video::effects::contract::kEffectIdVirtualBackgroundReplace) {
          if (have_maxine_bg_blur) {
            std::string mx_err;
            if (!maxine_bg_blur.ApplyRgbInPlace(rgb.data(),
                                                capA.width,
                                                capA.height,
                                                rgbStride,
                                                fx,
                                                apply_vignette_on_bg,
                                                vignette_center_x_px,
                                                vignette_center_y_px,
                                                &mx_err,
                                                defer_readback,
                                                &deferred_gpu_out)) {
              {
                std::lock_guard<std::mutex> lock(mu_);
                last_error_ = "Maxine virtual background failed: " + mx_err;
              }
              fx_failed = true;
            }
            if (defer_readback && deferred_gpu_out.kind != DeferredGpuKind::none) have_deferred_gpu_out = true;
            return;
          }

          if (have_open_cuda_vb) {
            // If the Open CUDA key light stage was deferred earlier in the chain (to avoid a
            // redundant CPU->GPU upload for matting), apply it here immediately after the Open CUDA
            // VB stage finishes (or is bypassed) so downstream stage ordering is preserved.
            const auto apply_deferred_open_cuda_key_light = [&]() {
              if (!pending_open_cuda_key_light) return;
              pending_open_cuda_key_light = false;
              if (!have_open_cuda_key_light) return;

              std::string kl_err;
              if (!open_cuda_key_light.ApplyRgbInPlace(
                      capture_sequence, rgb.data(), capA.width, capA.height, rgbStride, fx, &kl_err)) {
                {
                  std::lock_guard<std::mutex> lock(mu_);
                  last_error_ = "Open CUDA virtual key light failed: " + kl_err;
                }

                // Keep the pipeline alive; bypass on failures.
                if (studiocast::video::effects::ShouldAbortPipelineOnOpenCudaVbApplyFailure()) {
                  fx_failed = true;
                }
              }
            };

            // Open CUDA VB does not support vignette attachment yet.
            if (apply_vignette_on_bg) {
              {
                std::lock_guard<std::mutex> lock(mu_);
                last_error_ = "Open CUDA virtual background does not support vignette yet.";
              }
              apply_deferred_open_cuda_key_light();
              return;
            }

            std::string oc_err;

            // Pipeline-level Open CUDA transfer counters (env-var gated).
            // This matches the transfer invariant we're moving toward: one upload at the start of
            // the Open CUDA section and one download at the end (possibly deferred).
            if (!open_cuda_active_this_frame) {
              open_cuda_active_this_frame = true;
              if (debug_open_cuda_transfers) {
                ++open_cuda_active_frames;
              }
            }

            // Upload once at the start of the Open CUDA section (per-frame), then keep the frame
            // in GPU memory across Open CUDA stages.
            if (!open_cuda_uploaded_this_frame) {
              std::string init_err;
              if (!open_cuda_vb.EnsureInitialized(capA.width, capA.height, fx, &init_err)) {
                oc_err = init_err;
              } else {
                std::string berr;
                if (!open_cuda_frame_a.ReallocIfNeeded(&open_cuda_vb.cuda,
                                                       capA.width,
                                                       capA.height,
                                                       studiocast::cuda::PixelFormatGpu::rgb_u8,
                                                       &berr) ||
                    !open_cuda_frame_b.ReallocIfNeeded(&open_cuda_vb.cuda,
                                                       capA.width,
                                                       capA.height,
                                                       studiocast::cuda::PixelFormatGpu::rgb_u8,
                                                       &berr)) {
                  oc_err = "Open CUDA: failed to allocate ping-pong frame buffers: " + berr;
                } else if (!open_cuda_frame_a.UploadFromCpuRgb24(&open_cuda_vb.cuda,
                                                                 rgb.data(),
                                                                 rgbStride,
                                                                 open_cuda_vb.vb_stream,
                                                                 &berr)) {
                  oc_err = "Open CUDA: frame upload failed: " + berr;
                } else {
                  open_cuda_curr = &open_cuda_frame_a;
                  open_cuda_next = &open_cuda_frame_b;
                  open_cuda_uploaded_this_frame = true;
                  if (debug_open_cuda_transfers) {
                    ++pipeline_open_cuda_upload_calls;
                  }
                }
              }

              if (!open_cuda_uploaded_this_frame) {
                {
                  std::lock_guard<std::mutex> lock(mu_);
                  last_error_ = "Open CUDA virtual background failed: " + oc_err;
                }

                // Safety net: Open CUDA VB failures are treated as non-fatal so we keep producing
                // pass-through frames rather than blacking out the stream.
                if (studiocast::video::effects::ShouldAbortPipelineOnOpenCudaVbApplyFailure()) {
                  fx_failed = true;
                }

                apply_deferred_open_cuda_key_light();
                return;
              }
            }

            if (!open_cuda_vb.ApplyCudaRgb(
                    *open_cuda_curr, open_cuda_next, fx, capture_sequence, &oc_err, &deferred_gpu_out)) {
              {
                std::lock_guard<std::mutex> lock(mu_);
                last_error_ = "Open CUDA virtual background failed: " + oc_err;
              }

              // Safety net: Open CUDA VB failures are treated as non-fatal so we keep producing
              // pass-through frames rather than blacking out the stream.
              if (studiocast::video::effects::ShouldAbortPipelineOnOpenCudaVbApplyFailure()) {
                fx_failed = true;
              }

              apply_deferred_open_cuda_key_light();
              return;
            }

            // Ping-pong swap.
            const bool curr_was_a = (open_cuda_curr == &open_cuda_frame_a);
            open_cuda_curr = open_cuda_next;
            open_cuda_next = curr_was_a ? &open_cuda_frame_a : &open_cuda_frame_b;

            if (defer_readback) {
              have_deferred_gpu_out = true;
              return;
            }

            // Download GPU->CPU for any CPU-side continuation.
            std::string down_err;
            if (!open_cuda_curr->DownloadToCpuRgb24(&open_cuda_vb.cuda, rgb.data(), rgbStride, open_cuda_vb.vb_stream,
                                                    &down_err) ||
                !open_cuda_vb.cuda.StreamSynchronize(open_cuda_vb.vb_stream, &down_err)) {
              {
                std::lock_guard<std::mutex> lock(mu_);
                last_error_ = "Open CUDA virtual background failed: Open CUDA: frame download failed: " + down_err;
              }

              // Treat download failures consistently with stage failures.
              if (studiocast::video::effects::ShouldAbortPipelineOnOpenCudaVbApplyFailure()) {
                fx_failed = true;
              }

              apply_deferred_open_cuda_key_light();
              return;
            }

            if (debug_open_cuda_transfers) {
              ++pipeline_open_cuda_download_calls;
            }

            apply_deferred_open_cuda_key_light();
            return;
          }

          return;
        }

        if (stage_id == studiocast::video::effects::contract::kEffectIdAutoFrame) {
          if (have_maxine_auto_frame) {
            std::string mx_err;
            if (!maxine_auto_frame.ApplyRgbInPlace(rgb.data(),
                                                   capA.width,
                                                   capA.height,
                                                   rgbStride,
                                                   fx,
                                                   apply_vignette_on_auto_frame,
                                                   vignette_center_x_px,
                                                   vignette_center_y_px,
                                                   &mx_err,
                                                   defer_readback,
                                                   &deferred_gpu_out)) {
              {
                std::lock_guard<std::mutex> lock(mu_);
                last_error_ = "Maxine auto frame failed: " + mx_err;
              }
              fx_failed = true;
            }
            if (defer_readback && deferred_gpu_out.kind != DeferredGpuKind::none) have_deferred_gpu_out = true;
            return;
          }

          if (have_open_cuda_auto_frame) {
            // Open Video YuNet face detection is preferred for Auto Frame (cheaper than
            // foreground matting, and helps avoid running the matting model solely for tracking).
            const std::vector<studiocast::open_video::FaceDetection>* face_detections = nullptr;
            if (open_video_yunet.available()) {
              std::string fd_err;
              if (open_video_yunet.EnsureDetectionsForFrame(rgb.data(),
                                                           capA.width,
                                                           capA.height,
                                                           rgbStride,
                                                           fx.auto_frame.model_id,
                                                           capture_sequence,
                                                           &open_video_cache,
                                                           &fd_err)) {
                if (open_video_cache.face_detections) {
                  face_detections = &(*open_video_cache.face_detections);
                }
              } else {
                // Best-effort: if face detection can't run (runtime failure), Auto Frame will
                // fall back to matte tracking if available.
                if (!fd_err.empty()) {
                  std::lock_guard<std::mutex> lock(mu_);
                  last_error_ = "Open Video YuNet failed: " + fd_err;
                }
              }
            }

            std::string oc_err;
            if (!open_cuda_auto_frame.ApplyRgbInPlace(
                    capture_sequence,
                    rgb.data(),
                    capA.width,
                    capA.height,
                    rgbStride,
                    fx,
                    face_detections,
                    &oc_err)) {
              // Best-effort: Auto Frame failures shouldn't break the entire pipeline.
              if (!oc_err.empty()) {
                std::lock_guard<std::mutex> lock(mu_);
                last_error_ = "Open CUDA auto frame failed: " + oc_err;
              }
            }
          }
          return;
        }

        if (stage_id == studiocast::video::effects::contract::kEffectIdVignette) {
          if (!have_maxine_vignette_only) return;
          std::string mx_err;
          if (!vignette_only.ApplyRgbInPlace(rgb.data(),
                                             capA.width,
                                             capA.height,
                                             rgbStride,
                                             fx,
                                             vignette_center_x_px,
                                             vignette_center_y_px,
                                             &mx_err,
                                             defer_readback,
                                             &deferred_gpu_out)) {
            {
              std::lock_guard<std::mutex> lock(mu_);
              last_error_ = "CUDA vignette failed: " + mx_err;
            }
            fx_failed = true;
          }
          if (defer_readback && deferred_gpu_out.kind != DeferredGpuKind::none) have_deferred_gpu_out = true;
          return;
        }
      };

      for (const auto& stage_id : plan.ordered_effect_ids) {
        apply_stage(stage_id);
        if (fx_failed) break;
      }

      // Safety net: if key light was deferred but VB did not run (or bailed early), apply it now
      // so it isn't silently skipped.
      if (!fx_failed && pending_open_cuda_key_light && have_open_cuda_key_light) {
        pending_open_cuda_key_light = false;
        std::string kl_err;
        if (!open_cuda_key_light.ApplyRgbInPlace(
                capture_sequence, rgb.data(), capA.width, capA.height, rgbStride, fx, &kl_err)) {
          {
            std::lock_guard<std::mutex> lock(mu_);
            last_error_ = "Open CUDA virtual key light failed: " + kl_err;
          }
          if (studiocast::video::effects::ShouldAbortPipelineOnOpenCudaVbApplyFailure()) {
            fx_failed = true;
          }
        }
      }

      // CPU-only tail effects run last.
      if (!fx_failed) {
        chain.Apply(view);
      }
    }

    const auto t_effects_end = Clock::now();
    const double effects_ms = ToMs(t_effects_end - t_effects_start);

    if (fx_failed) {
      break;
    }

    const auto t_scale_start = Clock::now();

    // Pack/write to output format.
    //
    // IMPORTANT: The negotiated camera capture size (capA) can differ from the loopback output
    // size (outA) due to camera format negotiation (common with YUYV) and/or consumers requesting
    // a preferred resolution. v4l2loopback does not always scale between these sizes, so if we
    // only populate the top-left portion of a larger output buffer, consumers will display the
    // frame in the top-left quadrant.
    //
    // To keep output stable, scale the internal RGB frame to the writer's negotiated output
    // dimensions before packing to RGB24/YUYV.
    const int outW = outA.width;
    const int outH = outA.height;
    const bool scaling_needed = (capA.width != outW || capA.height != outH);

    int frameW = capA.width;
    int frameH = capA.height;

    const std::uint8_t* rgbOut = rgb.data();
    std::size_t rgbOutStride = rgbStride;
    std::size_t rgbOutBytes = rgb.size();

    // If we deferred GPU readback, perform GPU resize (bilinear) to output dimensions and
    // do a single GPU->CPU transfer for the final output buffer.
    if (have_deferred_gpu_out && deferred_gpu_out.kind == DeferredGpuKind::cuda_rgb) {
      std::string gerr;
      bool ok = true;

      if (!deferred_gpu_out.cuda_img || !deferred_gpu_out.cuda) {
        ok = false;
        gerr = "Deferred Open CUDA output reference is incomplete.";
      }

      if (ok) {
        if (!deferred_gpu_out.cuda_img->Valid()) {
          ok = false;
          gerr = "Deferred Open CUDA output image is invalid.";
        }
      }

      if (ok) {
        if (deferred_gpu_out.cuda_img->format != studiocast::cuda::PixelFormatGpu::rgb_u8) {
          ok = false;
          gerr = "Deferred Open CUDA output image format must be rgb_u8.";
        }
      }

      const studiocast::cuda::CudaImage* src_img = deferred_gpu_out.cuda_img;
      const std::size_t tightStride = static_cast<std::size_t>(outW) * 3u;
      const std::size_t tightBytes = tightStride * static_cast<std::size_t>(outH);

      const studiocast::cuda::CudaImage* download_img = src_img;
      if (ok && (capA.width != outW || capA.height != outH)) {
        if (!gpu_rgb_scaled.ReallocIfNeeded(deferred_gpu_out.cuda,
                                            outW,
                                            outH,
                                            studiocast::cuda::PixelFormatGpu::rgb_u8,
                                            &gerr)) {
          ok = false;
        }
        gpu_rgb_scaled_allocated = gpu_rgb_scaled.Valid();
        if (ok) {
          if (!studiocast::cuda::kernels::ResizeBilinear(*src_img, gpu_rgb_scaled, deferred_gpu_out.stream, &gerr)) {
            ok = false;
          }
        }
        download_img = &gpu_rgb_scaled;
      }

      if (ok) {
        rgbScaled.resize(tightBytes);
        if (debug_open_cuda_transfers && open_cuda_active_this_frame) {
          ++pipeline_open_cuda_download_calls;
        }
        if (!download_img->DownloadToCpuRgb24(deferred_gpu_out.cuda,
                                              rgbScaled.data(),
                                              tightStride,
                                              deferred_gpu_out.stream,
                                              &gerr)) {
          ok = false;
        }
      }

      if (ok) {
        if (!deferred_gpu_out.cuda->StreamSynchronize(deferred_gpu_out.stream, &gerr)) {
          ok = false;
        }
      }

      if (ok) {
        frameW = outW;
        frameH = outH;
        rgbOut = rgbScaled.data();
        rgbOutStride = tightStride;
        rgbOutBytes = rgbScaled.size();
      } else {
        // Attempt a best-effort readback into the pipeline RGB buffer so CPU scaling/output remains correct.
        std::string derr;
        bool readback_ok = false;
        if (deferred_gpu_out.cuda_img && deferred_gpu_out.cuda) {
          if (debug_open_cuda_transfers && open_cuda_active_this_frame) {
            ++pipeline_open_cuda_download_calls;
          }
          if (deferred_gpu_out.cuda_img->DownloadToCpuRgb24(deferred_gpu_out.cuda,
                                                            rgb.data(),
                                                            rgbStride,
                                                            deferred_gpu_out.stream,
                                                            &derr) &&
              deferred_gpu_out.cuda->StreamSynchronize(deferred_gpu_out.stream, &derr)) {
            readback_ok = true;
            have_deferred_gpu_out = false;
          }
        }

        std::lock_guard<std::mutex> lock(mu_);
        last_error_ = "Open CUDA deferred GPU resize path failed";
        if (!gerr.empty()) last_error_ += ": " + gerr;
        if (!readback_ok && !derr.empty()) last_error_ += " (and readback failed: " + derr + ")";
        last_error_ += ".";
      }
    } else if (have_deferred_gpu_out && deferred_gpu_out.kind == DeferredGpuKind::nvcv_bgr) {
      std::string gerr;
      bool ok = true;

      if (!deferred_gpu_out.nvcv_img || !deferred_gpu_out.cuda || !deferred_gpu_out.nvcv) {
        ok = false;
        gerr = "Deferred GPU output reference is incomplete.";
      }

      if (ok) {
        if (gpu_resize_cuda != deferred_gpu_out.cuda) {
          gpu_resize_cuda = deferred_gpu_out.cuda;
          if (!gpu_resize_bilinear.Initialize(gpu_resize_cuda, &gerr)) {
            ok = false;
          }
        }
      }

      auto ensure_scaled_gpu = [&](unsigned w, unsigned h) {
        if (!ok) return;
        auto& nvcv = *deferred_gpu_out.nvcv;
        if (!nvcv.IsInitialized() || !nvcv.f().NvCVImage_Alloc) {
          ok = false;
          gerr = "NvCVImage runtime not initialized.";
          return;
        }

        if (gpu_bgr_scaled_allocated) {
          if (gpu_bgr_scaled.width == w && gpu_bgr_scaled.height == h && gpu_bgr_scaled.pixelFormat == studiocast::maxine::NVCV_BGR &&
              gpu_bgr_scaled.componentType == studiocast::maxine::NVCV_U8 && gpu_bgr_scaled.planar == studiocast::maxine::NVCV_CHUNKY &&
              gpu_bgr_scaled.gpuMem == studiocast::maxine::NVCV_GPU) {
            return;
          }

          if (nvcv.f().NvCVImage_Realloc) {
            const auto st = nvcv.f().NvCVImage_Realloc(&gpu_bgr_scaled,
                                                       w,
                                                       h,
                                                       studiocast::maxine::NVCV_BGR,
                                                       studiocast::maxine::NVCV_U8,
                                                       studiocast::maxine::NVCV_CHUNKY,
                                                       studiocast::maxine::NVCV_GPU,
                                                       /*alignment=*/0);
            if (st == studiocast::maxine::NVCV_SUCCESS) {
              return;
            }
          }

          if (nvcv.f().NvCVImage_Dealloc) {
            (void)nvcv.f().NvCVImage_Dealloc(&gpu_bgr_scaled);
          }
          gpu_bgr_scaled = studiocast::maxine::NvCVImage{};
          gpu_bgr_scaled_allocated = false;
        }

        const auto st = nvcv.f().NvCVImage_Alloc(&gpu_bgr_scaled,
                                                 w,
                                                 h,
                                                 studiocast::maxine::NVCV_BGR,
                                                 studiocast::maxine::NVCV_U8,
                                                 studiocast::maxine::NVCV_CHUNKY,
                                                 studiocast::maxine::NVCV_GPU,
                                                 /*alignment=*/0);
        if (st != studiocast::maxine::NVCV_SUCCESS) {
          ok = false;
          gerr = "NvCVImage_Alloc(gpu scaled) failed: " + std::to_string(st);
          return;
        }
        gpu_bgr_scaled_allocated = true;
        gpu_resize_nvcv = deferred_gpu_out.nvcv;
      };

      ensure_scaled_gpu(static_cast<unsigned>(outW), static_cast<unsigned>(outH));

      if (ok) {
        if (!gpu_resize_bilinear.Resize(*deferred_gpu_out.nvcv_img, &gpu_bgr_scaled, deferred_gpu_out.stream, &gerr)) {
          ok = false;
        }
      }

      if (ok) {
        if (!deferred_gpu_out.nvcv->f().NvCVImage_Init || !deferred_gpu_out.nvcv->f().NvCVImage_Transfer) {
          ok = false;
          gerr = "NvCVImage_Init/Transfer unavailable.";
        }
      }

      if (ok) {
        if (outW <= 0 || outH <= 0) {
          ok = false;
          gerr = "Invalid output size for GPU resize: " + std::to_string(outW) + "x" + std::to_string(outH) + ".";
        }
      }

      if (ok) {
        const unsigned out_w = static_cast<unsigned>(outW);
        const unsigned out_h = static_cast<unsigned>(outH);
        const std::size_t tightStride = static_cast<std::size_t>(outW) * 3u;
        bgr_scaled_out.resize(tightStride * static_cast<std::size_t>(outH));

        const auto init = deferred_gpu_out.nvcv->f().NvCVImage_Init(&cpu_bgr_scaled,
                                                                    out_w,
                                                                    out_h,
                                                                    static_cast<int>(tightStride),
                                                                    bgr_scaled_out.data(),
                                                                    studiocast::maxine::NVCV_BGR,
                                                                    studiocast::maxine::NVCV_U8,
                                                                    studiocast::maxine::NVCV_CHUNKY,
                                                                    studiocast::maxine::NVCV_CPU);
        if (init != studiocast::maxine::NVCV_SUCCESS) {
          ok = false;
          gerr = "NvCVImage_Init(cpu scaled) failed: " + std::to_string(init);
        }

        if (ok) {
          const auto down = deferred_gpu_out.nvcv->f().NvCVImage_Transfer(&gpu_bgr_scaled,
                                                                          &cpu_bgr_scaled,
                                                                          1.0f,
                                                                          deferred_gpu_out.stream,
                                                                          nullptr);
          if (down != studiocast::maxine::NVCV_SUCCESS) {
            ok = false;
            gerr = "NvCVImage_Transfer(gpu->cpu scaled) failed: " + std::to_string(down);
          }
        }

        if (ok) {
          if (!deferred_gpu_out.cuda) {
            ok = false;
            gerr = "Deferred GPU resize readback: missing CUDA driver API.";
          } else if (!SyncAfterGpuToCpuTransfer(*deferred_gpu_out.cuda,
                                                deferred_gpu_out.stream,
                                                "Deferred GPU resize readback: after gpu->cpu transfer",
                                                &gerr)) {
            ok = false;
          }
        }

        if (ok) {
          rgbScaled.resize(tightStride * static_cast<std::size_t>(outH));
          studiocast::video::Bgr24ToRgb24(bgr_scaled_out.data(),
                                         rgbScaled.data(),
                                         outW,
                                         outH,
                                         tightStride,
                                         tightStride);
          frameW = outW;
          frameH = outH;
          rgbOut = rgbScaled.data();
          rgbOutStride = tightStride;
          rgbOutBytes = rgbScaled.size();
        }
      }

      if (!ok) {
        // Best-effort: transfer the unscaled GPU frame back to the pipeline RGB buffer so we can
        // continue with other scaling backends (or fail fast if CPU resize is disallowed).
        bool readback_ok = false;
        std::string derr;
        if (deferred_gpu_out.nvcv_img && deferred_gpu_out.nvcv && deferred_gpu_out.cuda) {
          const auto& nvcv_f = deferred_gpu_out.nvcv->f();
          if (deferred_gpu_out.nvcv->IsInitialized() && nvcv_f.NvCVImage_Init && nvcv_f.NvCVImage_Transfer) {
            const std::size_t srcTightStride = static_cast<std::size_t>(frameW) * 3u;
            std::vector<std::uint8_t> bgr_readback;
            bgr_readback.resize(srcTightStride * static_cast<std::size_t>(frameH));
            studiocast::maxine::NvCVImage cpu_bgr_readback{};

            const auto init = nvcv_f.NvCVImage_Init(&cpu_bgr_readback,
                                                    static_cast<unsigned>(frameW),
                                                    static_cast<unsigned>(frameH),
                                                    static_cast<int>(srcTightStride),
                                                    bgr_readback.data(),
                                                    studiocast::maxine::NVCV_BGR,
                                                    studiocast::maxine::NVCV_U8,
                                                    studiocast::maxine::NVCV_CHUNKY,
                                                    studiocast::maxine::NVCV_CPU);
            if (init == studiocast::maxine::NVCV_SUCCESS) {
              const auto down = nvcv_f.NvCVImage_Transfer(deferred_gpu_out.nvcv_img,
                                                          &cpu_bgr_readback,
                                                          1.0f,
                                                          deferred_gpu_out.stream,
                                                          nullptr);
              if (down == studiocast::maxine::NVCV_SUCCESS) {
                std::string sync_err;
                if (SyncAfterGpuToCpuTransfer(*deferred_gpu_out.cuda,
                                              deferred_gpu_out.stream,
                                              "Deferred NvCV readback after resize failure",
                                              &sync_err)) {
                  studiocast::video::Bgr24ToRgb24(bgr_readback.data(),
                                                 rgb.data(),
                                                 frameW,
                                                 frameH,
                                                 srcTightStride,
                                                 rgbStride);
                  readback_ok = true;
                  have_deferred_gpu_out = false;
                } else {
                  derr = sync_err;
                }
              } else {
                derr = "NvCVImage_Transfer(gpu->cpu) failed: " + std::to_string(down);
              }
            } else {
              derr = "NvCVImage_Init(cpu readback) failed: " + std::to_string(init);
            }
          }
        }

        std::lock_guard<std::mutex> lock(mu_);
        last_error_ = "Maxine/NVCV deferred GPU resize failed";
        if (!gerr.empty()) last_error_ += ": " + gerr;
        if (!readback_ok && !derr.empty()) last_error_ += " (and readback failed: " + derr + ")";
        last_error_ += ".";
      }
    }

    // Standalone GPU scaling backend: upload CPU RGB->GPU BGR, resize on GPU, download and convert back.
    // This is used when GPU scaling is selected but there is no deferred GPU stage to reuse.
    if ((gpu_backend == GpuResizeBackend::maxine_nvcv) && (frameW != outW || frameH != outH)) {
      std::string gerr;
      bool ok = maxine_scaler.initialized;
      if (!ok) {
        gerr = maxine_scaler.init_error.empty() ? "Maxine GPU scaler not initialized." : maxine_scaler.init_error;
      }

      const auto& nvcv = maxine_scaler.nvcv;
      const auto& nvcv_f = nvcv.f();
      if (ok) {
        if (!nvcv_f.NvCVImage_Init || !nvcv_f.NvCVImage_Transfer || !nvcv_f.NvCVImage_Alloc || !nvcv_f.NvCVImage_Dealloc) {
          ok = false;
          gerr = "NvCVImage API incomplete.";
        }
      }

      auto ensure_gpu_bgr = [&](studiocast::maxine::NvCVImage* img,
                                bool* allocated,
                                unsigned w,
                                unsigned h,
                                const char* what) {
        if (!ok) return;

        if (*allocated) {
          if (img->width == w && img->height == h && img->pixelFormat == studiocast::maxine::NVCV_BGR &&
              img->componentType == studiocast::maxine::NVCV_U8 && img->planar == studiocast::maxine::NVCV_CHUNKY &&
              img->gpuMem == studiocast::maxine::NVCV_GPU) {
            return;
          }

          if (nvcv_f.NvCVImage_Realloc) {
            const auto st = nvcv_f.NvCVImage_Realloc(img,
                                                     w,
                                                     h,
                                                     studiocast::maxine::NVCV_BGR,
                                                     studiocast::maxine::NVCV_U8,
                                                     studiocast::maxine::NVCV_CHUNKY,
                                                     studiocast::maxine::NVCV_GPU,
                                                     /*alignment=*/0);
            if (st == studiocast::maxine::NVCV_SUCCESS) {
              return;
            }
          }

          (void)nvcv_f.NvCVImage_Dealloc(img);
          *img = studiocast::maxine::NvCVImage{};
          *allocated = false;
        }

        const auto st = nvcv_f.NvCVImage_Alloc(img,
                                               w,
                                               h,
                                               studiocast::maxine::NVCV_BGR,
                                               studiocast::maxine::NVCV_U8,
                                               studiocast::maxine::NVCV_CHUNKY,
                                               studiocast::maxine::NVCV_GPU,
                                               /*alignment=*/0);
        if (st != studiocast::maxine::NVCV_SUCCESS) {
          ok = false;
          gerr = std::string("NvCVImage_Alloc(") + what + ") failed: " + std::to_string(st);
          return;
        }
        *allocated = true;
      };

      if (ok) {
        ensure_gpu_bgr(&maxine_scaler.gpu_in,
                       &maxine_scaler.gpu_in_allocated,
                       static_cast<unsigned>(frameW),
                       static_cast<unsigned>(frameH),
                       "gpu in");
      }

      if (ok) {
        ensure_gpu_bgr(&maxine_scaler.gpu_scaled,
                       &maxine_scaler.gpu_scaled_allocated,
                       static_cast<unsigned>(outW),
                       static_cast<unsigned>(outH),
                       "gpu scaled");
      }

      if (ok) {
        const std::size_t srcTightStride = static_cast<std::size_t>(frameW) * 3u;
        maxine_scaler.bgr_in.resize(srcTightStride * static_cast<std::size_t>(frameH));
        studiocast::video::Rgb24ToBgr24(rgbOut,
                                        maxine_scaler.bgr_in.data(),
                                        frameW,
                                        frameH,
                                        rgbOutStride,
                                        srcTightStride);

        const auto init = nvcv_f.NvCVImage_Init(&maxine_scaler.cpu_bgr_in,
                                                static_cast<unsigned>(frameW),
                                                static_cast<unsigned>(frameH),
                                                static_cast<int>(srcTightStride),
                                                maxine_scaler.bgr_in.data(),
                                                studiocast::maxine::NVCV_BGR,
                                                studiocast::maxine::NVCV_U8,
                                                studiocast::maxine::NVCV_CHUNKY,
                                                studiocast::maxine::NVCV_CPU);
        if (init != studiocast::maxine::NVCV_SUCCESS) {
          ok = false;
          gerr = "NvCVImage_Init(cpu in) failed: " + std::to_string(init);
        }

        if (ok) {
          const auto up = nvcv_f.NvCVImage_Transfer(&maxine_scaler.cpu_bgr_in,
                                                    &maxine_scaler.gpu_in,
                                                    1.0f,
                                                    nullptr,
                                                    nullptr);
          if (up != studiocast::maxine::NVCV_SUCCESS) {
            ok = false;
            gerr = "NvCVImage_Transfer(cpu->gpu in) failed: " + std::to_string(up);
          }
        }
      }

      if (ok) {
        if (!maxine_scaler.resize.Resize(maxine_scaler.gpu_in, &maxine_scaler.gpu_scaled, nullptr, &gerr)) {
          ok = false;
        }
      }

      if (ok) {
        const std::size_t dstTightStride = static_cast<std::size_t>(outW) * 3u;
        maxine_scaler.bgr_out.resize(dstTightStride * static_cast<std::size_t>(outH));

        const auto init = nvcv_f.NvCVImage_Init(&maxine_scaler.cpu_bgr_out,
                                                static_cast<unsigned>(outW),
                                                static_cast<unsigned>(outH),
                                                static_cast<int>(dstTightStride),
                                                maxine_scaler.bgr_out.data(),
                                                studiocast::maxine::NVCV_BGR,
                                                studiocast::maxine::NVCV_U8,
                                                studiocast::maxine::NVCV_CHUNKY,
                                                studiocast::maxine::NVCV_CPU);
        if (init != studiocast::maxine::NVCV_SUCCESS) {
          ok = false;
          gerr = "NvCVImage_Init(cpu out) failed: " + std::to_string(init);
        }

        if (ok) {
          const auto down = nvcv_f.NvCVImage_Transfer(&maxine_scaler.gpu_scaled,
                                                      &maxine_scaler.cpu_bgr_out,
                                                      1.0f,
                                                      nullptr,
                                                      nullptr);
          if (down != studiocast::maxine::NVCV_SUCCESS) {
            ok = false;
            gerr = "NvCVImage_Transfer(gpu->cpu out) failed: " + std::to_string(down);
          }
        }

        if (ok) {
          if (!SyncAfterGpuToCpuTransfer(maxine_scaler.cuda,
                                         /*stream=*/nullptr,
                                         "Standalone GPU scaler readback: after gpu->cpu transfer",
                                         &gerr)) {
            ok = false;
          }
        }

        if (ok) {
          rgbScaled.resize(dstTightStride * static_cast<std::size_t>(outH));
          studiocast::video::Bgr24ToRgb24(maxine_scaler.bgr_out.data(),
                                          rgbScaled.data(),
                                          outW,
                                          outH,
                                          dstTightStride,
                                          dstTightStride);
          frameW = outW;
          frameH = outH;
          rgbOut = rgbScaled.data();
          rgbOutStride = dstTightStride;
          rgbOutBytes = rgbScaled.size();
        }
      }

      if (!ok) {
        std::lock_guard<std::mutex> lock(mu_);
        last_error_ = "Maxine/NVCV GPU scaling failed";
        if (!gerr.empty()) last_error_ += ": " + gerr;
        last_error_ += ".";
      }
    }

    // Standalone GPU scaling backend (OpenCUDA/pure CUDA): upload CPU RGB -> GPU RGB, resize on GPU, download.
    //
    // This runs when OpenCUDA scaling is selected, and also serves as a fallback when Maxine/NVCV scaling fails.
    //
    // IMPORTANT: If Open CUDA effects were not active this frame and CPU resize is allowed, skip this
    // standalone scaler so we don't do unnecessary GPU uploads/downloads.
    const bool gpu_backend_supports_open_cuda_scaler =
        (gpu_backend == GpuResizeBackend::open_cuda || gpu_backend == GpuResizeBackend::maxine_nvcv);
    if (ShouldRunStandaloneOpenCudaScaler(/*scaling_needed=*/(frameW != outW || frameH != outH),
                                         /*gpu_backend_is_open_cuda_or_maxine=*/gpu_backend_supports_open_cuda_scaler,
                                         /*have_deferred_gpu_out=*/have_deferred_gpu_out,
                                         /*allow_cpu_resize=*/cfg.allow_cpu_resize,
                                         /*open_cuda_effects_ran=*/open_cuda_active_this_frame)) {
      std::string gerr;
      bool ok = open_cuda_scaler.EnsureInitialized(&gerr);
      if (ok) {
        if (!open_cuda_scaler.gpu_in.ReallocIfNeeded(&open_cuda_scaler.cuda,
                                                     frameW,
                                                     frameH,
                                                     studiocast::cuda::PixelFormatGpu::rgb_u8,
                                                     &gerr)) {
          ok = false;
        }
      }
      if (ok) {
        if (!open_cuda_scaler.gpu_scaled.ReallocIfNeeded(&open_cuda_scaler.cuda,
                                                         outW,
                                                         outH,
                                                         studiocast::cuda::PixelFormatGpu::rgb_u8,
                                                         &gerr)) {
          ok = false;
        }
      }
      if (ok) {
        if (!open_cuda_scaler.gpu_in.UploadFromCpuRgb24(&open_cuda_scaler.cuda,
                                                        rgbOut,
                                                        rgbOutStride,
                                                        open_cuda_scaler.stream,
                                                        &gerr)) {
          ok = false;
        }
      }
      if (ok) {
        if (!studiocast::cuda::kernels::ResizeBilinear(open_cuda_scaler.gpu_in,
                                                       open_cuda_scaler.gpu_scaled,
                                                       open_cuda_scaler.stream,
                                                       &gerr)) {
          ok = false;
        }
      }
      if (ok) {
        const std::size_t tightStride = static_cast<std::size_t>(outW) * 3u;
        rgbScaled.resize(tightStride * static_cast<std::size_t>(outH));
        if (!open_cuda_scaler.gpu_scaled.DownloadToCpuRgb24(&open_cuda_scaler.cuda,
                                                            rgbScaled.data(),
                                                            tightStride,
                                                            open_cuda_scaler.stream,
                                                            &gerr)) {
          ok = false;
        }
      }
      if (ok) {
        if (!open_cuda_scaler.cuda.StreamSynchronize(open_cuda_scaler.stream, &gerr)) {
          ok = false;
        }
      }
      if (ok) {
        const std::size_t tightStride = static_cast<std::size_t>(outW) * 3u;
        frameW = outW;
        frameH = outH;
        rgbOut = rgbScaled.data();
        rgbOutStride = tightStride;
        rgbOutBytes = rgbScaled.size();
      } else {
        std::lock_guard<std::mutex> lock(mu_);
        last_error_ = "OpenCUDA GPU scaling failed";
        if (!gerr.empty()) last_error_ += ": " + gerr;
        last_error_ += ".";
      }
    }

    if (frameW != outW || frameH != outH) {
      if (!cfg.allow_cpu_resize) {
        std::lock_guard<std::mutex> lock(mu_);
        last_error_ = OutputResizeDisallowedErrorMessage(frameW, frameH, outW, outH);
        break;
      }
      std::string resizeErr;
      const std::size_t tightDstStride = static_cast<std::size_t>(outW) * 3u;
      if (!ResizeRgb24Bilinear(rgbOut,
                               frameW,
                               frameH,
                               rgbOutStride,
                               outW,
                               outH,
                               &rgbScaled,
                               tightDstStride,
                               &resizeErr)) {
        std::lock_guard<std::mutex> lock(mu_);
        last_error_ = "Resize failed: " + resizeErr;
        break;
      }
      rgbOut = rgbScaled.data();
      rgbOutStride = tightDstStride;
      rgbOutBytes = rgbScaled.size();
    }

    const auto t_scale_end = Clock::now();
    const double scale_ms = scaling_needed ? ToMs(t_scale_end - t_scale_start) : 0.0;

    const auto t_write_start = Clock::now();

    if (outA.format == PixelFormat::rgb24) {
      const std::size_t wantRow = static_cast<std::size_t>(outW) * 3u;

      if (outA.bytes_per_line == wantRow && outA.size_image == rgbOutBytes) {
        std::string werr;
        if (!writer_.WriteFrame(rgbOut, rgbOutBytes, &werr)) {
          std::lock_guard<std::mutex> lock(mu_);
          last_error_ = "Write failed: " + werr;
          break;
        }
      } else {
        // Copy into padded output buffer.
        for (int y = 0; y < outH; ++y) {
          const std::uint8_t* srcRow = rgbOut + static_cast<std::size_t>(y) * rgbOutStride;
          std::uint8_t* dstRow = outBuf.data() + static_cast<std::size_t>(y) * outA.bytes_per_line;

          for (std::size_t i = 0; i < wantRow; ++i) dstRow[i] = srcRow[i];
          // zero any padding
          for (std::size_t i = wantRow; i < outA.bytes_per_line; ++i) dstRow[i] = 0;
        }

        std::string werr;
        if (!writer_.WriteFrame(outBuf.data(), outBuf.size(), &werr)) {
          std::lock_guard<std::mutex> lock(mu_);
          last_error_ = "Write failed: " + werr;
          break;
        }
      }
    } else {
      // Output is YUYV; convert RGB -> YUYV into outBuf with output stride.
      Rgb24ToYuyv(rgbOut, outW, outH, rgbOutStride, outBuf.data(), outA.bytes_per_line);

      std::string werr;
      if (!writer_.WriteFrame(outBuf.data(), outBuf.size(), &werr)) {
        std::lock_guard<std::mutex> lock(mu_);
        last_error_ = "Write failed: " + werr;
        break;
      }
    }

    const auto t_write_end = Clock::now();
    const double write_ms = ToMs(t_write_end - t_write_start);

    // Approximate end-to-end latency (driver timestamp -> end of write).
    double latency_ms = 0.0;
    if (f.timestamp_ns != 0) {
      const std::uint64_t now_ns = f.timestamp_monotonic ? NowNsMonotonic() : NowNsRealtime();
      if (now_ns >= f.timestamp_ns) {
        latency_ms = static_cast<double>(now_ns - f.timestamp_ns) / 1000000.0;
      }
    }

    ema_capture.Add(capture_ms);
    ema_effects.Add(effects_ms);
    ema_scale.Add(scale_ms);
    ema_write.Add(write_ms);
    ema_latency.Add(latency_ms);

    ++perf_frames;

    if (debug_open_cuda_transfers && ((perf_frames % 120) == 0)) {
      std::fprintf(stderr,
                   "Open CUDA transfers (pipeline): active_frames=%llu upload_calls=%llu download_calls=%llu\n",
                   static_cast<unsigned long long>(open_cuda_active_frames),
                   static_cast<unsigned long long>(pipeline_open_cuda_upload_calls),
                   static_cast<unsigned long long>(pipeline_open_cuda_download_calls));
    }

    double fps_actual = 0.0;
    if (have_last_frame_end) {
      const double period_ms = ToMs(t_write_end - last_frame_end);
      if (period_ms > 0.0) {
        ema_period.Add(period_ms);
        const double avg_period = ema_period.ValueOrZero();
        if (avg_period > 0.0) fps_actual = 1000.0 / avg_period;
      }
    }
    last_frame_end = t_write_end;
    have_last_frame_end = true;

    ++frameIndex;
    const bool publish_perf = (frameIndex == 1) || ((frameIndex % 10) == 0);
    if (publish_perf) {
      std::lock_guard<std::mutex> lock(mu_);
      frame_index_ = frameIndex;
      ms_per_frame_.capture = ema_capture.ValueOrZero();
      ms_per_frame_.scale = ema_scale.ValueOrZero();
      ms_per_frame_.effects = ema_effects.ValueOrZero();
      ms_per_frame_.write = ema_write.ValueOrZero();
      fps_actual_ = fps_actual;
      perf_sample_frames_ = perf_frames;

      debug_.latency_ms = ema_latency.ValueOrZero();
      debug_.capture_sequence = last_capture_sequence;
      debug_.dropped_capture_frames = dropped_capture_frames_total;

      open_cuda_transfers_.active_frames = open_cuda_active_frames;
      open_cuda_transfers_.upload_calls = pipeline_open_cuda_upload_calls;
      open_cuda_transfers_.download_calls = pipeline_open_cuda_download_calls;
    }
  }

  // Cleanup cached GPU resize output.
  if (gpu_bgr_scaled_allocated && gpu_resize_nvcv && gpu_resize_nvcv->IsInitialized() &&
      gpu_resize_nvcv->f().NvCVImage_Dealloc) {
    (void)gpu_resize_nvcv->f().NvCVImage_Dealloc(&gpu_bgr_scaled);
  }
  gpu_bgr_scaled = studiocast::maxine::NvCVImage{};
  gpu_bgr_scaled_allocated = false;

  // Cleanup Open CUDA GPU resize cache.
  if (gpu_rgb_scaled_allocated && gpu_rgb_scaled.Valid() && open_cuda_vb.cuda.IsInitialized()) {
    std::string derr;
    (void)gpu_rgb_scaled.Free(&open_cuda_vb.cuda, &derr);
  }
  gpu_rgb_scaled = studiocast::cuda::CudaImage{};
  gpu_rgb_scaled_allocated = false;

  // Cleanup Open CUDA ping-pong frame buffers.
  if (open_cuda_vb.cuda.IsInitialized()) {
    if (open_cuda_frame_a.Valid()) {
      std::string derr;
      (void)open_cuda_frame_a.Free(&open_cuda_vb.cuda, &derr);
    }
    if (open_cuda_frame_b.Valid()) {
      std::string derr;
      (void)open_cuda_frame_b.Free(&open_cuda_vb.cuda, &derr);
    }
  }
  open_cuda_frame_a = studiocast::cuda::CudaImage{};
  open_cuda_frame_b = studiocast::cuda::CudaImage{};
  open_cuda_curr = nullptr;
  open_cuda_next = nullptr;
  open_cuda_uploaded_this_frame = false;

  {
    std::lock_guard<std::mutex> lock(mu_);
    frame_index_ = frameIndex;
    running_ = false;

    if (!start_notified_) {
      start_notified_ = true;
      cv_.notify_all();
    }
  }
}

}  // namespace studiocast::video
