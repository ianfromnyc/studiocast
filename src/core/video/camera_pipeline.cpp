#include "camera_pipeline.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>
#include <set>
#include <sstream>
#include <vector>

#include "core/maxine/availability.h"
#include "core/maxine/maxine_manager.h"
#include "core/maxine/effects/ar_eye_contact_effect.h"
#include "core/maxine/effects/ar_auto_frame_tracker.h"
#include "core/maxine/effects/vfx_background_blur_effect.h"
#include "core/maxine/effects/vfx_green_screen_effect.h"
#include "core/maxine/effects/vfx_relighting_effect.h"
#include "core/maxine/cuda_crop_scale.h"
#include "core/maxine/cuda_driver_api.h"
#include "core/maxine/cuda_vignette.h"
#include "core/maxine/nvcv_api.h"
#include "core/maxine/vfx_api.h"
#include "core/video/convert.h"
#include "core/video/capture_error_policy.h"
#include "core/video/image_ppm.h"
#include "core/video/mjpeg_decode.h"
#include "core/video/effects/effect_chain.h"
#include "core/video/effects/broadcast_effect_contract.h"
#include "core/video/effects/broadcast_effect_rules.h"
#include "core/video/effects/mirror_effect.h"
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
  } else {
    // Avoid exposing stale negotiated formats when the pipeline is idle.
    s.capture = CaptureFormat{};
    s.output = ActualFormat{};
  }
  s.frame_index = frame_index_;
  s.effects_backends = effects_backends_;
  s.effects_note = effects_note_;
  s.last_error = last_error_;
  return s;
}

bool CameraPipeline::Start(const CameraPipelineConfig& cfg, std::string* error) {
  if (cfg.width <= 0 || cfg.height <= 0) {
    if (error) *error = "Invalid width/height.";
    return false;
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
  frame_index_ = 0;

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
                                     bool* out_opened_or_renegotiated,
                                     std::string* error) {
  if (out_opened_or_renegotiated) *out_opened_or_renegotiated = false;

  // Try to reuse an existing open writer when possible.
  if (writer_.IsOpen() && writer_device_ == outDev) {
    const auto& a = writer_.Actual();
    if (a.width == width && a.height == height && a.fps == fps) {
      output_ = a;
      output_device_ = outDev;
      return true;
    }

    // If format/dimensions changed, we need to renegotiate.
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
  if (cfg.width <= 0 || cfg.height <= 0) {
    if (error) *error = "Invalid width/height.";
    return false;
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

  bool opened_or_renegotiated = false;
  std::string oerr;
  if (!OpenOutputLocked(outDev, cfg.width, cfg.height, cfg.fps, &opened_or_renegotiated, &oerr)) {
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
      return;
    }
  }

  toJoin.join();

  std::lock_guard<std::mutex> lock(mu_);
  running_ = false;
  starting_ = false;
  capture_ = CaptureFormat{};
  output_ = ActualFormat{};
}

void CameraPipeline::ThreadMain(CameraPipelineConfig cfg) {
  // Open input capture.
  V4l2Capture cap;
  std::string inDev = cfg.input_device;
  std::ostringstream inAttempts;

  if (!inDev.empty()) {
    std::string cerr;
    if (!cap.Open(inDev,
                  cfg.width,
                  cfg.height,
                  cfg.fps,
                  CapturePixelFormat::yuyv,
                  cfg.prefer_mjpeg,
                  &cerr)) {
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
      if (cap.Open(d.dev_node,
                   cfg.width,
                   cfg.height,
                   cfg.fps,
                   CapturePixelFormat::yuyv,
                   cfg.prefer_mjpeg,
                   &cerr)) {
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
    // Keep output size aligned with the configured (requested) width/height.
    // Many webcams silently negotiate down when requesting YUYV at HD sizes; if
    // we switch the loopback format to that smaller negotiated size, consumers
    // (OBS/preview) that still request the configured size can display the frame
    // in the top-left quadrant.
    if (!OpenOutputLocked(outDev, cfg.width, cfg.height, cfg.fps, &opened_or_renegotiated, &oerr)) {
      last_error_ = oerr;
      running_ = false;
      start_notified_ = true;
      cv_.notify_all();
      return;
    }
  }

  const auto outA = writer_.Actual();

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
  struct GpuBgrOutputRef {
    const studiocast::maxine::NvCVImage* img = nullptr;  // non-owning
    studiocast::maxine::CUstream stream = nullptr;
    studiocast::maxine::CudaDriverApi* cuda = nullptr;   // non-owning
    studiocast::maxine::NvcvApi* nvcv = nullptr;         // non-owning
  };

  // GPU resize cache (allocated on demand when we can defer readback).
  studiocast::maxine::CudaBgrResizeBilinear gpu_resize_bilinear;
  studiocast::maxine::CudaDriverApi* gpu_resize_cuda = nullptr;  // non-owning
  studiocast::maxine::NvcvApi* gpu_resize_nvcv = nullptr;        // non-owning
  studiocast::maxine::NvCVImage gpu_bgr_scaled{};
  bool gpu_bgr_scaled_allocated = false;
  std::vector<std::uint8_t> bgr_scaled_out;
  studiocast::maxine::NvCVImage cpu_bgr_scaled{};

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
        if (!LoadPpmP6Rgb24(img_path, &iw, &ih, &tmp_replace_rgb_src, &img_err)) {
          if (error) *error = "Failed to load replace image (PPM/P6 required): " + img_err;
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
                         GpuBgrOutputRef* deferred_out) {
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
          deferred_out->img = out_gpu;
          deferred_out->stream = blur->cuda_stream();
          deferred_out->cuda = &cuda;
          deferred_out->nvcv = &nvcv;
          return true;
        }

        const auto down = nvcv.f().NvCVImage_Transfer(out_gpu, &cpu_bgr_out, 1.0f, blur->cuda_stream(), nullptr);
        if (down != studiocast::maxine::NVCV_SUCCESS) {
          if (error) *error = "NvCVImage_Transfer(gpu->cpu) failed: " + std::to_string(down);
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
          deferred_out->img = &gpu_bgr_out_img;
          deferred_out->stream = stream;
          deferred_out->cuda = &cuda;
          deferred_out->nvcv = &nvcv;
          return true;
        }

        const auto down = nvcv.f().NvCVImage_Transfer(&gpu_bgr_out_img, &cpu_bgr_out, 1.0f, stream, nullptr);
        if (down != studiocast::maxine::NVCV_SUCCESS) {
          if (error) *error = "NvCVImage_Transfer(gpu->cpu) failed: " + std::to_string(down);
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
                         GpuBgrOutputRef* deferred_out) {
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
        deferred_out->img = &gpu_bgr_out_img;
        deferred_out->stream = stream;
        deferred_out->cuda = &cuda;
        deferred_out->nvcv = &nvcv;
        return true;
      }

      const auto down = nvcv.f().NvCVImage_Transfer(&gpu_bgr_out_img, &cpu_bgr_out, 1.0f, stream, nullptr);
      if (down != studiocast::maxine::NVCV_SUCCESS) {
        if (error) *error = "NvCVImage_Transfer(gpu->cpu) failed: " + std::to_string(down);
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

      bgr_in.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3u);
      bgr_out.resize(bgr_in.size());

      const auto init_in = nvcv.f().NvCVImage_Init(&cpu_bgr_in,
                                                  width,
                                                  height,
                                                  static_cast<int>(width * 3),
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
                                                   width,
                                                   height,
                                                   static_cast<int>(width * 3),
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
                                                    width,
                                                    height,
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
                                                     width,
                                                     height,
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
                         GpuBgrOutputRef* deferred_out) {
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
        deferred_out->img = &gpu_bgr_out;
        deferred_out->stream = stream;
        deferred_out->cuda = &cuda;
        deferred_out->nvcv = &nvcv;
        return true;
      }

      // GPU -> CPU
      const auto down = nvcv.f().NvCVImage_Transfer(&gpu_bgr_out, &cpu_bgr_out, 1.0f, stream, nullptr);
      if (down != studiocast::maxine::NVCV_SUCCESS) {
        if (error) *error = "NvCVImage_Transfer(gpu->cpu) failed: " + std::to_string(down);
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

      bgr_in.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3u);
      bgr_out.resize(bgr_in.size());

      const auto init_in = nvcv.f().NvCVImage_Init(&cpu_bgr_in,
                                                  width,
                                                  height,
                                                  static_cast<int>(width * 3),
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
                                                   width,
                                                   height,
                                                   static_cast<int>(width * 3),
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
                                                    width,
                                                    height,
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
                                                     width,
                                                     height,
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
                         GpuBgrOutputRef* deferred_out) {
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
        deferred_out->img = &gpu_bgr_out;
        deferred_out->stream = stream;
        deferred_out->cuda = &cuda;
        deferred_out->nvcv = &nvcv;
        return true;
      }

      // GPU -> CPU
      const auto down = nvcv.f().NvCVImage_Transfer(&gpu_bgr_out, &cpu_bgr_out, 1.0f, stream, nullptr);
      if (down != studiocast::maxine::NVCV_SUCCESS) {
        if (error) *error = "NvCVImage_Transfer(gpu->cpu) failed: " + std::to_string(down);
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

      bgr_in.resize(static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3u);
      bgr_out.resize(bgr_in.size());

      const auto init_in = nvcv.f().NvCVImage_Init(&cpu_bgr_in,
                                                  width,
                                                  height,
                                                  static_cast<int>(width * 3),
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
                                                   width,
                                                   height,
                                                   static_cast<int>(width * 3),
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
                                                 width,
                                                 height,
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
                         GpuBgrOutputRef* deferred_out) {
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
        deferred_out->img = &gpu_bgr;
        deferred_out->stream = nullptr;
        deferred_out->cuda = &cuda;
        deferred_out->nvcv = &nvcv;
        return true;
      }

      // GPU -> CPU
      const auto down = nvcv.f().NvCVImage_Transfer(&gpu_bgr, &cpu_bgr_out, 1.0f, /*stream=*/nullptr, nullptr);
      if (down != studiocast::maxine::NVCV_SUCCESS) {
        if (error) *error = "NvCVImage_Transfer(gpu->cpu) failed: " + std::to_string(down);
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

  bool want_maxine_relight = false;
  bool have_maxine_relight = false;

  bool want_maxine_bg_blur = false;
  bool have_maxine_bg_blur = false;

  bool want_maxine_vignette_only = false;
  bool have_maxine_vignette_only = false;

  auto rebuildChain = [&](const studiocast::video::effects::BroadcastCameraEffects& fx) {
    chain.Clear();

    const auto plan = studiocast::video::effects::BuildBroadcastEffectsPlan(fx);
    appliedPlan = plan;

    std::string note;

    if (!plan.disabled.empty()) {
      note += "Effect rules:";
      for (const auto& d : plan.disabled) {
        note += "\n - " + d.id + ": " + d.reason;
      }
    }

    auto finalize_note_and_backends = [&] {
      std::lock_guard<std::mutex> lock(mu_);
      effects_backends_ = chain.BackendSummary();
      effects_note_ = note;
      if (!note.empty()) {
        last_error_ = note;
      }
    };

    want_maxine_bg_blur = false;
    have_maxine_bg_blur = false;

    want_maxine_auto_frame = false;
    have_maxine_auto_frame = false;

    want_maxine_eye_contact = false;
    have_maxine_eye_contact = false;

    want_maxine_relight = false;
    have_maxine_relight = false;

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

    // If Maxine-backed effects are requested but are not available, stop the
    // pipeline with a canonical, actionable message.
    const bool wants_vfx =
        has(studiocast::video::effects::contract::kEffectIdVirtualBackgroundBlur) ||
        has(studiocast::video::effects::contract::kEffectIdVirtualBackgroundRemove) ||
        has(studiocast::video::effects::contract::kEffectIdVirtualBackgroundReplace) ||
        has(studiocast::video::effects::contract::kEffectIdVideoNoiseRemoval) ||
        has(studiocast::video::effects::contract::kEffectIdVirtualKeyLight);
    const bool wants_ar =
        has(studiocast::video::effects::contract::kEffectIdEyeContact) ||
        has(studiocast::video::effects::contract::kEffectIdAutoFrame);

    if (wants_vfx || wants_ar) {
      studiocast::maxine::MaxineManager mgr;
      const auto diag = mgr.Diagnose(false);
      const std::set<std::string> avail(diag.available_effects.begin(), diag.available_effects.end());

      auto set_blocked = [&](studiocast::maxine::MaxineNeed need) {
        const auto c = studiocast::maxine::BuildCanonicalMaxineBlockedCopy(diag, need);
        note = studiocast::maxine::FormatCanonicalMaxineBlockedCopy(c);
        if (note.empty()) {
          note = c.summary;
        }
        stop_.store(true);
      };

      // AR effects.
      if (!stop_.load() && has(studiocast::video::effects::contract::kEffectIdEyeContact) &&
          !avail.count(std::string(studiocast::video::effects::contract::kEffectIdEyeContact))) {
        set_blocked(studiocast::maxine::MaxineNeed::ar);
      }
      if (!stop_.load() &&
          has(studiocast::video::effects::contract::kEffectIdAutoFrame) &&
          !avail.count(std::string(studiocast::video::effects::contract::kEffectIdAutoFrame))) {
        set_blocked(studiocast::maxine::MaxineNeed::ar);
      }

      // VFX effects.
      if (!stop_.load() && has(studiocast::video::effects::contract::kEffectIdVirtualBackgroundBlur) &&
          !avail.count(std::string(studiocast::video::effects::contract::kEffectIdVirtualBackgroundBlur))) {
        set_blocked(studiocast::maxine::MaxineNeed::vfx);
      }
      if (!stop_.load() && has(studiocast::video::effects::contract::kEffectIdVirtualBackgroundRemove) &&
          !avail.count(std::string(studiocast::video::effects::contract::kEffectIdVirtualBackgroundRemove))) {
        set_blocked(studiocast::maxine::MaxineNeed::vfx);
      }
      if (!stop_.load() && has(studiocast::video::effects::contract::kEffectIdVirtualBackgroundReplace) &&
          !avail.count(std::string(studiocast::video::effects::contract::kEffectIdVirtualBackgroundReplace))) {
        set_blocked(studiocast::maxine::MaxineNeed::vfx);
      }
      if (!stop_.load() && has(studiocast::video::effects::contract::kEffectIdVideoNoiseRemoval) &&
          !avail.count(std::string(studiocast::video::effects::contract::kEffectIdVideoNoiseRemoval))) {
        set_blocked(studiocast::maxine::MaxineNeed::vfx);
      }
      if (!stop_.load() && has(studiocast::video::effects::contract::kEffectIdVirtualKeyLight) &&
          !avail.count(std::string(studiocast::video::effects::contract::kEffectIdVirtualKeyLight))) {
        set_blocked(studiocast::maxine::MaxineNeed::vfx);
      }

      if (stop_.load()) {
        finalize_note_and_backends();
        appliedFx = fx;
        return;
      }
    }

    // Eye Contact (AR)
    if (has(studiocast::video::effects::contract::kEffectIdEyeContact)) {
      want_maxine_eye_contact = true;

      std::string mx_err;
      if (maxine_eye_contact.EnsureInitialized(capA.width, capA.height, fx, &mx_err)) {
        have_maxine_eye_contact = true;
        if (!note.empty()) note += "\n";
        note += "Maxine AR: Eye Contact.";
      } else {
        if (!note.empty()) note += "\n";
        note += mx_err;
        // No CPU fallback when Maxine is not available.
        stop_.store(true);
      }
    }

    // Background effects.
    if (has(studiocast::video::effects::contract::kEffectIdVirtualBackgroundBlur) ||
        has(studiocast::video::effects::contract::kEffectIdVirtualBackgroundRemove) ||
        has(studiocast::video::effects::contract::kEffectIdVirtualBackgroundReplace)) {

      // Maxine-only. (auto_select means "use Maxine if available; otherwise unavailable").
      // StudioCast no longer runs CPU placeholder background effects in the camera pipeline.
      want_maxine_bg_blur = true;

      std::string mx_err;
      if (maxine_bg_blur.EnsureInitialized(capA.width, capA.height, fx, &mx_err)) {
        have_maxine_bg_blur = true;
        if (fx.virtual_background.mode == studiocast::video::effects::VirtualBackgroundMode::blur) {
          if (!note.empty()) note += "\n";
          note += "Maxine VFX: Green Screen matte + Background Blur.";
        } else if (fx.virtual_background.mode == studiocast::video::effects::VirtualBackgroundMode::remove) {
          if (!note.empty()) note += "\n";
          note += "Maxine VFX: Green Screen matte + Composite (remove).";
        } else {
          if (!note.empty()) note += "\n";
          note += "Maxine VFX: Green Screen matte + Composite (replace).";
        }
      } else {
        if (!note.empty()) note += "\n";
        note += mx_err;
        // No CPU fallback when Maxine is not available.
        stop_.store(true);
      }

    } else if (has(studiocast::video::effects::contract::kEffectIdAutoFrame)) {
      // Auto Frame is Maxine-only (AR + GPU crop/scale).
      want_maxine_auto_frame = true;

      std::string mx_err;
      if (maxine_auto_frame.EnsureInitialized(capA.width, capA.height, fx, &mx_err)) {
        have_maxine_auto_frame = true;
        if (!note.empty()) note += "\n";
        note += "Maxine AR: Auto Frame (FaceBoxDetection + CUDA crop/scale).";
      } else {
        if (!note.empty()) note += "\n";
        note += mx_err;
        // No fallback.
        stop_.store(true);
      }
    }

    // Virtual Key Light (Maxine-only).
    if (has(studiocast::video::effects::contract::kEffectIdVirtualKeyLight)) {
      want_maxine_relight = true;
      std::string mx_err;
      if (maxine_relight.EnsureInitialized(capA.width, capA.height, fx, &mx_err)) {
        have_maxine_relight = true;
        if (!note.empty()) note += " ";
        note += "Maxine VFX: Video Relighting (Virtual Key Light).";
      } else {
        if (!note.empty()) note += " ";
        note += mx_err;
        // No fallback.
        stop_.store(true);
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
          if (!note.empty()) note += "\n";
          note += "CUDA: Vignette.";
        } else {
          if (!note.empty()) note += "\n";
          note += "Vignette unavailable: " + mx_err;
          // No fallback.
          stop_.store(true);
        }
      }
    }

    // Mirror (CPU) last.
    if (has(studiocast::video::effects::contract::kEffectIdMirror)) {
      chain.Add(std::make_unique<studiocast::video::effects::MirrorEffect>());
    }

    {
      std::lock_guard<std::mutex> lock(mu_);
      std::string backends = chain.BackendSummary();
      if (have_maxine_bg_blur) {
        if (!backends.empty()) backends += ",";
        backends += "virtual_background." +
                    studiocast::video::effects::ToString(fx.virtual_background.mode) + ":maxine";
      }
      if (have_maxine_relight) {
        if (!backends.empty()) backends += ",";
        backends += "virtual_key_light:maxine";
      }
      if (have_maxine_eye_contact) {
        if (!backends.empty()) backends += ",";
        backends += "eye_contact:maxine_ar";
      }
      if (have_maxine_auto_frame) {
        if (!backends.empty()) backends += ",";
        backends += "auto_frame:maxine_ar_cuda";
      }
      if (have_maxine_vignette_only) {
        if (!backends.empty()) backends += ",";
        backends += "vignette:cuda";
      }
      effects_backends_ = backends;
      effects_note_ = note;
    }

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

    GpuBgrOutputRef deferred_gpu_out{};
    bool have_deferred_gpu_out = false;

    // Apply effects (in-place on RGB buffer).
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
      // - no CPU tail effects (mirror) are active
      // This allows us to resize on GPU and perform a single GPU->CPU transfer for output.
      std::string last_stage_for_defer;
      for (auto it = plan.ordered_effect_ids.rbegin(); it != plan.ordered_effect_ids.rend(); ++it) {
        if (*it == studiocast::video::effects::contract::kEffectIdMirror) continue;
        if (*it == studiocast::video::effects::contract::kEffectIdVideoNoiseRemoval) continue;
        last_stage_for_defer = *it;
        break;
      }
      const bool mirror_enabled = has_stage(studiocast::video::effects::contract::kEffectIdMirror);
      const bool scaling_needed = (capA.width != outA.width || capA.height != outA.height);
      const bool allow_defer_readback = scaling_needed && !mirror_enabled && !last_stage_for_defer.empty();

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

      auto apply_stage = [&](const std::string& stage_id) {
        const bool defer_readback = allow_defer_readback && (stage_id == last_stage_for_defer);

        // Note: video noise removal exists in the effect schema but is not
        // currently applied in the pipeline.
        if (stage_id == studiocast::video::effects::contract::kEffectIdVideoNoiseRemoval) {
          return;
        }

        if (stage_id == studiocast::video::effects::contract::kEffectIdEyeContact) {
          if (!have_maxine_eye_contact) return;
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
          if (defer_readback && deferred_gpu_out.img) have_deferred_gpu_out = true;
          return;
        }

        if (stage_id == studiocast::video::effects::contract::kEffectIdVirtualKeyLight) {
          if (!have_maxine_relight) return;
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
          if (defer_readback && deferred_gpu_out.img) have_deferred_gpu_out = true;
          return;
        }

        if (stage_id == studiocast::video::effects::contract::kEffectIdVirtualBackgroundBlur ||
            stage_id == studiocast::video::effects::contract::kEffectIdVirtualBackgroundRemove ||
            stage_id == studiocast::video::effects::contract::kEffectIdVirtualBackgroundReplace) {
          if (!have_maxine_bg_blur) return;
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
          if (defer_readback && deferred_gpu_out.img) have_deferred_gpu_out = true;
          return;
        }

        if (stage_id == studiocast::video::effects::contract::kEffectIdAutoFrame) {
          if (!have_maxine_auto_frame) return;
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
          if (defer_readback && deferred_gpu_out.img) have_deferred_gpu_out = true;
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
          if (defer_readback && deferred_gpu_out.img) have_deferred_gpu_out = true;
          return;
        }
      };

      for (const auto& stage_id : plan.ordered_effect_ids) {
        if (stage_id == studiocast::video::effects::contract::kEffectIdMirror) continue;
        apply_stage(stage_id);
        if (fx_failed) break;
      }

      // CPU-only tail effects (e.g. mirror) run last.
      if (!fx_failed) {
        chain.Apply(view);
      }
    }

    if (fx_failed) {
      break;
    }

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

    int frameW = capA.width;
    int frameH = capA.height;

    const std::uint8_t* rgbOut = rgb.data();
    std::size_t rgbOutStride = rgbStride;
    std::size_t rgbOutBytes = rgb.size();

    // If we deferred GPU readback, perform GPU resize (bilinear) to output dimensions and
    // do a single GPU->CPU transfer for the final output buffer.
    if (have_deferred_gpu_out) {
      std::string gerr;
      bool ok = true;

      if (!deferred_gpu_out.img || !deferred_gpu_out.cuda || !deferred_gpu_out.nvcv) {
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
        if (!gpu_resize_bilinear.Resize(*deferred_gpu_out.img, &gpu_bgr_scaled, deferred_gpu_out.stream, &gerr)) {
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
        const std::size_t tightStride = static_cast<std::size_t>(outW) * 3u;
        bgr_scaled_out.resize(tightStride * static_cast<std::size_t>(outH));

        const auto init = deferred_gpu_out.nvcv->f().NvCVImage_Init(&cpu_bgr_scaled,
                                                                    outW,
                                                                    outH,
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
        // Fall back to the existing CPU resize path.
        std::lock_guard<std::mutex> lock(mu_);
        last_error_ = "GPU resize path failed (falling back to CPU resize): " + gerr;
      }
    }

    if (frameW != outW || frameH != outH) {
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

    ++frameIndex;
    if ((frameIndex % 10) == 0) {
      std::lock_guard<std::mutex> lock(mu_);
      frame_index_ = frameIndex;
    }
  }

  // Cleanup cached GPU resize output.
  if (gpu_bgr_scaled_allocated && gpu_resize_nvcv && gpu_resize_nvcv->IsInitialized() &&
      gpu_resize_nvcv->f().NvCVImage_Dealloc) {
    (void)gpu_resize_nvcv->f().NvCVImage_Dealloc(&gpu_bgr_scaled);
  }
  gpu_bgr_scaled = studiocast::maxine::NvCVImage{};
  gpu_bgr_scaled_allocated = false;

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
