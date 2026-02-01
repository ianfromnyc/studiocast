#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace studiocast::video {

enum class CapturePixelFormat {
  yuyv,
};

struct CaptureFormat {
  int width = 0;
  int height = 0;
  int fps = 0;
  CapturePixelFormat format = CapturePixelFormat::yuyv;

  std::size_t bytes_per_line = 0;
  std::size_t size_image = 0;
};

struct CapturedFrameView {
  const std::uint8_t *data = nullptr;
  std::size_t bytes = 0;
  int index = -1;
  std::uint64_t sequence = 0;
};

class V4l2Capture final {
public:
  V4l2Capture() = default;
  ~V4l2Capture();

  V4l2Capture(const V4l2Capture &) = delete;
  V4l2Capture &operator=(const V4l2Capture &) = delete;

  bool Open(const std::string &device, int width, int height, int fps,
            CapturePixelFormat fmt, std::string *error);

  void Close();

  bool IsOpen() const { return fd_ >= 0; }
  const CaptureFormat &Actual() const { return actual_; }

  // Acquire a frame (DQBUF). Caller MUST call ReleaseFrame() with the returned
  // view.
  bool AcquireFrame(CapturedFrameView *out, int timeout_ms, std::string *error);

  // Release a frame back to driver (QBUF).
  bool ReleaseFrame(const CapturedFrameView &f, std::string *error);

private:
  struct Buffer {
    void *start = nullptr;
    std::size_t length = 0;
  };

  bool StreamOn(std::string *error);
  bool StreamOff(std::string *error);

  // chosen buffer type (single-plane capture or mplane capture)
  unsigned int buf_type_ = 0;
  bool mplane_ = false;

  int fd_ = -1;
  bool streaming_ = false;

  CaptureFormat actual_{};
  std::vector<Buffer> buffers_;
};

} // namespace studiocast::video
