#include "v4l2_writer.h"

#include <cerrno>
#include <cstring>
#include <sstream>

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace studiocast::video {
namespace {

std::string ToLowerAscii(std::string s) {
  for (char& c : s) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

int IoctlRetry(int fd, unsigned long req, void* arg) {
  for (;;) {
    const int r = ::ioctl(fd, req, arg);
    if (r == 0) return 0;
    if (errno == EINTR) continue;
    return -1;
  }
}

std::uint32_t FourccFor(PixelFormat fmt) {
  switch (fmt) {
    case PixelFormat::yuyv:  return V4L2_PIX_FMT_YUYV;
    case PixelFormat::rgb24: return V4L2_PIX_FMT_RGB24;
  }
  return V4L2_PIX_FMT_YUYV;
}

std::size_t MinBytesPerLine(int width, PixelFormat fmt) {
  const auto w = static_cast<std::size_t>(width);
  switch (fmt) {
    case PixelFormat::yuyv:  return w * 2u;
    case PixelFormat::rgb24: return w * 3u;
  }
  return w * 2u;
}

}  // namespace

std::string PixelFormatName(PixelFormat fmt) {
  switch (fmt) {
    case PixelFormat::yuyv:  return "yuyv";
    case PixelFormat::rgb24: return "rgb24";
  }
  return "yuyv";
}

std::optional<PixelFormat> ParsePixelFormat(const std::string& s) {
  const auto t = ToLowerAscii(s);
  if (t == "yuyv" || t == "yuy2") return PixelFormat::yuyv;
  if (t == "rgb24" || t == "rgb") return PixelFormat::rgb24;
  return std::nullopt;
}

V4l2Writer::~V4l2Writer() { Close(); }

void V4l2Writer::Close() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  actual_ = ActualFormat{};
}

bool V4l2Writer::Open(const std::string& device,
                      int width,
                      int height,
                      int fps,
                      PixelFormat fmt,
                      std::string* error) {
  Close();

  if (device.empty()) {
    if (error) *error = "Device path is empty.";
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

  // v4l2loopback typically works with O_RDWR; O_WRONLY can be flaky on some setups.
  fd_ = ::open(device.c_str(), O_RDWR | O_CLOEXEC);
  if (fd_ < 0) {
    if (error) {
      *error = "Failed to open " + device + ": " + std::string(std::strerror(errno));
    }
    return false;
  }

  // Query capabilities (informational; don't be overly strict).
  v4l2_capability cap{};
  if (IoctlRetry(fd_, VIDIOC_QUERYCAP, &cap) != 0) {
    if (error) {
      *error = "VIDIOC_QUERYCAP failed: " + std::string(std::strerror(errno));
    }
    Close();
    return false;
  }

  // Set format.
  v4l2_format f{};
  f.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  f.fmt.pix.width = static_cast<__u32>(width);
  f.fmt.pix.height = static_cast<__u32>(height);
  f.fmt.pix.pixelformat = FourccFor(fmt);
  f.fmt.pix.field = V4L2_FIELD_NONE;

  // Provide sane defaults to help drivers.
  const std::size_t minBpl = MinBytesPerLine(width, fmt);
  f.fmt.pix.bytesperline = static_cast<__u32>(minBpl);
  f.fmt.pix.sizeimage = static_cast<__u32>(minBpl * static_cast<std::size_t>(height));

  if (IoctlRetry(fd_, VIDIOC_S_FMT, &f) != 0) {
    if (error) {
      std::ostringstream oss;
      oss << "VIDIOC_S_FMT failed for " << device
          << " (format=" << PixelFormatName(fmt) << ", " << width << "x" << height << "): "
          << std::strerror(errno);
      *error = oss.str();
    }
    Close();
    return false;
  }

  // Attempt to set fps (best-effort).
  v4l2_streamparm p{};
  p.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
  p.parm.output.timeperframe.numerator = 1;
  p.parm.output.timeperframe.denominator = static_cast<__u32>(fps);
  (void)IoctlRetry(fd_, VIDIOC_S_PARM, &p);

  // Capture actuals.
  actual_.width = static_cast<int>(f.fmt.pix.width);
  actual_.height = static_cast<int>(f.fmt.pix.height);
  actual_.fps = fps;
  actual_.format = fmt;

  actual_.bytes_per_line = static_cast<std::size_t>(f.fmt.pix.bytesperline);
  const std::size_t minActualBpl = MinBytesPerLine(actual_.width, fmt);
  if (actual_.bytes_per_line < minActualBpl) actual_.bytes_per_line = minActualBpl;

  actual_.size_image = static_cast<std::size_t>(f.fmt.pix.sizeimage);
  const std::size_t minSize = actual_.bytes_per_line * static_cast<std::size_t>(actual_.height);
  if (actual_.size_image < minSize) actual_.size_image = minSize;

  return true;
}

bool V4l2Writer::WriteFrame(const std::uint8_t* data, std::size_t bytes, std::string* error) {
  if (fd_ < 0) {
    if (error) *error = "Writer not open.";
    return false;
  }
  if (!data) {
    if (error) *error = "Frame data is null.";
    return false;
  }
  if (bytes < actual_.size_image) {
    if (error) *error = "Frame buffer too small for size_image.";
    return false;
  }

  const std::size_t toWrite = actual_.size_image;
  std::size_t offset = 0;

  while (offset < toWrite) {
    const std::size_t chunk = toWrite - offset;
    const ssize_t wrote = ::write(fd_, data + offset, chunk);
    if (wrote < 0) {
      if (errno == EINTR) continue;
      if (error) *error = std::string("write() failed: ") + std::strerror(errno);
      return false;
    }
    if (wrote == 0) {
      if (error) *error = "write() returned 0 (unexpected).";
      return false;
    }
    offset += static_cast<std::size_t>(wrote);
  }

  return true;
}

}  // namespace studiocast::video
