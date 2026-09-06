#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// V4L2 format the driver reports back from VIDIOC_S_FMT. Only a reference to
// one crosses this header, so it stays free of <linux/videodev2.h>.
struct v4l2_format;

namespace studiocast::video {

enum class CapturePixelFormat {
  yuyv,
  mjpeg,
  rgb24,
};

// Heuristic used by `V4l2Capture::Open()` to decide whether to prefer MJPEG at
// a given requested resolution (many webcams cap uncompressed YUYV at ~720p,
// but can do 1080p+ in MJPEG).
bool ShouldPreferMjpegForResolution(int width, int height);

struct CaptureFormatSupport {
  bool yuyv = false;

  // True when either MJPEG or JPEG compressed capture is supported.
  bool mjpeg = false;
};

// Pure negotiation helper used by `V4l2Capture::Open()` and tests. It returns
// the pixel-format attempt order after applying MJPEG preference and removing
// unsupported YUYV/MJPEG attempts.
std::vector<CapturePixelFormat>
CaptureFormatTryOrderForRequest(CapturePixelFormat requested,
                                bool prefer_mjpeg, int width, int height,
                                CaptureFormatSupport support);

struct CaptureFormat {
  int width = 0;
  int height = 0;
  int fps = 0;
  int fps_num = 0;
  int fps_den = 0;
  CapturePixelFormat format = CapturePixelFormat::yuyv;

  // V4L2 negotiated pixel format, as FourCC.
  // Example: V4L2_PIX_FMT_YUYV -> "YUYV".
  std::uint32_t pixfmt_fourcc = 0;
  std::string pixfmt;

  std::size_t bytes_per_line = 0;
  std::size_t size_image = 0;
};

// Reads the format the driver reported after VIDIOC_S_FMT into a
// `CaptureFormat`. `mplane` says which union arm of `f` holds the answer.
//
// The driver has already laid the frame out, so `bytes_per_line` keeps the
// stride the driver reported: a larger row size does not make the row longer,
// it only makes the reader walk the frame at a stride the data does not use.
// A row too short to hold the pixels, or a stride of 0, is raised to the
// packed row size, and `size_image` is raised to the row size times the
// height. Compressed MJPEG has no row size, so both values stay as reported.
//
// A report whose own rows do not fit its own frame size is refused, because
// the read path would then walk past the end of the mapped buffer. A frame
// size of 0 is no report at all rather than that disagreement, so it takes
// the raise instead of the refusal.
//
// Exposed for tests; `V4l2Capture::Open()` is the only other caller.
bool ParseChosenCaptureFmt(const v4l2_format &f, bool mplane, int fps,
                           int fps_num, int fps_den, CaptureFormat *out,
                           std::string *outErr);

// True when a buffer VIDIOC_QUERYBUF reported is long enough to hold the frame
// `fmt` describes. The read path walks `fmt.size_image` bytes of the mapping,
// so a shorter buffer is a read past the end of it. This is the length the
// walk must stay inside, and negotiation alone cannot know it.
//
// Exposed for tests; `V4l2Capture::Open()` is the only other caller.
bool CaptureBufferHoldsFrame(std::size_t mapped_length,
                             const CaptureFormat &fmt, std::string *outErr);

// Resolves where the image sits in a buffer VIDIOC_DQBUF just returned.
// `bytesused` is the driver's own count of the bytes it wrote, and it is the
// length the compressed read path walks, so the mapping has to bound it like
// every other walk: a count above `mapped_length` is clamped to it.
//
// On a multi-planar buffer `bytesused` also includes `data_offset`, so the
// image starts that many bytes into the plane and is that much shorter. The
// single-plane buffer has no such field and passes 0. An offset the payload
// does not reach describes no image at all, so the frame is refused.
//
// Exposed for tests; `CaptureAcceptDequeuedBuffer()` is the only other
// caller.
bool CaptureFramePayload(std::size_t mapped_length, std::size_t bytesused,
                         std::size_t data_offset, std::size_t *out_offset,
                         std::size_t *out_bytes, std::string *outErr);

// True when the walk the raw read path makes stays inside the mapping after a
// plane `data_offset` moves the start of the image.
//
// A raw reader does not walk the payload length: it walks
// `bytes_per_line * height` from the start of the image, because that is the
// layout negotiation gave it. An offset therefore moves the walk later in the
// mapping but does not make it shorter, and the last `data_offset` bytes of
// it fall past the end of the mapping. A compressed frame walks the payload
// length instead, which `CaptureFramePayload` already bounds, so it is always
// inside.
//
// Either the offset or the mapping can refuse a frame here, and `outErr` names
// the one the operator acts on. A mapping that would have held the walk on its
// own gives the offset message; a mapping shorter than the walk gives the
// mapping message, because no offset makes that walk fit.
//
// Exposed for tests; `CaptureAcceptDequeuedBuffer()` is the only other
// caller.
bool CaptureRawWalkFitsMapping(std::size_t mapped_length,
                               std::size_t data_offset,
                               const CaptureFormat &fmt, std::string *outErr);

struct CapturedFrameView {
  const std::uint8_t *data = nullptr;
  std::size_t bytes = 0;
  int index = -1;
  std::uint64_t sequence = 0;

  // Timestamp from the V4L2 driver (v4l2_buffer.timestamp).
  // Units: nanoseconds.
  // Epoch: depends on the driver; check `timestamp_monotonic`.
  std::uint64_t timestamp_ns = 0;
  bool timestamp_monotonic = false;
};

// A buffer VIDIOC_DQBUF just returned, in the fields both buffer types have.
// The multi-planar arm reads `bytesused` and `data_offset` from its one plane,
// and the single-plane arm has no `data_offset` and passes 0.
struct CaptureDequeuedBuffer {
  const void *mapped_start = nullptr;
  std::size_t mapped_length = 0;
  std::size_t bytesused = 0;
  std::size_t data_offset = 0;

  int index = -1;
  std::uint64_t sequence = 0;
  std::uint64_t timestamp_ns = 0;
  bool timestamp_monotonic = false;
};

// Fills `out` from a dequeued buffer and says whether the frame can be used.
//
// The buffer index goes into `out` before the two refusals this makes, so a
// refused frame carries the buffer the driver dequeued. That is a contract,
// not an accident: a refusal here leaves the buffer out of the driver queue,
// and `ReleaseFrame()` finds it by `CapturedFrameView::index` alone. An index
// written below the refusals leaves the view at -1, and the driver loses one
// buffer on every refused frame.
//
// Exposed for tests; `V4l2Capture::AcquireFrame()` is the only other caller.
bool CaptureAcceptDequeuedBuffer(const CaptureDequeuedBuffer &buf,
                                 const CaptureFormat &fmt,
                                 CapturedFrameView *out, std::string *outErr);

class V4l2Capture final {
public:
  V4l2Capture() = default;
  ~V4l2Capture();

  V4l2Capture(const V4l2Capture &) = delete;
  V4l2Capture &operator=(const V4l2Capture &) = delete;

  bool Open(const std::string &device, int width, int height, int fps,
            CapturePixelFormat fmt, bool prefer_mjpeg, std::string *error);

  // Enumerate supported capture modes and open the best-scoring one.
  // Intended for `CaptureMode::auto_best`.
  bool OpenBest(const std::string &device, int target_fps, bool prefer_mjpeg,
                std::string *error);

  void Close();

  bool IsOpen() const { return fd_ >= 0; }
  const CaptureFormat &Actual() const { return actual_; }

  // Acquire a frame (DQBUF). Caller MUST call ReleaseFrame() with the returned
  // view, on failure as well as on success.
  //
  // The call clears `*out` first, so the view carries only what this call put
  // in it. Every claim below is therefore the function's own, and holds for a
  // caller that keeps one view across calls as well.
  //
  // A frame refused after VIDIOC_DQBUF is a buffer already out of the driver
  // queue, and the view carries its `index`, so ReleaseFrame() gives that
  // buffer back. The one exception is a buffer index the driver reports out of
  // range: no buffer of this capture answers to it, so the view keeps `index`
  // at -1 and STREAMOFF is what reclaims that one buffer. A failure before
  // VIDIOC_DQBUF - a poll error, a timeout - dequeues nothing and leaves
  // `index` at -1 as well. ReleaseFrame() returns early on either, so the call
  // is correct after any failure.
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
