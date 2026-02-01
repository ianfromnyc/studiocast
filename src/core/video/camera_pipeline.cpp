#include "camera_pipeline.h"

#include <sstream>
#include <vector>

#include "core/video/convert.h"
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

std::string ChooseDefaultInputCamera(std::string* error) {
  const auto rep = ProbeLoopback();
  for (const auto& d : rep.devices) {
    if (!d.is_loopback && d.can_read) return d.dev_node;
  }
  if (error) *error = "No readable camera device found (no non-loopback /dev/video* with read access).";
  return {};
}

}  // namespace

CameraPipeline::~CameraPipeline() { Stop(); }

void CameraPipeline::SetMirrorEnabled(bool enabled) {
  mirror_.store(enabled);
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
  mirror_.store(cfg.effects.mirror);

  last_error_.clear();
  input_device_.clear();
  output_device_.clear();
  capture_ = CaptureFormat{};
  output_ = ActualFormat{};
  frame_index_ = 0;

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
  std::string inDev = cfg.input_device;
  if (inDev.empty()) {
    std::string e;
    inDev = ChooseDefaultInputCamera(&e);
    if (inDev.empty()) {
      std::lock_guard<std::mutex> lock(mu_);
      last_error_ = e.empty() ? "No input camera found." : e;
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

  V4l2Capture cap;
  {
    std::string cerr;
    if (!cap.Open(inDev, cfg.width, cfg.height, cfg.fps, CapturePixelFormat::yuyv, &cerr)) {
      std::lock_guard<std::mutex> lock(mu_);
      last_error_ = "Failed to open capture device " + inDev + ":\n" + cerr;
      running_ = false;
      start_notified_ = true;
      cv_.notify_all();
      return;
    }
  }

  const auto capA = cap.Actual();

  // Open writer: prefer RGB24 output (avoids RGB->YUYV conversion), fallback to YUYV.
  V4l2Writer writer;
  {
    std::string werr;
    if (!writer.Open(outDev, capA.width, capA.height, capA.fps, PixelFormat::rgb24, &werr)) {
      std::string werr2;
      if (!writer.Open(outDev, capA.width, capA.height, capA.fps, PixelFormat::yuyv, &werr2)) {
        std::lock_guard<std::mutex> lock(mu_);
        last_error_ = "Failed to open v4l2loopback output " + outDev + ":\n"
                      "Tried rgb24:\n" + werr + "\n\n"
                      "Tried yuyv:\n" + werr2;
        running_ = false;
        start_notified_ = true;
        cv_.notify_all();
        return;
      }
    }
  }

  const auto outA = writer.Actual();

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

    if (mirror_.load()) {
      MirrorRgb24InPlace(rgb.data(), capA.width, capA.height, rgbStride);
    }

    // Pack/write to output format
    if (outA.format == PixelFormat::rgb24) {
      const std::size_t wantRow = static_cast<std::size_t>(capA.width) * 3u;

      if (outA.bytes_per_line == wantRow && outA.size_image == rgb.size()) {
        std::string werr;
        if (!writer.WriteFrame(rgb.data(), rgb.size(), &werr)) {
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
        if (!writer.WriteFrame(outBuf.data(), outBuf.size(), &werr)) {
          std::lock_guard<std::mutex> lock(mu_);
          last_error_ = "Write failed: " + werr;
          break;
        }
      }
    } else {
      // Output is YUYV; convert RGB -> YUYV into outBuf with output stride.
      Rgb24ToYuyv(rgb.data(), capA.width, capA.height, rgbStride, outBuf.data(), outA.bytes_per_line);

      std::string werr;
      if (!writer.WriteFrame(outBuf.data(), outBuf.size(), &werr)) {
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
