#include "camera_pipeline.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <memory>
#include <sstream>
#include <vector>

#include "core/maxine/availability.h"
#include "core/maxine/effects/vfx_background_blur_effect.h"
#include "core/maxine/effects/vfx_green_screen_effect.h"
#include "core/maxine/effects/vfx_relighting_effect.h"
#include "core/maxine/nvcv_api.h"
#include "core/maxine/vfx_api.h"
#include "core/video/convert.h"
#include "core/video/image_ppm.h"
#include "core/video/effects/background_blur_cpu.h"
#include "core/video/effects/background_remove_cpu.h"
#include "core/video/effects/effect_chain.h"
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

void CameraPipeline::SetEffects(const CameraEffects& effects) {
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
  s.capture = capture_;
  s.output = output_;
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

bool CameraPipeline::OpenOutputLocked(const std::string& outDev, int width, int height, int fps, std::string* error) {
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

  std::string oerr;
  if (!OpenOutputLocked(outDev, cfg.width, cfg.height, cfg.fps, &oerr)) {
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
  if (writer_.IsOpen()) {
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
      return;
    }
  }

  toJoin.join();

  std::lock_guard<std::mutex> lock(mu_);
  running_ = false;
  starting_ = false;
}

void CameraPipeline::ThreadMain(CameraPipelineConfig cfg) {
  // Open input capture.
  V4l2Capture cap;
  std::string inDev = cfg.input_device;
  std::ostringstream inAttempts;

  if (!inDev.empty()) {
    std::string cerr;
    if (!cap.Open(inDev, cfg.width, cfg.height, cfg.fps, CapturePixelFormat::yuyv, &cerr)) {
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
      if (cap.Open(d.dev_node, cfg.width, cfg.height, cfg.fps, CapturePixelFormat::yuyv, &cerr)) {
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
      oss << "Failed to auto-select a usable camera (YUYV).\n";
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
    std::string oerr;
    if (!OpenOutputLocked(outDev, capA.width, capA.height, capA.fps, &oerr)) {
      last_error_ = oerr;
      running_ = false;
      start_notified_ = true;
      cv_.notify_all();
      return;
    }
  }

  const auto outA = writer_.Actual();

  const std::size_t rgbStride = static_cast<std::size_t>(capA.width) * 3u;
  std::vector<std::uint8_t> rgb(rgbStride * static_cast<std::size_t>(capA.height));

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
  CameraEffects appliedFx{};

  struct MaxineBackgroundBlurContext {
    bool initialized = false;
    bool enabled = false;
    std::string last_error;

    studiocast::maxine::vfx::VfxApi vfx;
    studiocast::maxine::NvcvApi nvcv;

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

    bool EnsureInitialized(int width, int height, const CameraEffects& fx, std::string* error) {
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
                             const CameraEffects& fx,
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

      const bool wantReplace = (fx.background == studiocast::video::effects::BackgroundEffect::replace);

      bool needsUpload = false;
      if (wantReplace) {
        if (!cached_bg_is_replace || cached_bg_path != fx.background_replace_image) {
          needsUpload = true;
        }
      } else {
        // remove mode uses a solid color
        if (cached_bg_is_replace || cached_bg_color_rgb != (fx.background_remove_color_rgb & 0xFFFFFFu)) {
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
        if (fx.background_replace_image.empty()) {
          if (error) *error = "background_replace_image not set.";
          return false;
        }

        int iw = 0, ih = 0;
        std::string img_err;
        if (!LoadPpmP6Rgb24(fx.background_replace_image, &iw, &ih, &tmp_replace_rgb_src, &img_err)) {
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
        cached_bg_path = fx.background_replace_image;
      } else {
        const std::uint32_t rgb = fx.background_remove_color_rgb & 0xFFFFFFu;
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
                         const CameraEffects& fx,
                         std::string* error) {
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

      if (fx.background == studiocast::video::effects::BackgroundEffect::blur) {
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

      if (fx.background == studiocast::video::effects::BackgroundEffect::blur) {
        const auto st2 = blur->Process(frame, &proc_err);
        if (st2 != studiocast::maxine::NVCV_SUCCESS) {
          if (error) *error = proc_err.empty() ? std::to_string(st2) : proc_err;
          return false;
        }

        const auto* out_gpu = blur->OutputGpu();
        if (!out_gpu) {
          if (error) *error = "Background Blur did not produce an output image.";
          return false;
        }

        const auto down = nvcv.f().NvCVImage_Transfer(out_gpu, &cpu_bgr_out, 1.0f, blur->cuda_stream(), nullptr);
        if (down != studiocast::maxine::NVCV_SUCCESS) {
          if (error) *error = "NvCVImage_Transfer(gpu->cpu) failed: " + std::to_string(down);
          return false;
        }
      } else if (fx.background == studiocast::video::effects::BackgroundEffect::remove ||
                 fx.background == studiocast::video::effects::BackgroundEffect::replace) {
        if (!nvcv.f().NvCVImage_Composite) {
          if (error) *error = "NvCVImage_Composite unavailable.";
          return false;
        }

        const auto stream = greenscreen->cuda_stream();
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

    bool ResolveHdriPath(const CameraEffects& fx, std::filesystem::path* out, std::string* error) {
      if (!out) return false;
      out->clear();

      // User override.
      if (!fx.virtual_key_light.hdri_path.empty()) {
        std::error_code ec;
        if (std::filesystem::exists(fx.virtual_key_light.hdri_path, ec)) {
          *out = fx.virtual_key_light.hdri_path;
          return true;
        }
        if (error) *error = "Virtual Key Light HDRI path does not exist: " + fx.virtual_key_light.hdri_path.string();
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

    bool EnsureInitialized(int width, int height, const CameraEffects& fx, std::string* error) {
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

        CameraEffects tmp = fx;
        tmp.virtual_key_light.hdri_path = hdri;

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

      CameraEffects tmp = fx;
      tmp.virtual_key_light.hdri_path = hdri;

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
                         const CameraEffects& fx,
                         std::string* error) {
      if (!initialized || !greenscreen || !relight) {
        if (error) *error = "Maxine relighting not initialized.";
        return false;
      }
      if (!rgb || width <= 0 || height <= 0 || rgb_stride == 0) {
        if (error) *error = "Invalid RGB buffer.";
        return false;
      }

      const float intensity = std::max(0.0f, std::min(1.0f, fx.virtual_key_light.intensity));
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

      CameraEffects tmp = fx;
      tmp.virtual_key_light.hdri_path = hdri;

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

  bool want_maxine_relight = false;
  bool have_maxine_relight = false;

  bool want_maxine_bg_blur = false;
  bool have_maxine_bg_blur = false;

  auto rebuildChain = [&](const CameraEffects& fx) {
    chain.Clear();

    std::string note;

    want_maxine_bg_blur = false;
    have_maxine_bg_blur = false;

    want_maxine_relight = false;
    have_maxine_relight = false;

    // Mirror
    if (fx.mirror) {
      chain.Add(std::make_unique<studiocast::video::effects::MirrorEffect>());
    }

    // Background effects.
    if (fx.background == studiocast::video::effects::BackgroundEffect::blur ||
        fx.background == studiocast::video::effects::BackgroundEffect::remove ||
        fx.background == studiocast::video::effects::BackgroundEffect::replace) {

      const bool forceCpu = (fx.background_backend == studiocast::video::effects::EffectBackend::cpu);

      if (!forceCpu) {
        // Maxine-only (auto_select means "use Maxine if available; otherwise unavailable").
        want_maxine_bg_blur = true;

        std::string mx_err;
        if (maxine_bg_blur.EnsureInitialized(capA.width, capA.height, fx, &mx_err)) {
          have_maxine_bg_blur = true;
          if (fx.background == studiocast::video::effects::BackgroundEffect::blur) {
            note = "Maxine VFX: Green Screen matte + Background Blur.";
          } else if (fx.background == studiocast::video::effects::BackgroundEffect::remove) {
            note = "Maxine VFX: Green Screen matte + Composite (remove).";
          } else {
            note = "Maxine VFX: Green Screen matte + Composite (replace).";
          }
        } else {
          note = "Maxine virtual background unavailable: " + mx_err;
          // No CPU fallback when Maxine is not available.
          stop_.store(true);
        }

      } else {
        // CPU backend explicitly selected: keep placeholders only where they exist.
        if (fx.background == studiocast::video::effects::BackgroundEffect::blur) {
          chain.Add(std::make_unique<studiocast::video::effects::BackgroundBlurCpuEffect>(fx.background_strength));
          note = "CPU placeholder: center-focus mask (no AI segmentation yet).";
        } else if (fx.background == studiocast::video::effects::BackgroundEffect::remove) {
          chain.Add(std::make_unique<studiocast::video::effects::BackgroundRemoveCpuEffect>());
          note = "CPU placeholder: center-focus mask (no AI segmentation yet).";
        } else {
          note = "CPU background replace not supported.";
          stop_.store(true);
        }
      }

    } else if (fx.background == studiocast::video::effects::BackgroundEffect::auto_frame) {
      // Not implemented yet.
      note = "Auto Frame is not implemented yet.";
    }

    // Virtual Key Light (Maxine-only).
    if (fx.virtual_key_light.enabled) {
      want_maxine_relight = true;
      std::string mx_err;
      if (maxine_relight.EnsureInitialized(capA.width, capA.height, fx, &mx_err)) {
        have_maxine_relight = true;
        if (!note.empty()) note += " ";
        note += "Maxine VFX: Video Relighting (Virtual Key Light).";
      } else {
        if (!note.empty()) note += " ";
        note += "Virtual Key Light unavailable: " + mx_err;
        // No fallback.
        stop_.store(true);
      }
    }

    {
      std::lock_guard<std::mutex> lock(mu_);
      std::string backends = chain.BackendSummary();
      if (have_maxine_bg_blur) {
        if (!backends.empty()) backends += ",";
        backends += "virtual_background." + studiocast::video::effects::ToString(fx.background) + ":maxine";
      }
      if (have_maxine_relight) {
        if (!backends.empty()) backends += ",";
        backends += "virtual_key_light:maxine";
      }
      effects_backends_ = backends;
      effects_note_ = note;
    }

    appliedFx = fx;
  };

  // Initial chain based on config at pipeline start.
  {
    CameraEffects fx;
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
      // timeouts can happen; treat as recoverable unless stop requested
      if (stop_.load()) break;
      std::lock_guard<std::mutex> lock(mu_);
      last_error_ = "Capture acquire failed: " + ferr;
      break;
    }

    // Convert capture YUYV -> internal RGB (tight stride)
    YuyvToRgb24(f.data, capA.width, capA.height, capA.bytes_per_line, rgb.data(), rgbStride);

    std::string rerr;
    (void)cap.ReleaseFrame(f, &rerr);

    // Apply effects (in-place on RGB buffer).
    bool fx_failed = false;
    {
      CameraEffects fx;
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

      chain.Apply(view);

      // Maxine Virtual Background = Blur (GPU): Green Screen matte -> Background Blur.
      if (have_maxine_bg_blur) {
        std::string mx_err;
        if (!maxine_bg_blur.ApplyRgbInPlace(rgb.data(), capA.width, capA.height, rgbStride, fx, &mx_err)) {
          {
            std::lock_guard<std::mutex> lock(mu_);
            last_error_ = "Maxine background blur failed: " + mx_err;
          }
          fx_failed = true;
        }
      }

      // Maxine Virtual Key Light (GPU): Green Screen matte -> Video Relighting -> Composite.
      if (have_maxine_relight) {
        std::string mx_err;
        if (!maxine_relight.ApplyRgbInPlace(rgb.data(), capA.width, capA.height, rgbStride, fx, &mx_err)) {
          {
            std::lock_guard<std::mutex> lock(mu_);
            last_error_ = "Maxine relighting failed: " + mx_err;
          }
          fx_failed = true;
        }
      }
    }

    if (fx_failed) {
      break;
    }

    // Pack/write to output format
    if (outA.format == PixelFormat::rgb24) {
      const std::size_t wantRow = static_cast<std::size_t>(capA.width) * 3u;

      if (outA.bytes_per_line == wantRow && outA.size_image == rgb.size()) {
        std::string werr;
        if (!writer_.WriteFrame(rgb.data(), rgb.size(), &werr)) {
          std::lock_guard<std::mutex> lock(mu_);
          last_error_ = "Write failed: " + werr;
          break;
        }
      } else {
        // Copy into padded output buffer
        for (int y = 0; y < capA.height; ++y) {
          const std::uint8_t* srcRow = rgb.data() + static_cast<std::size_t>(y) * rgbStride;
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
      Rgb24ToYuyv(rgb.data(), capA.width, capA.height, rgbStride, outBuf.data(), outA.bytes_per_line);

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
