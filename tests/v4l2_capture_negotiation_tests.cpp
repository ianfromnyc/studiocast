#include "core/video/v4l2_capture.h"
#include "core/video/capture_error_policy.h"

#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

#include <linux/videodev2.h>

namespace studiocast::tests {
namespace {

using studiocast::video::CaptureFormatSupport;
using studiocast::video::CaptureFormatTryOrderForRequest;
using studiocast::video::CapturePixelFormat;
using studiocast::video::ShouldFallbackToRawAfterMjpegDecodeFailure;
using studiocast::video::ShouldPreferMjpegForResolution;

bool Expect(bool condition, const char *message) {
  if (!condition)
    std::cerr << message << "\n";
  return condition;
}

bool SameOrder(const std::vector<CapturePixelFormat> &actual,
               std::initializer_list<CapturePixelFormat> expected) {
  return actual == std::vector<CapturePixelFormat>(expected);
}

std::optional<CapturePixelFormat>
FakeNegotiate(const std::vector<CapturePixelFormat> &try_order,
              CapturePixelFormat succeeds_on,
              std::vector<CapturePixelFormat> *attempts) {
  if (attempts)
    attempts->clear();
  for (const CapturePixelFormat fmt : try_order) {
    if (attempts)
      attempts->push_back(fmt);
    if (fmt == succeeds_on)
      return fmt;
  }
  return std::nullopt;
}

bool TestCapturePreferenceTreats720pAsMjpegWorthy() {
  return Expect(ShouldPreferMjpegForResolution(1280, 720),
                "1280x720 should prefer MJPEG when MJPEG preference is "
                "enabled") &&
         Expect(ShouldPreferMjpegForResolution(1920, 1080),
                "1080p should prefer MJPEG when MJPEG preference is enabled") &&
         Expect(!ShouldPreferMjpegForResolution(640, 480),
                "640x480 should keep uncompressed YUYV first") &&
         Expect(!ShouldPreferMjpegForResolution(0, 720),
                "invalid dimensions should not prefer MJPEG");
}

bool TestYuyvRequestTriesMjpegFirstAtHdWhenPreferred() {
  CaptureFormatSupport support;
  support.yuyv = true;
  support.mjpeg = true;

  const auto order = CaptureFormatTryOrderForRequest(
      CapturePixelFormat::yuyv, /*prefer_mjpeg=*/true, 1280, 720, support);

  return Expect(
      SameOrder(order, {CapturePixelFormat::mjpeg, CapturePixelFormat::yuyv}),
      "YUYV request at 720p with MJPEG preference should try MJPEG then YUYV");
}

bool TestYuyvRequestFallsBackToMjpegAfterYuyvAtLowResolution() {
  CaptureFormatSupport support;
  support.yuyv = true;
  support.mjpeg = true;

  const auto order = CaptureFormatTryOrderForRequest(
      CapturePixelFormat::yuyv, /*prefer_mjpeg=*/true, 640, 480, support);

  return Expect(
      SameOrder(order, {CapturePixelFormat::yuyv, CapturePixelFormat::mjpeg}),
      "low-resolution YUYV request should try YUYV before MJPEG fallback");
}

bool TestYuyvRequestDoesNotAddMjpegFallbackWhenPreferenceDisabled() {
  CaptureFormatSupport support;
  support.yuyv = true;
  support.mjpeg = true;

  const auto order = CaptureFormatTryOrderForRequest(
      CapturePixelFormat::yuyv, /*prefer_mjpeg=*/false, 1920, 1080, support);

  return Expect(SameOrder(order, {CapturePixelFormat::yuyv}),
                "disabled MJPEG preference should keep a YUYV-only try order");
}

bool TestUnsupportedFormatsAreSkippedWithoutDuplicates() {
  CaptureFormatSupport mjpeg_only;
  mjpeg_only.mjpeg = true;

  const auto mjpeg_order = CaptureFormatTryOrderForRequest(
      CapturePixelFormat::yuyv, /*prefer_mjpeg=*/true, 1920, 1080,
      mjpeg_only);

  CaptureFormatSupport yuyv_only;
  yuyv_only.yuyv = true;

  const auto yuyv_order = CaptureFormatTryOrderForRequest(
      CapturePixelFormat::yuyv, /*prefer_mjpeg=*/true, 1920, 1080, yuyv_only);

  return Expect(SameOrder(mjpeg_order, {CapturePixelFormat::mjpeg}),
                "unsupported YUYV should be skipped when MJPEG is available") &&
         Expect(SameOrder(yuyv_order, {CapturePixelFormat::yuyv}),
                "unsupported MJPEG should be skipped without duplicating YUYV");
}

bool TestExplicitMjpegRequestDoesNotFallBackToYuyvInsideOpenOrder() {
  CaptureFormatSupport support;
  support.yuyv = true;
  support.mjpeg = true;

  const auto order = CaptureFormatTryOrderForRequest(
      CapturePixelFormat::mjpeg, /*prefer_mjpeg=*/true, 1280, 720, support);

  return Expect(SameOrder(order, {CapturePixelFormat::mjpeg}),
                "explicit MJPEG request should not add an implicit YUYV "
                "fallback in Open()");
}

bool TestFakeNegotiationUsesOrderedFallback() {
  CaptureFormatSupport support;
  support.yuyv = true;
  support.mjpeg = true;
  const auto order = CaptureFormatTryOrderForRequest(
      CapturePixelFormat::yuyv, /*prefer_mjpeg=*/true, 1280, 720, support);

  std::vector<CapturePixelFormat> attempts;
  const auto selected =
      FakeNegotiate(order, CapturePixelFormat::yuyv, &attempts);

  return Expect(selected.has_value() &&
                    *selected == CapturePixelFormat::yuyv,
                "fake negotiation should fall back to YUYV when MJPEG fails") &&
         Expect(SameOrder(attempts,
                          {CapturePixelFormat::mjpeg,
                           CapturePixelFormat::yuyv}),
                "fake negotiation should preserve MJPEG/YUYV attempt order");
}

bool TestMjpegDecodeFailureFallsBackToRawOnce() {
  studiocast::video::CaptureFormat mjpeg;
  mjpeg.format = CapturePixelFormat::mjpeg;

  studiocast::video::CaptureFormat yuyv;
  yuyv.format = CapturePixelFormat::yuyv;

  return Expect(
             ShouldFallbackToRawAfterMjpegDecodeFailure(
                 mjpeg, /*fallback_already_attempted=*/false),
             "first MJPEG decode failure should allow raw capture fallback") &&
         Expect(
             !ShouldFallbackToRawAfterMjpegDecodeFailure(
                 mjpeg, /*fallback_already_attempted=*/true),
             "MJPEG decode failure should not loop raw fallback attempts") &&
         Expect(
             !ShouldFallbackToRawAfterMjpegDecodeFailure(
                 yuyv, /*fallback_already_attempted=*/false),
             "raw capture failures should not re-enter MJPEG fallback policy");
}

struct RowLayoutCase {
  const char *name;
  std::uint32_t fourcc;
  int width;
  int height;

  // What the driver reported after VIDIOC_S_FMT.
  std::uint32_t driver_bytesperline;
  std::uint32_t driver_sizeimage;

  // What the capture format must carry into the read path.
  std::size_t bytes_per_line;
  std::size_t size_image;

  // Which arm of the `v4l2_format` union the driver filled. The mplane arm
  // reads the row size out of `plane_fmt[0]`, not out of `fmt.pix`.
  bool mplane = false;
};

// Writes a driver report into the union arm `mplane` names. `num_planes` is
// meaningful on the mplane arm only, where a report of no planes has no row
// size to read.
void FillDriverFormat(v4l2_format *f, const RowLayoutCase &c,
                      std::uint8_t num_planes = 1) {
  *f = v4l2_format{};

  if (!c.mplane) {
    f->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    f->fmt.pix.width = static_cast<__u32>(c.width);
    f->fmt.pix.height = static_cast<__u32>(c.height);
    f->fmt.pix.pixelformat = c.fourcc;
    f->fmt.pix.bytesperline = c.driver_bytesperline;
    f->fmt.pix.sizeimage = c.driver_sizeimage;
    return;
  }

#ifdef V4L2_CAP_VIDEO_CAPTURE_MPLANE
  f->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
  f->fmt.pix_mp.width = static_cast<__u32>(c.width);
  f->fmt.pix_mp.height = static_cast<__u32>(c.height);
  f->fmt.pix_mp.pixelformat = c.fourcc;
  f->fmt.pix_mp.num_planes = num_planes;
  if (num_planes >= 1) {
    f->fmt.pix_mp.plane_fmt[0].bytesperline = c.driver_bytesperline;
    f->fmt.pix_mp.plane_fmt[0].sizeimage = c.driver_sizeimage;
  }
#else
  (void)num_planes;
#endif
}

// The driver fills the capture buffer before the program reads it, so the
// stride the driver reports is the stride the frame really uses. Raising it
// does not make the row longer: it makes the reader walk the mmap'd frame at
// a stride the data does not use, which misreads every row after the first
// and runs past the end of the frame. v4l2loopback accepts an odd YUYV width
// and reports a packed `bytesperline`, so this case is reachable.
//
// A stride below the packed row size is the one that cannot be believed. The
// row must at least hold the pixels, so that value alone is raised.
bool TestCaptureNegotiationKeepsTheDriverRowStride() {
  using studiocast::video::CaptureFormat;
  using studiocast::video::ParseChosenCaptureFmt;

  const RowLayoutCase cases[] = {
      {"odd YUYV width, packed driver stride", V4L2_PIX_FMT_YUYV, 3, 4, 6, 24,
       6u, 24u},
      {"even YUYV width, packed driver stride", V4L2_PIX_FMT_YUYV, 640, 480,
       1280, 614400, 1280u, 614400u},
      {"YUYV padded row is kept", V4L2_PIX_FMT_YUYV, 640, 480, 1536, 737280,
       1536u, 737280u},
      {"YUYV stride below the packed row is raised", V4L2_PIX_FMT_YUYV, 3, 4, 4,
       16, 6u, 24u},
      {"YUYV stride of zero is raised", V4L2_PIX_FMT_YUYV, 640, 480, 0, 0,
       1280u, 614400u},
      {"a stride with no frame size at all", V4L2_PIX_FMT_YUYV, 640, 480, 1280,
       0, 1280u, 614400u},
      {"odd RGB24 width, packed driver stride", V4L2_PIX_FMT_RGB24, 3, 4, 9, 36,
       9u, 36u},
      {"MJPEG keeps the reported values", V4L2_PIX_FMT_MJPEG, 640, 480, 0,
       100000, 0u, 100000u},
#ifdef V4L2_CAP_VIDEO_CAPTURE_MPLANE
      {"odd YUYV width, packed driver stride, mplane", V4L2_PIX_FMT_YUYV, 3, 4,
       6, 24, 6u, 24u, true},
      {"YUYV padded row is kept, mplane", V4L2_PIX_FMT_YUYV, 640, 480, 1536,
       737280, 1536u, 737280u, true},
      {"YUYV stride below the packed row is raised, mplane", V4L2_PIX_FMT_YUYV,
       3, 4, 4, 16, 6u, 24u, true},
#endif
  };

  for (const RowLayoutCase &c : cases) {
    v4l2_format f{};
    FillDriverFormat(&f, c);

    CaptureFormat got{};
    if (!Expect(ParseChosenCaptureFmt(f, c.mplane, /*fps=*/30,
                                      /*fps_num=*/1, /*fps_den=*/30, &got,
                                      nullptr),
                "the reported capture format must parse")) {
      std::cerr << "  case " << c.name << "\n";
      return false;
    }

    if (!Expect(got.bytes_per_line == c.bytes_per_line,
                "the capture row stride must be the one the frame uses")) {
      std::cerr << "  case " << c.name << ": expected " << c.bytes_per_line
                << ", got " << got.bytes_per_line << "\n";
      return false;
    }

    if (!Expect(got.size_image == c.size_image,
                "the capture frame size must follow the row stride")) {
      std::cerr << "  case " << c.name << ": expected " << c.size_image
                << ", got " << got.size_image << "\n";
      return false;
    }
  }

  return true;
}

// A driver that reports more rows than its own frame size holds is out of the
// V4L2 contract. Raising `size_image` to match the stride hides that: the read
// path then walks a frame longer than the buffer the driver sized, which is a
// read past the end of the mapping on every frame. Refuse the report instead.
bool TestCaptureNegotiationRefusesRowsTheFrameSizeCannotHold() {
  using studiocast::video::CaptureFormat;
  using studiocast::video::ParseChosenCaptureFmt;

  // Only the driver report matters here, so the expected values stay 0.
  const RowLayoutCase cases[] = {
      {"padded YUYV rows overrun the reported frame", V4L2_PIX_FMT_YUYV, 640,
       480, 1536, 614400, 0u, 0u},
      {"odd YUYV rows overrun the reported frame", V4L2_PIX_FMT_YUYV, 3, 4, 6,
       16, 0u, 0u},
      {"RGB24 rows overrun the reported frame", V4L2_PIX_FMT_RGB24, 640, 480,
       1920, 614400, 0u, 0u},
#ifdef V4L2_CAP_VIDEO_CAPTURE_MPLANE
      {"padded YUYV rows overrun the reported frame, mplane", V4L2_PIX_FMT_YUYV,
       640, 480, 1536, 614400, 0u, 0u, true},
#endif
  };

  for (const RowLayoutCase &c : cases) {
    v4l2_format f{};
    FillDriverFormat(&f, c);

    CaptureFormat got{};
    std::string err;
    if (!Expect(!ParseChosenCaptureFmt(f, c.mplane, /*fps=*/30,
                                       /*fps_num=*/1, /*fps_den=*/30, &got,
                                       &err),
                "a frame size the rows overrun must fail negotiation")) {
      std::cerr << "  case " << c.name << ": got bytes_per_line "
                << got.bytes_per_line << ", size_image " << got.size_image
                << "\n";
      return false;
    }

    if (!Expect(!err.empty(), "the refusal must name the reason")) {
      std::cerr << "  case " << c.name << "\n";
      return false;
    }
  }

  return true;
}

// The read path holds one plane only, so the mplane arm reads `plane_fmt[0]`
// and nothing else. A report of no planes has nothing to read, and a report of
// more than one plane describes a frame plane 0 does not hold. Both must fail
// here, where the plane count can be named, and not later at VIDIOC_QUERYBUF
// where the kernel answers EINVAL alone.
bool TestCaptureNegotiationRefusesAnMplaneReportWithoutOnePlane() {
#ifdef V4L2_CAP_VIDEO_CAPTURE_MPLANE
  using studiocast::video::CaptureFormat;
  using studiocast::video::ParseChosenCaptureFmt;

  const RowLayoutCase c = {"mplane report with the wrong plane count",
                           V4L2_PIX_FMT_YUYV,
                           640,
                           480,
                           1280,
                           614400,
                           0u,
                           0u,
                           true};

  for (const int num_planes : {0, 2, 3}) {
    v4l2_format f{};
    FillDriverFormat(&f, c, static_cast<std::uint8_t>(num_planes));

    CaptureFormat got{};
    std::string err;
    if (!Expect(!ParseChosenCaptureFmt(f, /*mplane=*/true, /*fps=*/30,
                                       /*fps_num=*/1, /*fps_den=*/30, &got,
                                       &err),
                "an mplane report of other than one plane must fail "
                "negotiation")) {
      std::cerr << "  num_planes " << num_planes << "\n";
      return false;
    }

    if (!Expect(!err.empty(), "the refusal must name the reason")) {
      std::cerr << "  num_planes " << num_planes << "\n";
      return false;
    }
  }

  return true;
#else
  return true;
#endif
}

// `size_image` is the length the read path walks, and the mapped buffer is
// the only place that length has to fit. A driver that reports a row size
// below the packed row leaves the negotiated frame larger than the frame the
// driver sized, so the buffer it maps is the value that decides.
bool TestCaptureBufferMustHoldTheNegotiatedFrame() {
  using studiocast::video::CaptureBufferHoldsFrame;
  using studiocast::video::CaptureFormat;

  CaptureFormat fmt;
  fmt.width = 3;
  fmt.height = 4;
  fmt.format = CapturePixelFormat::yuyv;
  fmt.bytes_per_line = 6;
  fmt.size_image = 24;

  std::string err;
  if (!Expect(CaptureBufferHoldsFrame(24u, fmt, &err),
              "a buffer exactly the frame size must be accepted"))
    return false;

  if (!Expect(CaptureBufferHoldsFrame(4096u, fmt, &err),
              "a buffer longer than the frame must be accepted"))
    return false;

  // The driver reported bytesperline=4, sizeimage=16 for this 3x4 YUYV frame,
  // so negotiation raised the stride to the packed row and the frame to 24.
  // The buffer the driver mapped still holds only 16 bytes.
  err.clear();
  if (!Expect(!CaptureBufferHoldsFrame(16u, fmt, &err),
              "a buffer shorter than the frame must be refused"))
    return false;

  if (!Expect(!err.empty(), "the refusal must name the reason"))
    return false;

  return Expect(!CaptureBufferHoldsFrame(0u, fmt, nullptr),
                "an empty buffer must be refused") &&
         Expect(CaptureBufferHoldsFrame(0u, CaptureFormat{}, nullptr),
                "a frame of no bytes must not refuse an empty buffer");
}

// `bytesused` is the driver's own count of the bytes it put in the buffer,
// and it is the length the MJPEG walk uses, so it must be bounded by the
// mapping like every other walk. On a multi-planar buffer it also includes
// `data_offset`: the image starts that many bytes into the plane and is that
// much shorter.
bool TestCaptureFramePayloadStaysInsideTheMapping() {
  using studiocast::video::CaptureFramePayload;

  std::size_t off = 0;
  std::size_t bytes = 0;

  if (!Expect(CaptureFramePayload(4096u, 1024u, /*data_offset=*/0u, &off,
                                  &bytes, nullptr) &&
                  off == 0u && bytes == 1024u,
              "a payload inside the mapping must be taken as reported"))
    return false;

  // A driver that says it wrote more than the buffer holds is outside the
  // V4L2 contract. The mapping is the length the walk has to stay inside.
  if (!Expect(CaptureFramePayload(4096u, 9000u, /*data_offset=*/0u, &off,
                                  &bytes, nullptr) &&
                  off == 0u && bytes == 4096u,
              "a payload longer than the mapping must be clamped to it"))
    return false;

  // `data_offset` is included in `bytesused`, so the image is both later and
  // shorter than the plane count alone says.
  if (!Expect(CaptureFramePayload(4096u, 1024u, /*data_offset=*/64u, &off,
                                  &bytes, nullptr) &&
                  off == 64u && bytes == 960u,
              "a plane data_offset must move and shorten the image"))
    return false;

  if (!Expect(CaptureFramePayload(4096u, 1024u, /*data_offset=*/1024u, &off,
                                  &bytes, nullptr) &&
                  off == 1024u && bytes == 0u,
              "an offset at the end of the payload must give no image"))
    return false;

  std::string err;
  if (!Expect(!CaptureFramePayload(4096u, 1024u, /*data_offset=*/1025u, &off,
                                   &bytes, &err),
              "an offset past the payload must refuse the frame"))
    return false;

  if (!Expect(!err.empty(), "the refusal must name the reason"))
    return false;

  // The clamp runs first, so an offset inside an overstated payload but
  // outside the mapping is still refused.
  return Expect(!CaptureFramePayload(4096u, 9000u, /*data_offset=*/8192u, &off,
                                     &bytes, nullptr),
                "an offset outside the mapping must refuse the frame");
}

} // namespace

bool TestV4l2CaptureFramePayloadStaysInsideTheMapping() {
  return TestCaptureFramePayloadStaysInsideTheMapping();
}

bool TestV4l2CapturePreferenceTreats720pAsMjpegWorthy() {
  return TestCapturePreferenceTreats720pAsMjpegWorthy();
}

bool TestV4l2YuyvRequestTriesMjpegFirstAtHdWhenPreferred() {
  return TestYuyvRequestTriesMjpegFirstAtHdWhenPreferred();
}

bool TestV4l2YuyvRequestFallsBackToMjpegAfterYuyvAtLowResolution() {
  return TestYuyvRequestFallsBackToMjpegAfterYuyvAtLowResolution();
}

bool TestV4l2YuyvRequestDoesNotAddMjpegFallbackWhenPreferenceDisabled() {
  return TestYuyvRequestDoesNotAddMjpegFallbackWhenPreferenceDisabled();
}

bool TestV4l2UnsupportedFormatsAreSkippedWithoutDuplicates() {
  return TestUnsupportedFormatsAreSkippedWithoutDuplicates();
}

bool TestV4l2ExplicitMjpegRequestDoesNotFallBackToYuyvInsideOpenOrder() {
  return TestExplicitMjpegRequestDoesNotFallBackToYuyvInsideOpenOrder();
}

bool TestV4l2FakeNegotiationUsesOrderedFallback() {
  return TestFakeNegotiationUsesOrderedFallback();
}

bool TestV4l2MjpegDecodeFailureFallsBackToRawOnce() {
  return TestMjpegDecodeFailureFallsBackToRawOnce();
}

bool TestV4l2CaptureNegotiationKeepsTheDriverRowStride() {
  return TestCaptureNegotiationKeepsTheDriverRowStride();
}

bool TestV4l2CaptureNegotiationRefusesRowsTheFrameSizeCannotHold() {
  return TestCaptureNegotiationRefusesRowsTheFrameSizeCannotHold();
}

bool TestV4l2CaptureNegotiationRefusesAnMplaneReportWithoutOnePlane() {
  return TestCaptureNegotiationRefusesAnMplaneReportWithoutOnePlane();
}

bool TestV4l2CaptureBufferMustHoldTheNegotiatedFrame() {
  return TestCaptureBufferMustHoldTheNegotiatedFrame();
}

} // namespace studiocast::tests
