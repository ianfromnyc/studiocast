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

// True when a format read back from the device names a frame the device can
// be asked to take again. It is the question `ParseChosenOutputFmt` asks of a
// rung, asked of the format the walk saves.
//
// A blank report, of width or height 0, is refused: S_FMT of a blank does not
// fail on v4l2loopback, the driver rewrites the blank to its default geometry
// and sets the format every consumer sees, thus "putting it back" would
// change the device the walk was to leave alone. A format the writer cannot
// fill still passes, because the device held it before the writer asked for
// anything.
//
// Exposed for tests; the format restore of `V4l2Writer::Open()` is the only
// other caller.
bool SavedOutputFmtIsRestorable(const v4l2_format &f, bool mplane);

// True when a failed `V4l2Writer::Open()` names a condition that may be gone
// a moment later, thus one the caller can wait out. `CameraPipeline` retries
// the open for a short budget while this answers yes, and reports the failure
// at once when it answers no.
//
// The window is the one v4l2loopback opens while a producer closes or a
// consumer disconnects. It has two faces: the driver answers EINVAL, or it
// answers a blank frame report. A report the driver stands by, such as a
// plane count the writer cannot use or a device that takes no write(), says
// the same thing on every retry.
//
// A failure the format ladder composed is read by the refusal that stopped
// the walk alone, which `ComposeLadderFailure` gives a line of its own. The
// attempt log below that line holds the errno of every rung the walk left
// behind, and a rung the walk went past says nothing about the refusal that
// decided it: on an mplane-only device every single-plane rung answers EINVAL
// and the walk then stops on a plane count no retry can change. Every other
// failure the open reports is one refusal, thus the whole text is read.
bool OutputOpenErrorIsTransient(const std::string &error);

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

  // True when the question changes the format the device holds: S_FMT does,
  // G_FMT does not. The walk reads the held format before the first rung that
  // changes it, and puts that format back only when such a rung answered.
  bool mutates = false;

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
  // index of that rung in `rungs`. A walk that kept a rung leaves both empty,
  // because the layout it gives back is the whole answer.
  //
  // This is the refusal that stopped the walk, thus the one that says whether
  // the failure can be waited out. `ComposeLadderFailure` writes both the
  // rung name and the message on one line of the failure, and
  // `OutputOpenErrorIsTransient` reads that line alone.
  std::string first_refusal;
  std::size_t first_refusal_rung = 0;
};

// How the walk puts the device format back. A rung that asks S_FMT changes
// the format the device holds, and the walk goes on past an S_FMT the driver
// accepted whose answer the parse refused, thus a walk that fails altogether
// can leave the device holding a format the writer named as one it cannot
// use. Both members are best effort, and a walk given neither changes nothing
// on failure.
//
// The save and the restore work on one buffer type at a time, because the
// S_FMT that puts a format back names the type the format was read under. A
// walk that saved the format of one type and put it back on another would
// write to a type its rungs never touched.
struct FormatRestore {
  // Reads the format the device holds for the buffer type of one rung. The
  // walk calls this before the first rung of that type whose `mutates` says
  // it changes the format, thus it asks nothing about a type no rung changes
  // and asks about each such type once. True when the answer is one the walk
  // can put back.
  std::function<bool(const FormatLadderRung &rung, v4l2_format *outFmt)> save;

  // Asks the device to take a saved format again. The walk calls this once
  // for each buffer type it saved and a rung of that type then changed, and
  // it does not read the outcome: the caller is on its way to reporting the
  // failure.
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
// walk that keeps no rung puts back the formats `restore.save` read, so that
// it leaves the device as it found it. It puts back the format of a buffer
// type only when a rung of that type changed it, and it puts the types back
// in the reverse of the order it read them, so that a driver which keeps one
// format for several types ends on the format the walk read first.
//
// That is a claim about the walk alone, not about the open. Everything after
// a kept rung is outside it: a caller that opens the device and then closes
// it, because the format the walk kept is not the format it asked for, leaves
// the device holding the format the walk set.
//
// Exposed for tests; `V4l2Writer::Open()` is the only other caller.
FormatLadderResult
ChooseOutputFormat(const std::vector<FormatLadderRung> &rungs, int fps,
                   const FormatRestore &restore = {});

// Composes the failure a walk that kept no rung reports: the header the
// caller gives, the refusal that stopped the walk, and one line for every
// rung the walk left behind.
//
// Exposed for tests; `V4l2Writer::Open()` is the only other caller.
std::string ComposeLadderFailure(const std::string &header,
                                 const std::vector<FormatLadderRung> &rungs,
                                 const FormatLadderResult &ladder);

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
