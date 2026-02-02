#include "v4l2_capture.h"

#include <cerrno>
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

std::uint32_t FourccFor(CapturePixelFormat fmt) {
  switch (fmt) {
    case CapturePixelFormat::yuyv: return V4L2_PIX_FMT_YUYV;
    case CapturePixelFormat::rgb24: return V4L2_PIX_FMT_RGB24;
  }
  return V4L2_PIX_FMT_YUYV;
}

std::string CapturePixelFormatLabel(CapturePixelFormat fmt) {
  switch (fmt) {
    case CapturePixelFormat::yuyv: return "YUYV";
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
  if (fourcc == V4L2_PIX_FMT_RGB24) {
    *out = CapturePixelFormat::rgb24;
    return true;
  }
  return false;
}

struct TypeSpec {
  unsigned int type = 0;
  bool mplane = false;
};

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

  if (IoctlRetry(fd, VIDIOC_S_FMT, &f) == 0) {
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

bool ParseChosenCaptureFmt(const v4l2_format& f, bool mplane, int fps, CaptureFormat* out, std::string* outErr) {
  if (!out) return false;

  int w = 0, h = 0;
  std::size_t bpl = 0;
  std::size_t size = 0;

  CapturePixelFormat parsedFmt{};

  if (!mplane) {
    w = static_cast<int>(f.fmt.pix.width);
    h = static_cast<int>(f.fmt.pix.height);
    bpl = static_cast<std::size_t>(f.fmt.pix.bytesperline);
    size = static_cast<std::size_t>(f.fmt.pix.sizeimage);

    if (!ParseCapturePixelFormat(f.fmt.pix.pixelformat, &parsedFmt)) {
      if (outErr) *outErr = "Unsupported capture pixel format";
      return false;
    }
  } else {
#ifdef V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
    w = static_cast<int>(f.fmt.pix_mp.width);
    h = static_cast<int>(f.fmt.pix_mp.height);
    if (f.fmt.pix_mp.num_planes < 1) {
      if (outErr) *outErr = "mplane format returned num_planes=0";
      return false;
    }
    bpl = static_cast<std::size_t>(f.fmt.pix_mp.plane_fmt[0].bytesperline);
    size = static_cast<std::size_t>(f.fmt.pix_mp.plane_fmt[0].sizeimage);

    if (!ParseCapturePixelFormat(f.fmt.pix_mp.pixelformat, &parsedFmt)) {
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
  a.format = parsedFmt;

  // Provide conservative minima.
  const std::size_t bpp = (a.format == CapturePixelFormat::rgb24) ? 3u : 2u;
  const std::size_t minBpl = static_cast<std::size_t>(a.width) * bpp;
  if (bpl < minBpl) bpl = minBpl;
  a.bytes_per_line = bpl;

  const std::size_t minSize = bpl * static_cast<std::size_t>(a.height);
  if (size < minSize) size = minSize;
  a.size_image = size;

  *out = a;
  return true;
}

}  // namespace

V4l2Capture::~V4l2Capture() { Close(); }

bool V4l2Capture::Open(const std::string& device,
                       int width,
                       int height,
                       int fps,
                       CapturePixelFormat fmt,
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
    std::string err;
    v4l2_format f{};
    if (TrySetFmtCapture(fd_, t, width, height, fmt, &f, &err)) {
      chosen = f;
      chosenMplane = t.mplane;
      buf_type_ = t.type;
      mplane_ = t.mplane;
      ok = true;
      break;
    }
    attempts << "Try S_FMT (" << (t.mplane ? "CAPTURE_MPLANE" : "CAPTURE")
             << "): " << (err.empty() ? "(no detail)" : err) << "\n";
  }

  if (!ok) {
    if (error) {
      std::ostringstream oss;
      oss << "Failed to set capture format to " << CapturePixelFormatLabel(fmt)
          << " on " << device << ".\n"
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

  std::string perr;
  if (!ParseChosenCaptureFmt(chosen, chosenMplane, fps, &actual_, &perr)) {
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

  const int pr = ::poll(&pfd, 1, timeout_ms);
  if (pr < 0) {
    if (errno == EINTR) return false;
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
