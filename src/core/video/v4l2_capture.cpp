#include "v4l2_capture.h"

#include <algorithm>
#include <cmath>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <optional>
#include <sstream>

#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace studiocast::video {
namespace {

int IoctlRetry(int fd, unsigned long req, void* arg) {
  for (;;) {
    const int r = ::ioctl(fd, req, arg);
    if (r == 0) return 0;
    if (errno == EINTR) continue;
    return -1;
  }
}

std::string FourccToString(std::uint32_t f) {
  char s[5];
  s[0] = static_cast<char>(f & 0xFFu);
  s[1] = static_cast<char>((f >> 8) & 0xFFu);
  s[2] = static_cast<char>((f >> 16) & 0xFFu);
  s[3] = static_cast<char>((f >> 24) & 0xFFu);
  s[4] = '\0';
  return std::string(s);
}

std::uint32_t FourccFor(CapturePixelFormat fmt) {
  switch (fmt) {
    case CapturePixelFormat::yuyv: return V4L2_PIX_FMT_YUYV;
    case CapturePixelFormat::mjpeg: return V4L2_PIX_FMT_MJPEG;
    case CapturePixelFormat::rgb24: return V4L2_PIX_FMT_RGB24;
  }
  return V4L2_PIX_FMT_YUYV;
}

std::string CapturePixelFormatLabel(CapturePixelFormat fmt) {
  switch (fmt) {
    case CapturePixelFormat::yuyv: return "YUYV";
    case CapturePixelFormat::mjpeg: return "MJPG";
    case CapturePixelFormat::rgb24: return "RGB24";
  }
  return "UNKNOWN";
}

bool ParseCapturePixelFormat(std::uint32_t fourcc, CapturePixelFormat* out) {
  if (!out) return false;
  if (fourcc == V4L2_PIX_FMT_YUYV) {
    *out = CapturePixelFormat::yuyv;
    return true;
  }
  if (fourcc == V4L2_PIX_FMT_MJPEG || fourcc == V4L2_PIX_FMT_JPEG) {
    *out = CapturePixelFormat::mjpeg;
    return true;
  }
  if (fourcc == V4L2_PIX_FMT_RGB24) {
    *out = CapturePixelFormat::rgb24;
    return true;
  }
  return false;
}

std::vector<std::uint32_t> EnumeratePixelFormats(int fd, unsigned int type) {
  std::vector<std::uint32_t> out;
  v4l2_fmtdesc desc{};
  desc.type = type;

  for (desc.index = 0; IoctlRetry(fd, VIDIOC_ENUM_FMT, &desc) == 0; ++desc.index) {
    if (desc.pixelformat == 0) continue;
    // Deduplicate while preserving order.
    bool exists = false;
    for (const auto f : out) {
      if (f == desc.pixelformat) {
        exists = true;
        break;
      }
    }
    if (!exists) out.push_back(desc.pixelformat);
  }
  return out;
}

bool SupportsFourcc(const std::vector<std::uint32_t>& fmts, std::uint32_t fourcc) {
  for (const auto f : fmts) {
    if (f == fourcc) return true;
  }
  return false;
}

struct TypeSpec {
  unsigned int type = 0;
  bool mplane = false;
};

struct Mode {
  CapturePixelFormat fmt{};
  std::uint32_t fourcc = 0;
  int width = 0;
  int height = 0;

  // Best known maximum FPS for this mode, based on `VIDIOC_ENUM_FRAMEINTERVALS`.
  // `fps_max <= 0` means unknown.
  double fps_max = 0.0;
  int fps_max_num = 0;
  int fps_max_den = 0;
};

struct FpsInfo {
  double fps_max = 0.0;
  int num = 0;
  int den = 0;
};

double FpsFromFraction(int num, int den) {
  if (num <= 0 || den <= 0) return 0.0;
  return static_cast<double>(den) / static_cast<double>(num);
}

void AppendUnique(std::vector<std::uint32_t>* out, const std::vector<std::uint32_t>& in) {
  if (!out) return;
  for (const auto v : in) {
    bool exists = false;
    for (const auto e : *out) {
      if (e == v) {
        exists = true;
        break;
      }
    }
    if (!exists) out->push_back(v);
  }
}

void AppendUnique(std::vector<std::pair<int, int>>* out, const std::pair<int, int>& v) {
  if (!out) return;
  for (const auto& e : *out) {
    if (e == v) return;
  }
  out->push_back(v);
}

std::vector<std::uint32_t> EnumeratePixelFormatsAnyType(int fd) {
  std::vector<std::uint32_t> out;

  const TypeSpec types[] = {
      {V4L2_BUF_TYPE_VIDEO_CAPTURE, false},
#ifdef V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
      {V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, true},
#endif
  };

  for (const auto& t : types) {
    AppendUnique(&out, EnumeratePixelFormats(fd, t.type));
  }
  return out;
}

std::vector<std::pair<int, int>> EnumerateFrameSizes(int fd, std::uint32_t fourcc) {
  std::vector<std::pair<int, int>> out;

  v4l2_frmsizeenum e{};
  e.pixel_format = fourcc;

  for (e.index = 0; IoctlRetry(fd, VIDIOC_ENUM_FRAMESIZES, &e) == 0; ++e.index) {
    if (e.type == V4L2_FRMSIZE_TYPE_DISCRETE) {
      const int w = static_cast<int>(e.discrete.width);
      const int h = static_cast<int>(e.discrete.height);
      if (w > 0 && h > 0) AppendUnique(&out, {w, h});
      continue;
    }

    if (e.type == V4L2_FRMSIZE_TYPE_STEPWISE || e.type == V4L2_FRMSIZE_TYPE_CONTINUOUS) {
      const int minW = static_cast<int>(e.stepwise.min_width);
      const int maxW = static_cast<int>(e.stepwise.max_width);
      const int minH = static_cast<int>(e.stepwise.min_height);
      const int maxH = static_cast<int>(e.stepwise.max_height);
      const int stepW = std::max(1, static_cast<int>(e.stepwise.step_width));
      const int stepH = std::max(1, static_cast<int>(e.stepwise.step_height));

      auto aligned = [&](int w, int h) -> bool {
        if (w < minW || w > maxW || h < minH || h > maxH) return false;
        if (((w - minW) % stepW) != 0) return false;
        if (((h - minH) % stepH) != 0) return false;
        return true;
      };

      // Always include min/max.
      if (minW > 0 && minH > 0) AppendUnique(&out, {minW, minH});
      if (maxW > 0 && maxH > 0) AppendUnique(&out, {maxW, maxH});

      // Include common sizes if they fit.
      const std::pair<int, int> common[] = {
          {640, 480},
          {800, 600},
          {1024, 768},
          {1280, 720},
          {1280, 800},
          {1600, 900},
          {1920, 1080},
          {2560, 1440},
          {3840, 2160},
      };
      for (const auto& s : common) {
        if (aligned(s.first, s.second)) AppendUnique(&out, s);
      }

      // Only one stepwise/continuous record is expected.
      break;
    }
  }

  return out;
}

std::optional<FpsInfo> EnumerateFpsMax(int fd, std::uint32_t fourcc, int width, int height) {
  v4l2_frmivalenum e{};
  e.pixel_format = fourcc;
  e.width = static_cast<__u32>(width);
  e.height = static_cast<__u32>(height);

  bool any = false;
  FpsInfo out;

  for (e.index = 0; IoctlRetry(fd, VIDIOC_ENUM_FRAMEINTERVALS, &e) == 0; ++e.index) {
    if (e.type == V4L2_FRMIVAL_TYPE_DISCRETE) {
      const int num = static_cast<int>(e.discrete.numerator);
      const int den = static_cast<int>(e.discrete.denominator);
      const double fps = FpsFromFraction(num, den);
      if (fps <= 0.0) continue;

      if (!any || fps > out.fps_max) {
        any = true;
        out.fps_max = fps;
        out.num = num;
        out.den = den;
      }
      continue;
    }

    if (e.type == V4L2_FRMIVAL_TYPE_STEPWISE || e.type == V4L2_FRMIVAL_TYPE_CONTINUOUS) {
      // For time-per-frame, the smallest interval corresponds to the highest FPS.
      const int num = static_cast<int>(e.stepwise.min.numerator);
      const int den = static_cast<int>(e.stepwise.min.denominator);
      const double fps = FpsFromFraction(num, den);
      if (fps > 0.0) {
        any = true;
        out.fps_max = fps;
        out.num = num;
        out.den = den;
      }
      break;
    }
  }

  if (!any) return std::nullopt;
  return out;
}

std::vector<Mode> EnumerateModes(int fd) {
  std::vector<Mode> out;

  const auto formats = EnumeratePixelFormatsAnyType(fd);
  for (const auto fourcc : formats) {
    CapturePixelFormat fmt{};
    if (!ParseCapturePixelFormat(fourcc, &fmt)) continue;

    const auto sizes = EnumerateFrameSizes(fd, fourcc);
    for (const auto& s : sizes) {
      const int w = s.first;
      const int h = s.second;
      if (w <= 0 || h <= 0) continue;

      Mode m;
      m.fmt = fmt;
      m.fourcc = fourcc;
      m.width = w;
      m.height = h;

      if (const auto fi = EnumerateFpsMax(fd, fourcc, w, h)) {
        m.fps_max = fi->fps_max;
        m.fps_max_num = fi->num;
        m.fps_max_den = fi->den;
      }

      out.push_back(m);
    }
  }

  return out;
}

std::int64_t Pixels(int w, int h) {
  return static_cast<std::int64_t>(w) * static_cast<std::int64_t>(h);
}

int FormatRank(CapturePixelFormat fmt, bool prefer_mjpeg, int w, int h) {
  // Prefer MJPEG at 720p+ when enabled (USB bandwidth), otherwise prefer uncompressed.
  const bool hi = Pixels(w, h) >= (1280LL * 720LL);

  if (prefer_mjpeg && hi) {
    if (fmt == CapturePixelFormat::mjpeg) return 3;
    if (fmt == CapturePixelFormat::yuyv) return 2;
    return 1;
  }

  if (fmt == CapturePixelFormat::yuyv) return 3;
  if (fmt == CapturePixelFormat::mjpeg) return 2;
  return 1;
}

double ScoreMode(const Mode& m, int target_fps, bool prefer_mjpeg) {
  const double fps = m.fps_max;
  const double factor_unknown = 0.5;

  double fps_factor = factor_unknown;
  if (target_fps <= 0) {
    fps_factor = 1.0;
  } else if (fps > 0.0) {
    fps_factor = std::min(1.0, fps / static_cast<double>(target_fps));
  }

  const double pixels = static_cast<double>(Pixels(m.width, m.height));
  double score = pixels * fps_factor;

  // Preference tweaks.
  if (prefer_mjpeg && m.fmt == CapturePixelFormat::mjpeg && Pixels(m.width, m.height) >= (1280LL * 720LL)) {
    score += pixels * 0.10;
  }
  score += std::min(std::max(0.0, fps), 240.0) / 1000.0;

  return score;
}

bool TrySetFmtCapture(int fd,
                      const TypeSpec& t,
                      int width,
                      int height,
                      CapturePixelFormat fmt,
                      v4l2_format* outFmt,
                      std::string* outErr) {
  v4l2_format f{};
  f.type = t.type;

  if (!t.mplane) {
    f.fmt.pix.width = static_cast<__u32>(width);
    f.fmt.pix.height = static_cast<__u32>(height);
    f.fmt.pix.pixelformat = FourccFor(fmt);
    f.fmt.pix.field = V4L2_FIELD_ANY;
    f.fmt.pix.bytesperline = 0;
    f.fmt.pix.sizeimage = 0;
  } else {
#ifdef V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
    f.fmt.pix_mp.width = static_cast<__u32>(width);
    f.fmt.pix_mp.height = static_cast<__u32>(height);
    f.fmt.pix_mp.pixelformat = FourccFor(fmt);
    f.fmt.pix_mp.field = V4L2_FIELD_ANY;
    f.fmt.pix_mp.num_planes = 1;
    f.fmt.pix_mp.plane_fmt[0].bytesperline = 0;
    f.fmt.pix_mp.plane_fmt[0].sizeimage = 0;
#else
    if (outErr) *outErr = "mplane capture not supported by headers";
    return false;
#endif
  }

  auto trySet = [&](std::uint32_t fourcc) -> bool {
    if (!t.mplane) {
      f.fmt.pix.pixelformat = fourcc;
    } else {
#ifdef V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
      f.fmt.pix_mp.pixelformat = fourcc;
#else
      return false;
#endif
    }
    return IoctlRetry(fd, VIDIOC_S_FMT, &f) == 0;
  };

  if (trySet(FourccFor(fmt))) {
    if (outFmt) *outFmt = f;
    return true;
  }

  // Some drivers expose compressed capture as `V4L2_PIX_FMT_JPEG` instead of `V4L2_PIX_FMT_MJPEG`.
  if (fmt == CapturePixelFormat::mjpeg && trySet(V4L2_PIX_FMT_JPEG)) {
    if (outFmt) *outFmt = f;
    return true;
  }

  if (outErr) {
    std::ostringstream oss;
    oss << "VIDIOC_S_FMT failed: " << std::strerror(errno);
    *outErr = oss.str();
  }
  return false;
}

bool ParseChosenCaptureFmt(const v4l2_format& f,
                           bool mplane,
                           int fps,
                           int fps_num,
                           int fps_den,
                           CaptureFormat* out,
                           std::string* outErr) {
  if (!out) return false;

  int w = 0, h = 0;
  std::uint32_t fourcc = 0;
  std::size_t bpl = 0;
  std::size_t size = 0;

  CapturePixelFormat parsedFmt{};

  if (!mplane) {
    w = static_cast<int>(f.fmt.pix.width);
    h = static_cast<int>(f.fmt.pix.height);
    fourcc = f.fmt.pix.pixelformat;
    bpl = static_cast<std::size_t>(f.fmt.pix.bytesperline);
    size = static_cast<std::size_t>(f.fmt.pix.sizeimage);

    if (!ParseCapturePixelFormat(fourcc, &parsedFmt)) {
      if (outErr) *outErr = "Unsupported capture pixel format";
      return false;
    }
  } else {
#ifdef V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
    w = static_cast<int>(f.fmt.pix_mp.width);
    h = static_cast<int>(f.fmt.pix_mp.height);
    fourcc = f.fmt.pix_mp.pixelformat;
    if (f.fmt.pix_mp.num_planes < 1) {
      if (outErr) *outErr = "mplane format returned num_planes=0";
      return false;
    }
    bpl = static_cast<std::size_t>(f.fmt.pix_mp.plane_fmt[0].bytesperline);
    size = static_cast<std::size_t>(f.fmt.pix_mp.plane_fmt[0].sizeimage);

    if (!ParseCapturePixelFormat(fourcc, &parsedFmt)) {
      if (outErr) *outErr = "Unsupported capture pixel format (mplane)";
      return false;
    }
#else
    if (outErr) *outErr = "mplane capture not supported by headers";
    return false;
#endif
  }

  CaptureFormat a;
  a.width = w;
  a.height = h;
  a.fps = fps;
  a.fps_num = fps_num;
  a.fps_den = fps_den;
  a.format = parsedFmt;
  a.pixfmt_fourcc = fourcc;
  a.pixfmt = FourccToString(fourcc);

  if (a.format == CapturePixelFormat::mjpeg) {
    // Compressed: `bytes_per_line` is not meaningful and drivers often report 0.
    // Keep the negotiated values as-is.
    a.bytes_per_line = bpl;
    a.size_image = size;
  } else {
    // Provide conservative minima for uncompressed formats.
    const std::size_t bpp = (a.format == CapturePixelFormat::rgb24) ? 3u : 2u;
    const std::size_t minBpl = static_cast<std::size_t>(a.width) * bpp;
    if (bpl < minBpl) bpl = minBpl;
    a.bytes_per_line = bpl;

    const std::size_t minSize = bpl * static_cast<std::size_t>(a.height);
    if (size < minSize) size = minSize;
    a.size_image = size;
  }

  *out = a;
  return true;
}

}  // namespace

bool ShouldPreferMjpegForResolution(int width, int height) {
  // Heuristic: uncompressed YUYV at >720p tends to be unsupported or unstable on many UVC webcams,
  // while MJPEG often supports 1080p+.
  if (width <= 0 || height <= 0) return false;
  const std::int64_t pixels = static_cast<std::int64_t>(width) * static_cast<std::int64_t>(height);
  const std::int64_t yuyvLikelyMax = 1280LL * 720LL;
  return pixels > yuyvLikelyMax;
}

V4l2Capture::~V4l2Capture() { Close(); }

bool V4l2Capture::Open(const std::string& device,
                       int width,
                       int height,
                       int fps,
                       CapturePixelFormat fmt,
                       bool prefer_mjpeg,
                       std::string* error) {
  Close();

  if (device.empty()) {
    if (error) *error = "Capture device is empty.";
    return false;
  }
  if (width <= 0 || height <= 0) {
    if (error) *error = "Invalid width/height.";
    return false;
  }
  if (fps <= 0 || fps > 240) {
    if (error) *error = "Invalid fps (1..240).";
    return false;
  }

  fd_ = ::open(device.c_str(), O_RDWR | O_CLOEXEC);
  if (fd_ < 0) {
    if (error) *error = "Failed to open " + device + ": " + std::string(std::strerror(errno));
    return false;
  }

  v4l2_capability cap{};
  if (IoctlRetry(fd_, VIDIOC_QUERYCAP, &cap) != 0) {
    if (error) *error = "VIDIOC_QUERYCAP failed: " + std::string(std::strerror(errno));
    Close();
    return false;
  }

  // Try capture single-plane first, then mplane.
  const TypeSpec types[] = {
      {V4L2_BUF_TYPE_VIDEO_CAPTURE, false},
#ifdef V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
      {V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, true},
#endif
  };

  v4l2_format chosen{};
  bool chosenMplane = false;
  bool ok = false;

  std::ostringstream attempts;

  for (const auto& t : types) {
    const auto supported = EnumeratePixelFormats(fd_, t.type);

    const bool supportsMjpeg = SupportsFourcc(supported, V4L2_PIX_FMT_MJPEG) ||
                               SupportsFourcc(supported, V4L2_PIX_FMT_JPEG);
    const bool supportsYuyv = SupportsFourcc(supported, V4L2_PIX_FMT_YUYV);

    // Only apply MJPEG preference when the caller requested the normal uncompressed YUYV path.
    // (Other callers can explicitly request other formats.)
    const bool mjpegFirst = (fmt == CapturePixelFormat::yuyv) && prefer_mjpeg && supportsMjpeg &&
                            ShouldPreferMjpegForResolution(width, height);

    std::vector<CapturePixelFormat> tryOrder;
    if (mjpegFirst) {
      tryOrder.push_back(CapturePixelFormat::mjpeg);
      tryOrder.push_back(CapturePixelFormat::yuyv);
    } else {
      tryOrder.push_back(fmt);
      // Fallback: if we asked for YUYV first and it fails, try MJPEG if enabled and supported.
      if (fmt == CapturePixelFormat::yuyv && prefer_mjpeg && supportsMjpeg) {
        tryOrder.push_back(CapturePixelFormat::mjpeg);
      }
    }

    for (const auto fTry : tryOrder) {
      if (fTry == CapturePixelFormat::mjpeg && !supportsMjpeg) continue;
      if (fTry == CapturePixelFormat::yuyv && !supportsYuyv) continue;

      std::string err;
      v4l2_format f{};
      if (TrySetFmtCapture(fd_, t, width, height, fTry, &f, &err)) {
        chosen = f;
        chosenMplane = t.mplane;
        buf_type_ = t.type;
        mplane_ = t.mplane;
        ok = true;
        break;
      }

      attempts << "Try S_FMT (" << (t.mplane ? "CAPTURE_MPLANE" : "CAPTURE")
               << ", " << CapturePixelFormatLabel(fTry) << "): "
               << (err.empty() ? "(no detail)" : err) << "\n";
    }

    if (ok) break;
  }

  if (!ok) {
    if (error) {
      std::ostringstream oss;
      oss << "Failed to set capture format on " << device << " (requested "
          << CapturePixelFormatLabel(fmt) << ").\n"
          << attempts.str()
          << "Tip: check supported formats with:\n"
          << "  v4l2-ctl --device " << device << " --list-formats-ext\n";
      *error = oss.str();
    }
    Close();
    return false;
  }

  // Set FPS (best-effort).
  v4l2_streamparm sp{};
  sp.type = buf_type_;
  sp.parm.capture.timeperframe.numerator = 1;
  sp.parm.capture.timeperframe.denominator = static_cast<__u32>(fps);
  (void)IoctlRetry(fd_, VIDIOC_S_PARM, &sp);

  int negotiatedFps = fps;
  int fpsNum = 1;
  int fpsDen = fps;
  v4l2_streamparm gp{};
  gp.type = buf_type_;
  if (IoctlRetry(fd_, VIDIOC_G_PARM, &gp) == 0) {
    const auto num = static_cast<int>(gp.parm.capture.timeperframe.numerator);
    const auto den = static_cast<int>(gp.parm.capture.timeperframe.denominator);
    if (num > 0 && den > 0) {
      fpsNum = num;
      fpsDen = den;
      const double f = static_cast<double>(den) / static_cast<double>(num);
      if (f > 0.0) negotiatedFps = static_cast<int>(std::floor(f + 0.5));
    }
  }

  // Query the active format (best-effort). Many drivers already return the negotiated
  // format via S_FMT, but G_FMT makes the intent explicit.
  v4l2_format active = chosen;
  active.type = buf_type_;
  if (IoctlRetry(fd_, VIDIOC_G_FMT, &active) == 0) {
    chosen = active;
  }

  std::string perr;
  if (!ParseChosenCaptureFmt(chosen, chosenMplane, negotiatedFps, fpsNum, fpsDen, &actual_, &perr)) {
    if (error) *error = "Capture negotiation succeeded but parsing failed: " + perr;
    Close();
    return false;
  }

  // Request buffers
  v4l2_requestbuffers req{};
  req.count = 4;
  req.type = buf_type_;
  req.memory = V4L2_MEMORY_MMAP;

  if (IoctlRetry(fd_, VIDIOC_REQBUFS, &req) != 0) {
    if (error) *error = "VIDIOC_REQBUFS failed: " + std::string(std::strerror(errno));
    Close();
    return false;
  }
  if (req.count < 2) {
    if (error) *error = "Insufficient V4L2 buffers allocated (need >=2).";
    Close();
    return false;
  }

  buffers_.resize(static_cast<std::size_t>(req.count));

  // Map & queue buffers
  for (unsigned int i = 0; i < req.count; ++i) {
    if (!mplane_) {
      v4l2_buffer b{};
      b.type = buf_type_;
      b.memory = V4L2_MEMORY_MMAP;
      b.index = i;

      if (IoctlRetry(fd_, VIDIOC_QUERYBUF, &b) != 0) {
        if (error) *error = "VIDIOC_QUERYBUF failed: " + std::string(std::strerror(errno));
        Close();
        return false;
      }

      void* start = ::mmap(nullptr,
                           static_cast<std::size_t>(b.length),
                           PROT_READ | PROT_WRITE,
                           MAP_SHARED,
                           fd_,
                           static_cast<off_t>(b.m.offset));
      if (start == MAP_FAILED) {
        if (error) *error = "mmap failed: " + std::string(std::strerror(errno));
        Close();
        return false;
      }

      buffers_[static_cast<std::size_t>(i)].start = start;
      buffers_[static_cast<std::size_t>(i)].length = static_cast<std::size_t>(b.length);

      if (IoctlRetry(fd_, VIDIOC_QBUF, &b) != 0) {
        if (error) *error = "VIDIOC_QBUF failed: " + std::string(std::strerror(errno));
        Close();
        return false;
      }
    } else {
#ifdef V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
      v4l2_plane planes[1]{};
      v4l2_buffer b{};
      b.type = buf_type_;
      b.memory = V4L2_MEMORY_MMAP;
      b.index = i;
      b.m.planes = planes;
      b.length = 1;

      if (IoctlRetry(fd_, VIDIOC_QUERYBUF, &b) != 0) {
        if (error) *error = "VIDIOC_QUERYBUF (mplane) failed: " + std::string(std::strerror(errno));
        Close();
        return false;
      }

      const std::size_t len = static_cast<std::size_t>(planes[0].length);
      const off_t off = static_cast<off_t>(planes[0].m.mem_offset);

      void* start = ::mmap(nullptr, len, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, off);
      if (start == MAP_FAILED) {
        if (error) *error = "mmap (mplane) failed: " + std::string(std::strerror(errno));
        Close();
        return false;
      }

      buffers_[static_cast<std::size_t>(i)].start = start;
      buffers_[static_cast<std::size_t>(i)].length = len;

      if (IoctlRetry(fd_, VIDIOC_QBUF, &b) != 0) {
        if (error) *error = "VIDIOC_QBUF (mplane) failed: " + std::string(std::strerror(errno));
        Close();
        return false;
      }
#else
      if (error) *error = "mplane capture not supported by headers";
      Close();
      return false;
#endif
    }
  }

  if (!StreamOn(error)) {
    Close();
    return false;
  }

  return true;
}

bool V4l2Capture::OpenBest(const std::string& device,
                           int target_fps,
                           bool prefer_mjpeg,
                           std::string* error) {
  Close();

  if (device.empty()) {
    if (error) *error = "Capture device is empty.";
    return false;
  }
  if (target_fps <= 0 || target_fps > 240) {
    if (error) *error = "Invalid fps (1..240).";
    return false;
  }

  const int fd = ::open(device.c_str(), O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    if (error) *error = "Failed to open " + device + ": " + std::string(std::strerror(errno));
    return false;
  }

  v4l2_capability cap{};
  if (IoctlRetry(fd, VIDIOC_QUERYCAP, &cap) != 0) {
    if (error) *error = "VIDIOC_QUERYCAP failed: " + std::string(std::strerror(errno));
    ::close(fd);
    return false;
  }

  auto modes = EnumerateModes(fd);
  ::close(fd);

  if (modes.empty()) {
    if (error) {
      std::ostringstream oss;
      oss << "Failed to enumerate any supported capture modes for " << device << ".\n"
          << "Tip: check supported formats with:\n"
          << "  v4l2-ctl --device " << device << " --list-formats-ext\n";
      *error = oss.str();
    }
    return false;
  }

  auto better = [&](const Mode& a, const Mode& b) -> bool {
    const double sa = ScoreMode(a, target_fps, prefer_mjpeg);
    const double sb = ScoreMode(b, target_fps, prefer_mjpeg);
    if (sa > sb) return true;
    if (sa < sb) return false;

    const auto pa = Pixels(a.width, a.height);
    const auto pb = Pixels(b.width, b.height);
    if (pa != pb) return pa > pb;

    const bool meetsA = (a.fps_max > 0.0) ? (a.fps_max + 1e-6 >= static_cast<double>(target_fps)) : false;
    const bool meetsB = (b.fps_max > 0.0) ? (b.fps_max + 1e-6 >= static_cast<double>(target_fps)) : false;
    if (meetsA != meetsB) return meetsA;

    const int ra = FormatRank(a.fmt, prefer_mjpeg, a.width, a.height);
    const int rb = FormatRank(b.fmt, prefer_mjpeg, b.width, b.height);
    if (ra != rb) return ra > rb;

    if (a.fps_max != b.fps_max) return a.fps_max > b.fps_max;
    if (a.width != b.width) return a.width > b.width;
    if (a.height != b.height) return a.height > b.height;
    return FourccToString(a.fourcc) < FourccToString(b.fourcc);
  };

  std::sort(modes.begin(), modes.end(), [&](const Mode& a, const Mode& b) { return better(a, b); });
  const Mode best = modes.front();

  // Deterministic one-shot debug log.
  {
    std::ostringstream oss;
    oss << "V4L2 auto_best: selected " << FourccToString(best.fourcc) << " " << best.width << "x"
        << best.height << " (max_fps=";
    if (best.fps_max > 0.0) {
      oss << best.fps_max;
    } else {
      oss << "unknown";
    }
    oss << ", target_fps=" << target_fps << ", prefer_mjpeg=" << (prefer_mjpeg ? "true" : "false")
        << ") for device " << device << "\n";

    const std::size_t n = std::min<std::size_t>(modes.size(), 8);
    for (std::size_t i = 0; i < n; ++i) {
      const auto& m = modes[i];
      oss << "  [" << i << "] " << FourccToString(m.fourcc) << " " << m.width << "x" << m.height;
      if (m.fps_max > 0.0) {
        oss << " max_fps=" << m.fps_max;
      }
      oss << " score=" << ScoreMode(m, target_fps, prefer_mjpeg) << "\n";
    }
    std::fprintf(stderr, "%s", oss.str().c_str());
  }

  // Open using the chosen mode. Disable Open()'s MJPEG heuristic to keep the selection explicit.
  std::string err;
  if (Open(device, best.width, best.height, target_fps, best.fmt, false, &err)) {
    return true;
  }

  // Fallback: try the alternate common format at the same resolution.
  if (best.fmt != CapturePixelFormat::mjpeg) {
    if (Open(device, best.width, best.height, target_fps, CapturePixelFormat::mjpeg, false, &err)) {
      return true;
    }
  }
  if (best.fmt != CapturePixelFormat::yuyv) {
    if (Open(device, best.width, best.height, target_fps, CapturePixelFormat::yuyv, false, &err)) {
      return true;
    }
  }

  if (error) {
    std::ostringstream oss;
    oss << "OpenBest selected " << FourccToString(best.fourcc) << " " << best.width << "x" << best.height
        << " but opening it failed:\n"
        << err;
    *error = oss.str();
  }
  return false;
}

bool V4l2Capture::StreamOn(std::string* error) {
  if (streaming_) return true;

  unsigned int type = buf_type_;
  if (IoctlRetry(fd_, VIDIOC_STREAMON, &type) != 0) {
    if (error) *error = "VIDIOC_STREAMON failed: " + std::string(std::strerror(errno));
    return false;
  }
  streaming_ = true;
  return true;
}

bool V4l2Capture::StreamOff(std::string* error) {
  if (!streaming_) return true;

  unsigned int type = buf_type_;
  if (IoctlRetry(fd_, VIDIOC_STREAMOFF, &type) != 0) {
    if (error) *error = "VIDIOC_STREAMOFF failed: " + std::string(std::strerror(errno));
    return false;
  }
  streaming_ = false;
  return true;
}

bool V4l2Capture::AcquireFrame(CapturedFrameView* out, int timeout_ms, std::string* error) {
  if (!out) return false;
  if (fd_ < 0) {
    if (error) *error = "Capture not open.";
    return false;
  }

  pollfd pfd{};
  pfd.fd = fd_;
  pfd.events = POLLIN;

  int pr = 0;
  for (;;) {
    pr = ::poll(&pfd, 1, timeout_ms);
    if (pr < 0 && errno == EINTR) {
      // Retry on signal interruption.
      continue;
    }
    break;
  }

  if (pr < 0) {
    if (error) *error = "poll failed: " + std::string(std::strerror(errno));
    return false;
  }
  if (pr == 0) {
    if (error) *error = "Timed out waiting for camera frame.";
    return false;
  }

  if (!mplane_) {
    v4l2_buffer b{};
    b.type = buf_type_;
    b.memory = V4L2_MEMORY_MMAP;

    if (IoctlRetry(fd_, VIDIOC_DQBUF, &b) != 0) {
      if (error) *error = "VIDIOC_DQBUF failed: " + std::string(std::strerror(errno));
      return false;
    }

    const std::size_t idx = static_cast<std::size_t>(b.index);
    if (idx >= buffers_.size()) {
      if (error) *error = "Driver returned invalid buffer index.";
      return false;
    }

    out->index = static_cast<int>(b.index);
    out->sequence = b.sequence;
    out->bytes = static_cast<std::size_t>(b.bytesused);
    out->data = static_cast<const std::uint8_t*>(buffers_[idx].start);
    return true;
  }

#ifdef V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
  v4l2_plane planes[1]{};
  v4l2_buffer b{};
  b.type = buf_type_;
  b.memory = V4L2_MEMORY_MMAP;
  b.m.planes = planes;
  b.length = 1;

  if (IoctlRetry(fd_, VIDIOC_DQBUF, &b) != 0) {
    if (error) *error = "VIDIOC_DQBUF (mplane) failed: " + std::string(std::strerror(errno));
    return false;
  }

  const std::size_t idx = static_cast<std::size_t>(b.index);
  if (idx >= buffers_.size()) {
    if (error) *error = "Driver returned invalid buffer index (mplane).";
    return false;
  }

  out->index = static_cast<int>(b.index);
  out->sequence = b.sequence;
  out->bytes = static_cast<std::size_t>(planes[0].bytesused);
  out->data = static_cast<const std::uint8_t*>(buffers_[idx].start);
  return true;
#else
  if (error) *error = "mplane capture not supported by headers";
  return false;
#endif
}

bool V4l2Capture::ReleaseFrame(const CapturedFrameView& f, std::string* error) {
  if (fd_ < 0) {
    if (error) *error = "Capture not open.";
    return false;
  }
  if (f.index < 0) return false;

  const unsigned int idx = static_cast<unsigned int>(f.index);

  if (!mplane_) {
    v4l2_buffer b{};
    b.type = buf_type_;
    b.memory = V4L2_MEMORY_MMAP;
    b.index = idx;

    if (IoctlRetry(fd_, VIDIOC_QBUF, &b) != 0) {
      if (error) *error = "VIDIOC_QBUF failed: " + std::string(std::strerror(errno));
      return false;
    }
    return true;
  }

#ifdef V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
  v4l2_plane planes[1]{};
  v4l2_buffer b{};
  b.type = buf_type_;
  b.memory = V4L2_MEMORY_MMAP;
  b.index = idx;
  b.m.planes = planes;
  b.length = 1;

  if (IoctlRetry(fd_, VIDIOC_QBUF, &b) != 0) {
    if (error) *error = "VIDIOC_QBUF (mplane) failed: " + std::string(std::strerror(errno));
    return false;
  }
  return true;
#else
  if (error) *error = "mplane capture not supported by headers";
  return false;
#endif
}

void V4l2Capture::Close() {
  if (fd_ < 0) return;

  std::string ignored;
  (void)StreamOff(&ignored);

  for (auto& b : buffers_) {
    if (b.start && b.start != MAP_FAILED && b.length > 0) {
      (void)::munmap(b.start, b.length);
    }
    b.start = nullptr;
    b.length = 0;
  }
  buffers_.clear();

  ::close(fd_);
  fd_ = -1;

  streaming_ = false;
  buf_type_ = 0;
  mplane_ = false;
  actual_ = CaptureFormat{};
}

}  // namespace studiocast::video
