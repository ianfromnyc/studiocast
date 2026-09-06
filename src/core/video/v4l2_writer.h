#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// V4L2 format the driver reports back from VIDIOC_S_FMT. Only a reference to
// one crosses this header, so it stays free of <linux/videodev2.h>.
struct v4l2_format;

namespace studiocast::video {

enum class PixelFormat {
  yuyv,
  rgb24,
};

std::string PixelFormatName(PixelFormat fmt);
std::optional<PixelFormat> ParsePixelFormat(const std::string &s);

// Smallest bytes_per_line a frame of this width needs, which is also the row
// size the writer asks the driver for and the size the pipeline gives its
// output buffer. YUYV packs pixels in pairs, so an odd width still fills the
// whole final pair: the row is ceil(width / 2) * 4 bytes, not width * 2. That
// matches what every RGB24 to YUYV converter writes.
std::size_t MinBytesPerLine(int width, PixelFormat fmt);

struct ActualFormat {
  int width = 0;
  int height = 0;
  int fps = 0;
  int fps_num = 0;
  int fps_den = 0;
  PixelFormat format = PixelFormat::yuyv;

  // V4L2 negotiated pixel format, as FourCC.
  // Example: V4L2_PIX_FMT_YUYV -> "YUYV".
  std::uint32_t pixfmt_fourcc = 0;
  std::string pixfmt;

  std::size_t bytes_per_line = 0;
  std::size_t size_image = 0;
};

// Reads the format the driver reported after VIDIOC_S_FMT or VIDIOC_G_FMT
// into an `ActualFormat`. `mplane` says which union arm of `f` holds the
// answer: `fmt.pix` for a single-planar type, `fmt.pix_mp` for a
// multi-planar one.
//
// The writer gives the driver a frame with write(), and that I/O method
// takes a buffer of one plane only, so a multi-planar report of any other
// plane count is refused.
//
// A blank frame, of width or height 0, is refused: it takes no bytes at all,
// and it is what a v4l2loopback device reports while a consumer disconnects.
//
// A row too short to hold the pixels is raised to the packed row size, then
// the rows the writer walks are measured against the frame size the driver
// reported. A report whose rows do not fit that frame is refused, because the
// writer would then push more bytes than the frame the driver sized. A frame
// size of 0 is no report at all, thus it follows the rows instead.
//
// The frame the parse gives back is bounded, because it sizes buffers as well
// as writes. A report of a frame larger than 256 MiB, which is more than
// twice an 8K RGB24 frame, is refused.
//
// Exposed for tests; the writer itself is the only other caller.
bool ParseChosenOutputFmt(const v4l2_format &f, bool mplane, int fps,
                          ActualFormat *out, std::string *outErr);

// True when the capabilities a device reports from VIDIOC_QUERYCAP hold the
// read/write I/O method. The writer gives the driver every frame with
// write(), which is the file I/O the kernel offers under
// `V4L2_CAP_READWRITE`, thus a device without that cap takes no frame at all.
//
// Exposed for tests; `V4l2Writer::Open()` is the only other caller.
bool OutputDeviceCanWrite(std::uint32_t caps, std::string *outErr);

// One rung of the format ladder: a question for the driver, and the union arm
// the answer lands in.
struct FormatLadderRung {
  // Names the rung in the attempt log, e.g. "VIDEO_OUTPUT S_FMT(no stride)".
  std::string name;

  // The V4L2 buffer type the question names, and thus the type the caller
  // must use for every ioctl that follows the walk.
  std::uint32_t buf_type = 0;

  // True when the answer lands in `fmt.pix_mp`.
  bool mplane = false;

  // Asks the driver. Fills `*outFmt` on success, `*outErr` on failure.
  std::function<bool(v4l2_format *outFmt, std::string *outErr)> ask;
};

// What the walk of the ladder found.
struct FormatLadderResult {
  bool ok = false;
  ActualFormat actual{};

  // Index in `rungs` of the rung that gave the layout. Only valid when `ok`.
  std::size_t rung = 0;

  // One line for each rung the walk tried and left, for the failure message.
  std::string attempt_log;

  // The message of the first rung whose answer the parse refused, and the
  // index of that rung in `rungs`. Both belong to the failure message: a walk
  // that kept a rung leaves the message empty, because the layout it gives
  // back is the whole answer.
  std::string first_refusal;
  std::size_t first_refusal_rung = 0;
};

// How the walk puts the device format back. A rung that asks S_FMT changes
// the format the device holds, and the walk goes on past an S_FMT the driver
// accepted whose answer the parse refused, thus a walk that fails altogether
// can leave the device holding a format the writer named as one it cannot
// use. Both members are best effort, and a walk given neither changes nothing
// on failure.
struct FormatRestore {
  // Reads the format the device holds before the first rung. True when the
  // answer is one the walk can put back.
  std::function<bool(v4l2_format *outFmt)> save;

  // Asks the device to take the saved format again. The walk calls this only
  // when it failed after a rung the driver answered, and it does not read the
  // outcome: the caller is on its way to reporting the failure.
  std::function<void(const v4l2_format &fmt)> restore;
};

// Walks the rungs in order and keeps the first one the driver answers *and*
// `ParseChosenOutputFmt` can use.
//
// A parse refusal is not the end of the walk. The rung below often asks a
// question the driver answers self-consistently: a driver that pads the row
// of a with-stride S_FMT and echoes the frame size back computes both numbers
// itself when the writer asks for no stride.
//
// A walk that keeps a rung leaves the device holding that rung's format. A
// walk that fails puts back the format `restore.save` read, so that a failed
// open leaves the device as it found it.
//
// Exposed for tests; `V4l2Writer::Open()` is the only other caller.
FormatLadderResult
ChooseOutputFormat(const std::vector<FormatLadderRung> &rungs, int fps,
                   const FormatRestore &restore = {});

class V4l2Writer final {
public:
  V4l2Writer() = default;
  ~V4l2Writer();

  V4l2Writer(const V4l2Writer &) = delete;
  V4l2Writer &operator=(const V4l2Writer &) = delete;

  bool Open(const std::string &device, int width, int height, int fps,
            PixelFormat fmt, std::string *error);

  void Close();

  // Refresh cached negotiated format from the kernel.
  //
  // This is useful for v4l2loopback: some consumers may renegotiate
  // global caps via VIDIOC_S_FMT. When that happens, the writer must
  // update its cached size_image/bytes_per_line to avoid write() failures.
  bool RefreshActual(std::string *error);

  // Re-negotiate output format/fps on the currently open writer FD.
  //
  // This avoids closing/re-opening the producer FD, which can destabilize
  // certain v4l2loopback configurations (especially with exclusive_caps=1).
  bool Renegotiate(const std::string &device, int width, int height, int fps,
                   PixelFormat fmt, std::string *error);

  bool WriteFrame(const std::uint8_t *data, std::size_t bytes,
                  std::string *error);

  bool IsOpen() const { return fd_ >= 0; }
  const ActualFormat &Actual() const { return actual_; }

private:
  int fd_ = -1;
  ActualFormat actual_{};
};

} // namespace studiocast::video
