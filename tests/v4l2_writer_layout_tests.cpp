#include "core/video/v4l2_writer.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>
#include <vector>

#include <linux/videodev2.h>

namespace studiocast::tests {
namespace {

bool Expect(bool condition, const char *message) {
  if (!condition)
    std::cerr << message << "\n";
  return condition;
}

struct RowSizeCase {
  int width;
  std::size_t yuyv;
  std::size_t rgb24;
};

// One driver report, and the layout the writer must take from it.
struct LayoutCase {
  const char *name;
  std::uint32_t fourcc;
  int width;
  int height;
  __u32 driver_bytesperline;
  __u32 driver_sizeimage;
  std::size_t want_bytes_per_line;
  std::size_t want_size_image;
  bool mplane = false;
};

// Builds the `v4l2_format` the driver hands back from VIDIOC_S_FMT. The union
// arm follows `c.mplane`, which is what `ParseChosenOutputFmt` has to read.
void FillDriverFormat(v4l2_format *f, const LayoutCase &c,
                      std::uint8_t num_planes = 1) {
  *f = v4l2_format{};

  if (!c.mplane) {
    f->type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    f->fmt.pix.width = static_cast<__u32>(c.width);
    f->fmt.pix.height = static_cast<__u32>(c.height);
    f->fmt.pix.pixelformat = c.fourcc;
    f->fmt.pix.bytesperline = c.driver_bytesperline;
    f->fmt.pix.sizeimage = c.driver_sizeimage;
    return;
  }

#ifdef V4L2_CAP_VIDEO_OUTPUT_MPLANE
  f->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
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

} // namespace

// The row size the writer asks the driver for is the size the RGB24 to YUYV
// converters write, and the pipeline sizes its output buffer from it. A YUYV
// row packs pixels in pairs, so an odd width still fills the whole final pair:
// the row is ceil(width / 2) * 4 bytes. Two bytes less per row, and the last
// row of the frame runs past the buffer.
bool TestV4l2WriterRowSizeHoldsTheOddWidthYuyvPair() {
  using video::MinBytesPerLine;
  using video::PixelFormat;

  const RowSizeCase cases[] = {
      {1, 4u, 3u},    {2, 4u, 6u},    {3, 8u, 9u},         {7, 16u, 21u},
      {16, 32u, 48u}, {17, 36u, 51u}, {640, 1280u, 1920u},
  };

  for (const RowSizeCase &c : cases) {
    const std::size_t yuyv = MinBytesPerLine(c.width, PixelFormat::yuyv);
    if (!Expect(yuyv == c.yuyv,
                "YUYV row size must hold the whole final pixel pair")) {
      std::cerr << "  width " << c.width << ": expected " << c.yuyv << ", got "
                << yuyv << "\n";
      return false;
    }

    const std::size_t rgb24 = MinBytesPerLine(c.width, PixelFormat::rgb24);
    if (!Expect(rgb24 == c.rgb24,
                "RGB24 row size must be three bytes per pixel")) {
      std::cerr << "  width " << c.width << ": expected " << c.rgb24 << ", got "
                << rgb24 << "\n";
      return false;
    }
  }

  return true;
}

// A driver answers VIDIOC_S_FMT in one of two union arms: `fmt.pix` for a
// single-planar buffer type, `fmt.pix_mp` for a multi-planar one. The writer
// reads both, and the layout it takes must not depend on which arm carried
// it. The mplane cases are the arm a device that offers VIDEO_OUTPUT_MPLANE
// alone gives, which is the only arm such a device has.
//
// The writer keeps the stride the driver reports and raises only a row too
// short to hold the pixels, because the row must at least hold the pixels the
// converter writes into it. The frame size then follows the row, as long as
// the raised rows still fit the frame the driver sized.
bool TestV4l2WriterFormatParseReadsBothUnionArms() {
  using video::ActualFormat;
  using video::ParseChosenOutputFmt;
  using video::PixelFormat;

  const LayoutCase cases[] = {
      {"even YUYV width, packed driver stride", V4L2_PIX_FMT_YUYV, 640, 480,
       1280, 614400, 1280u, 614400u},
      {"YUYV padded row is kept", V4L2_PIX_FMT_YUYV, 640, 480, 1536, 737280,
       1536u, 737280u},
      {"odd YUYV width, packed driver stride", V4L2_PIX_FMT_YUYV, 3, 4, 8, 32,
       8u, 32u},
      {"YUYV stride below the packed row is raised", V4L2_PIX_FMT_YUYV, 3, 4, 4,
       32, 8u, 32u},
      {"YUYV stride of zero is raised", V4L2_PIX_FMT_YUYV, 640, 480, 0, 0,
       1280u, 614400u},
      {"odd RGB24 width, packed driver stride", V4L2_PIX_FMT_RGB24, 3, 4, 9, 36,
       9u, 36u},
      {"a frame size larger than the rows is kept", V4L2_PIX_FMT_YUYV, 640, 480,
       1280, 616448, 1280u, 616448u},
#ifdef V4L2_CAP_VIDEO_OUTPUT_MPLANE
      {"even YUYV width, packed driver stride, mplane", V4L2_PIX_FMT_YUYV, 640,
       480, 1280, 614400, 1280u, 614400u, true},
      {"YUYV padded row is kept, mplane", V4L2_PIX_FMT_YUYV, 640, 480, 1536,
       737280, 1536u, 737280u, true},
      {"YUYV stride below the packed row is raised, mplane", V4L2_PIX_FMT_YUYV,
       3, 4, 4, 32, 8u, 32u, true},
      {"odd RGB24 width, packed driver stride, mplane", V4L2_PIX_FMT_RGB24, 3,
       4, 9, 36, 9u, 36u, true},
#endif
  };

  for (const LayoutCase &c : cases) {
    v4l2_format f{};
    FillDriverFormat(&f, c);

    ActualFormat got{};
    std::string err;
    if (!Expect(ParseChosenOutputFmt(f, c.mplane, /*fps=*/30, &got, &err),
                "a supported driver report must parse")) {
      std::cerr << "  " << c.name << ": " << err << "\n";
      return false;
    }

    if (!Expect(got.width == c.width && got.height == c.height,
                "the parse must keep the reported frame size")) {
      std::cerr << "  " << c.name << ": got " << got.width << "x" << got.height
                << "\n";
      return false;
    }

    if (!Expect(got.pixfmt_fourcc == c.fourcc,
                "the parse must keep the reported pixel format")) {
      std::cerr << "  " << c.name << ": got " << got.pixfmt << "\n";
      return false;
    }

    if (!Expect(got.bytes_per_line == c.want_bytes_per_line,
                "the parse must keep the driver row stride")) {
      std::cerr << "  " << c.name << ": expected " << c.want_bytes_per_line
                << ", got " << got.bytes_per_line << "\n";
      return false;
    }

    if (!Expect(got.size_image == c.want_size_image,
                "the frame size must follow the row stride")) {
      std::cerr << "  " << c.name << ": expected " << c.want_size_image
                << ", got " << got.size_image << "\n";
      return false;
    }

    const PixelFormat want_fmt = (c.fourcc == V4L2_PIX_FMT_RGB24)
                                     ? PixelFormat::rgb24
                                     : PixelFormat::yuyv;
    if (!Expect(got.format == want_fmt,
                "the parse must name the pixel format it read")) {
      std::cerr << "  " << c.name << "\n";
      return false;
    }
  }

  // The writer sends YUYV or RGB24 only, so a compressed format is one it
  // cannot fill. Both arms must refuse it.
  const LayoutCase unsupported[] = {
      {"MJPEG is refused", V4L2_PIX_FMT_MJPEG, 640, 480, 0, 100000, 0u, 0u},
#ifdef V4L2_CAP_VIDEO_OUTPUT_MPLANE
      {"MJPEG is refused, mplane", V4L2_PIX_FMT_MJPEG, 640, 480, 0, 100000, 0u,
       0u, true},
#endif
  };

  for (const LayoutCase &c : unsupported) {
    v4l2_format f{};
    FillDriverFormat(&f, c);

    ActualFormat got{};
    std::string err;
    if (!Expect(!ParseChosenOutputFmt(f, c.mplane, /*fps=*/30, &got, &err),
                "a pixel format the writer cannot fill must be refused")) {
      std::cerr << "  " << c.name << "\n";
      return false;
    }

    if (!Expect(!err.empty(), "the refusal must name the reason")) {
      std::cerr << "  " << c.name << "\n";
      return false;
    }
  }

  return true;
}

// The writer gives the driver a frame with write(), and the kernel refuses
// that I/O method on a buffer of more than one plane: `__vb2_init_fileio` in
// videobuf2-core.c answers -EBUSY when `vb->num_planes != 1`. A report of no
// planes has nothing to read at all, because the arm reads `plane_fmt[0]`
// alone.
//
// So one plane is the only count the writer can use. Name the count at
// negotiation, where the layout is still in hand, rather than let the open
// succeed and every write() fail with "Device or resource busy".
bool TestV4l2WriterRefusesAnMplanePlaneCountItCannotWrite() {
#ifdef V4L2_CAP_VIDEO_OUTPUT_MPLANE
  using video::ActualFormat;
  using video::ParseChosenOutputFmt;

  const LayoutCase c = {"mplane report with the wrong plane count",
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

    ActualFormat got{};
    std::string err;
    if (!Expect(
            !ParseChosenOutputFmt(f, /*mplane=*/true, /*fps=*/30, &got, &err),
            "an mplane report of other than one plane must be refused")) {
      std::cerr << "  num_planes " << num_planes << "\n";
      return false;
    }

    if (!Expect(!err.empty(), "the refusal must name the reason")) {
      std::cerr << "  num_planes " << num_planes << "\n";
      return false;
    }
  }

  // One plane is the count the writer asks for and the only one it can use.
  v4l2_format ok{};
  FillDriverFormat(&ok, c, /*num_planes=*/1);

  ActualFormat got{};
  std::string err;
  return Expect(
      ParseChosenOutputFmt(ok, /*mplane=*/true, /*fps=*/30, &got, &err),
      "an mplane report of one plane must still be accepted");
#else
  return true;
#endif
}

// The rows the writer walks have to fit the frame size the driver says the
// buffer holds. The writer sizes every write() from `size_image`, so raising
// that value to match a longer row only hides the disagreement and then
// pushes more bytes than the frame the driver sized. On a v4l2loopback output
// the surplus runs into the next frame, and the picture is torn from then on.
//
// The measure comes after the row is raised, because the raised row is the
// row the writer walks. A stride below the packed row is a contradiction in
// the report of a packed format, thus the frame that stride implies gets no
// more trust than a stride the driver padded: the writer has no buffer to
// measure, because write() gives it none.
//
// A frame size of 0 is not that disagreement: it is no report at all, and the
// raise gives it the value the row implies. That case must stay accepted.
bool TestV4l2WriterRefusesRowsTheFrameSizeCannotHold() {
  using video::ActualFormat;
  using video::ParseChosenOutputFmt;

  const LayoutCase refused[] = {
      {"padded YUYV rows overrun the reported frame", V4L2_PIX_FMT_YUYV, 640,
       480, 1536, 614400, 0u, 0u},
      {"RGB24 rows overrun the reported frame", V4L2_PIX_FMT_RGB24, 640, 480,
       2048, 921600, 0u, 0u},
      {"one byte of stride too many", V4L2_PIX_FMT_YUYV, 640, 480, 1281, 614400,
       0u, 0u},
      {"a stride raised past the packed row overruns the frame",
       V4L2_PIX_FMT_YUYV, 3, 4, 4, 16, 0u, 0u},
      {"a stride of zero raised past the packed row overruns the frame",
       V4L2_PIX_FMT_RGB24, 640, 480, 0, 614400, 0u, 0u},
#ifdef V4L2_CAP_VIDEO_OUTPUT_MPLANE
      {"padded YUYV rows overrun the reported frame, mplane", V4L2_PIX_FMT_YUYV,
       640, 480, 1536, 614400, 0u, 0u, true},
      {"RGB24 rows overrun the reported frame, mplane", V4L2_PIX_FMT_RGB24, 640,
       480, 2048, 921600, 0u, 0u, true},
      {"a stride raised past the packed row overruns the frame, mplane",
       V4L2_PIX_FMT_YUYV, 3, 4, 4, 16, 0u, 0u, true},
#endif
  };

  for (const LayoutCase &c : refused) {
    v4l2_format f{};
    FillDriverFormat(&f, c);

    ActualFormat got{};
    std::string err;
    if (!Expect(!ParseChosenOutputFmt(f, c.mplane, /*fps=*/30, &got, &err),
                "rows the reported frame size cannot hold must be refused")) {
      std::cerr << "  " << c.name << ": got size_image " << got.size_image
                << "\n";
      return false;
    }

    if (!Expect(!err.empty(), "the refusal must name the reason")) {
      std::cerr << "  " << c.name << "\n";
      return false;
    }
  }

  // The two reports that look like the refusal but are not it: a frame size
  // of 0 is no report at all, and a raised row that still fits the frame the
  // driver sized keeps that frame.
  const LayoutCase accepted[] = {
      {"a stride with no frame size at all", V4L2_PIX_FMT_YUYV, 640, 480, 1280,
       0, 1280u, 614400u},
      {"a stride below the packed row is raised inside the frame",
       V4L2_PIX_FMT_YUYV, 3, 4, 4, 32, 8u, 32u},
      {"a stride of zero is raised inside the frame", V4L2_PIX_FMT_YUYV, 640,
       480, 0, 614400, 1280u, 614400u},
#ifdef V4L2_CAP_VIDEO_OUTPUT_MPLANE
      {"a stride with no frame size at all, mplane", V4L2_PIX_FMT_YUYV, 640,
       480, 1280, 0, 1280u, 614400u, true},
#endif
  };

  for (const LayoutCase &c : accepted) {
    v4l2_format f{};
    FillDriverFormat(&f, c);

    ActualFormat got{};
    std::string err;
    if (!Expect(ParseChosenOutputFmt(f, c.mplane, /*fps=*/30, &got, &err),
                "a report that is not self-contradictory must be accepted")) {
      std::cerr << "  " << c.name << ": " << err << "\n";
      return false;
    }

    if (!Expect(got.bytes_per_line == c.want_bytes_per_line &&
                    got.size_image == c.want_size_image,
                "the accepted report must take the implied layout")) {
      std::cerr << "  " << c.name << ": got " << got.bytes_per_line << " and "
                << got.size_image << "\n";
      return false;
    }
  }

  return true;
}

// Negotiation is a ladder: for each buffer type the writer asks S_FMT with a
// stride, then S_FMT without one, and if no type answers at all it walks the
// list again with G_FMT. A rung the driver answers is only usable if the
// layout in that answer parses, so a rung the parse refuses must fall through
// to the next one: the rung below often asks a question the driver answers
// self-consistently. A driver that pads the stride of a with-stride S_FMT and
// echoes the frame size back computes both numbers itself when the writer
// asks for no stride.
//
// If no rung gives a usable layout, the first refusal is the one the caller
// reports, because it names the rung the writer wanted most.
bool TestV4l2WriterFormatLadderStepsPastAParseRefusal() {
  using video::ChooseOutputFormat;
  using video::FormatLadderResult;
  using video::FormatLadderRung;

  // Builds a rung that answers with one driver report.
  auto Answer = [](const char *name, const LayoutCase &c) {
    FormatLadderRung r;
    r.name = name;
    r.mplane = c.mplane;
    r.ask = [c](v4l2_format *outFmt, std::string *) {
      FillDriverFormat(outFmt, c);
      return true;
    };
    return r;
  };

  // Builds a rung the driver refuses outright.
  auto Refuse = [](const char *name, const char *why) {
    FormatLadderRung r;
    r.name = name;
    r.ask = [why](v4l2_format *, std::string *outErr) {
      if (outErr)
        *outErr = why;
      return false;
    };
    return r;
  };

  // 640x480 YUYV padded to a 1536 byte row, with the frame size the writer
  // asked for echoed back: 480 rows of 1536 do not fit 614400 bytes.
  const LayoutCase contradictory = {"padded row, echoed frame size",
                                    V4L2_PIX_FMT_YUYV,
                                    640,
                                    480,
                                    1536,
                                    614400,
                                    0u,
                                    0u};
  const LayoutCase sane = {"driver sized both numbers itself",
                           V4L2_PIX_FMT_YUYV,
                           640,
                           480,
                           1280,
                           614400,
                           1280u,
                           614400u};

  {
    const std::vector<FormatLadderRung> rungs = {
        Answer("VIDEO_OUTPUT S_FMT(with stride)", contradictory),
        Answer("VIDEO_OUTPUT S_FMT(no stride)", sane),
    };

    const FormatLadderResult got = ChooseOutputFormat(rungs, /*fps=*/30);
    if (!Expect(got.ok, "the rung below a parse refusal must be tried"))
      return false;
    if (!Expect(got.rung == 1u, "the walk must keep the rung that parsed")) {
      std::cerr << "  kept rung " << got.rung << "\n";
      return false;
    }
    if (!Expect(got.actual.bytes_per_line == 1280u &&
                    got.actual.size_image == 614400u,
                "the kept rung must give its own layout")) {
      std::cerr << "  got " << got.actual.bytes_per_line << " and "
                << got.actual.size_image << "\n";
      return false;
    }
    if (!Expect(!got.attempt_log.empty(),
                "the refused rung must stay in the attempt log"))
      return false;
    // The refusal belongs to the failure message alone, so a walk that kept a
    // rung reports none: the layout it gives back is the whole answer.
    if (!Expect(got.first_refusal.empty(),
                "a walk that kept a rung must report no refusal")) {
      std::cerr << "  got '" << got.first_refusal << "'\n";
      return false;
    }
  }

  // A rung the driver refuses outright is walked past the same way, and the
  // walk goes on to the next buffer type.
  {
    const std::vector<FormatLadderRung> rungs = {
        Refuse("VIDEO_OUTPUT S_FMT(with stride)", "S_FMT failed: Invalid "
                                                  "argument"),
        Answer("VIDEO_OUTPUT S_FMT(no stride)", contradictory),
        Answer("VIDEO_CAPTURE S_FMT(with stride)", sane),
    };

    const FormatLadderResult got = ChooseOutputFormat(rungs, /*fps=*/30);
    if (!Expect(got.ok && got.rung == 2u,
                "the walk must reach the next buffer type")) {
      std::cerr << "  ok " << got.ok << ", kept rung " << got.rung << "\n";
      return false;
    }
  }

  // Every rung refused: the caller gets the first refusal, and a log of the
  // whole ladder.
  {
    const LayoutCase unsupported = {"MJPEG the writer cannot fill",
                                    V4L2_PIX_FMT_MJPEG,
                                    640,
                                    480,
                                    0,
                                    100000,
                                    0u,
                                    0u};

    const std::vector<FormatLadderRung> rungs = {
        Answer("VIDEO_OUTPUT S_FMT(with stride)", unsupported),
        Answer("VIDEO_OUTPUT S_FMT(no stride)", contradictory),
    };

    const FormatLadderResult got = ChooseOutputFormat(rungs, /*fps=*/30);
    if (!Expect(!got.ok, "a ladder no rung parses must fail"))
      return false;
    if (!Expect(got.first_refusal.find("MJPG") != std::string::npos,
                "the failure must keep the first refusal")) {
      std::cerr << "  got '" << got.first_refusal << "'\n";
      return false;
    }
    if (!Expect(got.attempt_log.find("S_FMT(no stride)") != std::string::npos,
                "the attempt log must name every rung tried")) {
      std::cerr << "  got '" << got.attempt_log << "'\n";
      return false;
    }
  }

  // The failure names the rung of the first refusal as well as its message,
  // thus the caller can point at that rung without printing its text twice.
  {
    const std::vector<FormatLadderRung> rungs = {
        Refuse("VIDEO_OUTPUT S_FMT(with stride)", "S_FMT failed: Invalid "
                                                  "argument"),
        Answer("VIDEO_OUTPUT S_FMT(no stride)", contradictory),
        Answer("VIDEO_CAPTURE S_FMT(with stride)", contradictory),
    };

    const FormatLadderResult got = ChooseOutputFormat(rungs, /*fps=*/30);
    if (!Expect(!got.ok, "a ladder no rung parses must fail"))
      return false;
    if (!Expect(got.first_refusal_rung == 1u,
                "the failure must name the rung of the first refusal")) {
      std::cerr << "  named rung " << got.first_refusal_rung << "\n";
      return false;
    }
  }

  return true;
}

// The writer gives the driver every frame with write(), which is the file I/O
// method `V4L2_CAP_READWRITE` advertises. A device without that cap fails
// every frame, whatever the format says, so negotiation must refuse it while
// the caps are still in hand. Without the check the open succeeds and each
// frame fails on its own, and none of those failures names the cause.
bool TestV4l2WriterRefusesADeviceThatCannotTakeWrites() {
  using video::OutputDeviceCanWrite;

  std::string err;
  if (!Expect(OutputDeviceCanWrite(V4L2_CAP_VIDEO_OUTPUT | V4L2_CAP_READWRITE |
                                       V4L2_CAP_STREAMING,
                                   &err),
              "a device that advertises READWRITE must be accepted")) {
    std::cerr << "  " << err << "\n";
    return false;
  }

  // v4l2loopback advertises CAPTURE and READWRITE on the producer side, which
  // is the device StudioCast writes to.
  if (!Expect(OutputDeviceCanWrite(V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_READWRITE |
                                       V4L2_CAP_STREAMING,
                                   &err),
              "the v4l2loopback caps must be accepted")) {
    std::cerr << "  " << err << "\n";
    return false;
  }

  const __u32 refused[] = {
      V4L2_CAP_VIDEO_OUTPUT | V4L2_CAP_STREAMING,
      V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING,
      0u,
  };

  for (const __u32 caps : refused) {
    err.clear();
    if (!Expect(!OutputDeviceCanWrite(caps, &err),
                "a device without READWRITE must be refused")) {
      std::cerr << "  caps 0x" << std::hex << caps << std::dec << "\n";
      return false;
    }

    if (!Expect(!err.empty(), "the refusal must name the reason")) {
      std::cerr << "  caps 0x" << std::hex << caps << std::dec << "\n";
      return false;
    }
  }

  return true;
}

// Some v4l2loopback configurations transiently report a "blank" format, of
// width and height 0, while a consumer disconnects or a renegotiation window
// is open. A frame of no rows takes no bytes, so `WriteFrame` would push
// nothing at all and the output would stay silent until something
// renegotiated. `RefreshActual` has always refused such a report; the parse
// must refuse it too, because the ladder is the machinery that steps past a
// rung the writer cannot use, and this is one of those rungs.
bool TestV4l2WriterFormatLadderRefusesABlankFrameReport() {
  using video::ActualFormat;
  using video::ChooseOutputFormat;
  using video::FormatLadderResult;
  using video::FormatLadderRung;
  using video::ParseChosenOutputFmt;

  const LayoutCase blank[] = {
      {"no width and no height", V4L2_PIX_FMT_YUYV, 0, 0, 0, 0, 0u, 0u},
      {"no height", V4L2_PIX_FMT_YUYV, 640, 0, 1280, 0, 0u, 0u},
      {"no width", V4L2_PIX_FMT_YUYV, 0, 480, 0, 614400, 0u, 0u},
#ifdef V4L2_CAP_VIDEO_OUTPUT_MPLANE
      {"no width and no height, mplane", V4L2_PIX_FMT_YUYV, 0, 0, 0, 0, 0u, 0u,
       true},
#endif
  };

  for (const LayoutCase &c : blank) {
    v4l2_format f{};
    FillDriverFormat(&f, c);

    ActualFormat got{};
    std::string err;
    if (!Expect(!ParseChosenOutputFmt(f, c.mplane, /*fps=*/30, &got, &err),
                "a blank frame report must be refused")) {
      std::cerr << "  " << c.name << ": got " << got.width << "x" << got.height
                << " size_image " << got.size_image << "\n";
      return false;
    }

    if (!Expect(!err.empty(), "the refusal must name the reason")) {
      std::cerr << "  " << c.name << "\n";
      return false;
    }
  }

  // Builds a rung that answers with one driver report.
  auto Answer = [](const char *name, const LayoutCase &c) {
    FormatLadderRung r;
    r.name = name;
    r.mplane = c.mplane;
    r.ask = [c](v4l2_format *outFmt, std::string *) {
      FillDriverFormat(outFmt, c);
      return true;
    };
    return r;
  };

  const LayoutCase real = {"the frame the device holds",
                           V4L2_PIX_FMT_YUYV,
                           640,
                           480,
                           1280,
                           614400,
                           1280u,
                           614400u};

  // The rung below a blank report is the one the driver answers once the
  // window closes, so the walk must reach it.
  {
    const std::vector<FormatLadderRung> rungs = {
        Answer("VIDEO_OUTPUT S_FMT(with stride)", blank[0]),
        Answer("VIDEO_OUTPUT S_FMT(no stride)", real),
    };

    const FormatLadderResult got = ChooseOutputFormat(rungs, /*fps=*/30);
    if (!Expect(got.ok && got.rung == 1u,
                "the rung below a blank report must be kept")) {
      std::cerr << "  ok " << got.ok << ", kept rung " << got.rung << "\n";
      return false;
    }
    if (!Expect(got.actual.width == 640 && got.actual.height == 480 &&
                    got.actual.size_image == 614400u,
                "the kept rung must give the frame the writer fills")) {
      std::cerr << "  got " << got.actual.width << "x" << got.actual.height
                << " size_image " << got.actual.size_image << "\n";
      return false;
    }
  }

  // A ladder of blank rungs alone gives no layout, and the failure names the
  // blank report rather than an empty frame the writer would never fill.
  {
    const std::vector<FormatLadderRung> rungs = {
        Answer("VIDEO_OUTPUT S_FMT(with stride)", blank[0]),
        Answer("VIDEO_OUTPUT S_FMT(no stride)", blank[1]),
    };

    const FormatLadderResult got = ChooseOutputFormat(rungs, /*fps=*/30);
    if (!Expect(!got.ok, "a ladder of blank rungs alone must fail")) {
      std::cerr << "  kept rung " << got.rung << " with size_image "
                << got.actual.size_image << "\n";
      return false;
    }
    if (!Expect(got.first_refusal.find("blank frame") != std::string::npos,
                "the failure must name the blank report")) {
      std::cerr << "  got '" << got.first_refusal << "'\n";
      return false;
    }
  }

  return true;
}

// `size_image` is not only the count of bytes each write() sends: the
// pipeline and the feed give their frame buffers that size. A report the
// writer takes at face value thus sizes an allocation, and a driver that
// reports `sizeimage = 0` gets its frame size from the row alone, where a
// nonsense row of 4 GB asks for a 4 TB buffer.
//
// So the writer takes no frame larger than a documented maximum. 8K RGB24 is
// 95 MiB, thus the bound is far above every frame this daemon sends and no
// real report comes near it.
bool TestV4l2WriterRefusesAFrameLargerThanItCanHold() {
  using video::ActualFormat;
  using video::ParseChosenOutputFmt;

  const LayoutCase refused[] = {
      // No frame size at all, and a row of 4 GB: the implied frame is 4.3 TB.
      {"a nonsense row with no frame size", V4L2_PIX_FMT_YUYV, 1920, 1080,
       4000000000u, 0, 0u, 0u},
      // A report that agrees with itself, and still names a 3.9 GB frame.
      {"a self-consistent frame of gigabytes", V4L2_PIX_FMT_YUYV, 1920, 1000,
       3900000u, 3900000000u, 0u, 0u},
#ifdef V4L2_CAP_VIDEO_OUTPUT_MPLANE
      {"a nonsense row with no frame size, mplane", V4L2_PIX_FMT_YUYV, 1920,
       1080, 4000000000u, 0, 0u, 0u, true},
#endif
  };

  for (const LayoutCase &c : refused) {
    v4l2_format f{};
    FillDriverFormat(&f, c);

    ActualFormat got{};
    std::string err;
    if (!Expect(!ParseChosenOutputFmt(f, c.mplane, /*fps=*/30, &got, &err),
                "a frame larger than the writer takes must be refused")) {
      std::cerr << "  " << c.name << ": got size_image " << got.size_image
                << "\n";
      return false;
    }

    if (!Expect(!err.empty(), "the refusal must name the reason")) {
      std::cerr << "  " << c.name << "\n";
      return false;
    }
  }

  // The bound sits far above the largest frame the daemon sends, thus an 8K
  // RGB24 frame of 95 MiB still parses.
  const LayoutCase accepted[] = {
      {"8K RGB24 stays inside the bound", V4L2_PIX_FMT_RGB24, 7680, 4320, 23040,
       99532800u, 23040u, 99532800u},
  };

  for (const LayoutCase &c : accepted) {
    v4l2_format f{};
    FillDriverFormat(&f, c);

    ActualFormat got{};
    std::string err;
    if (!Expect(ParseChosenOutputFmt(f, c.mplane, /*fps=*/30, &got, &err),
                "a large but plausible frame must be accepted")) {
      std::cerr << "  " << c.name << ": " << err << "\n";
      return false;
    }

    if (!Expect(got.bytes_per_line == c.want_bytes_per_line &&
                    got.size_image == c.want_size_image,
                "the accepted report must take the implied layout")) {
      std::cerr << "  " << c.name << ": got " << got.bytes_per_line << " and "
                << got.size_image << "\n";
      return false;
    }
  }

  return true;
}

// The walk goes on past an S_FMT the driver accepted whose answer the parse
// refused, thus a walk that fails altogether can leave the device holding a
// format the writer named as one it cannot use. That is not the format the
// device held before the writer asked, and no other opener asked for it.
//
// So the walk reads the format the device holds before the first rung that
// changes it, and puts that format back when no rung gives a usable layout.
// A rung that asks G_FMT changes nothing, thus a walk in which only such
// rungs answered gets no S_FMT, and a ladder of them alone is never saved
// from at all.
bool TestV4l2WriterFormatLadderPutsTheDeviceFormatBack() {
  using video::ChooseOutputFormat;
  using video::FormatLadderResult;
  using video::FormatLadderRung;
  using video::FormatRestore;

  // Builds a rung that answers with one driver report. `mutates` tells the
  // walk whether the question changes the format the device holds: S_FMT
  // does, G_FMT does not.
  auto Answer = [](const char *name, const LayoutCase &c, bool mutates) {
    FormatLadderRung r;
    r.name = name;
    r.mplane = c.mplane;
    r.mutates = mutates;
    r.ask = [c](v4l2_format *outFmt, std::string *) {
      FillDriverFormat(outFmt, c);
      return true;
    };
    return r;
  };

  // Builds an S_FMT rung the driver refuses outright.
  auto Refuse = [](const char *name) {
    FormatLadderRung r;
    r.name = name;
    r.mutates = true;
    r.ask = [](v4l2_format *, std::string *outErr) {
      if (outErr)
        *outErr = "VIDIOC_S_FMT failed: Invalid argument";
      return false;
    };
    return r;
  };

  // The format the device holds before the writer asks anything.
  const LayoutCase held = {"the format the device already holds",
                           V4L2_PIX_FMT_YUYV,
                           1920,
                           1080,
                           3840,
                           4147200,
                           3840u,
                           4147200u};
  // MJPEG is a format the writer cannot fill, so every rung of it is refused.
  const LayoutCase unusable = {"MJPEG the writer cannot fill",
                               V4L2_PIX_FMT_MJPEG,
                               640,
                               480,
                               0,
                               100000,
                               0u,
                               0u};
  const LayoutCase sane = {"driver sized both numbers itself",
                           V4L2_PIX_FMT_YUYV,
                           640,
                           480,
                           1280,
                           614400,
                           1280u,
                           614400u};

  int saves = 0;
  std::vector<int> restored;
  FormatRestore guard;
  guard.save = [&](const FormatLadderRung &, v4l2_format *outFmt) {
    ++saves;
    FillDriverFormat(outFmt, held);
    return true;
  };
  guard.restore = [&](const v4l2_format &f) {
    restored.push_back(static_cast<int>(f.fmt.pix.width));
  };

  // A walk that ends with no usable layout puts the saved format back.
  {
    saves = 0;
    restored.clear();
    const std::vector<FormatLadderRung> rungs = {
        Answer("VIDEO_OUTPUT S_FMT(with stride)", unusable, true),
        Answer("VIDEO_OUTPUT S_FMT(no stride)", unusable, true),
    };

    const FormatLadderResult got = ChooseOutputFormat(rungs, /*fps=*/30, guard);
    if (!Expect(!got.ok, "a ladder no rung parses must fail"))
      return false;
    if (!Expect(saves == 1, "the walk must read the held format once")) {
      std::cerr << "  saved " << saves << " times\n";
      return false;
    }
    if (!Expect(restored.size() == 1u && restored[0] == 1920,
                "the failed walk must put the held format back")) {
      std::cerr << "  restored " << restored.size() << " formats\n";
      return false;
    }
  }

  // A walk that keeps a rung leaves the device holding that rung's format.
  {
    saves = 0;
    restored.clear();
    const std::vector<FormatLadderRung> rungs = {
        Answer("VIDEO_OUTPUT S_FMT(with stride)", unusable, true),
        Answer("VIDEO_OUTPUT S_FMT(no stride)", sane, true),
    };

    const FormatLadderResult got = ChooseOutputFormat(rungs, /*fps=*/30, guard);
    if (!Expect(got.ok && got.rung == 1u, "the walk must keep the sane rung"))
      return false;
    if (!Expect(restored.empty(),
                "a walk that kept a rung must not put anything back")) {
      std::cerr << "  restored " << restored.size() << " formats\n";
      return false;
    }
  }

  // A device that answered no rung was never asked to take a format, thus it
  // needs no S_FMT to put one back.
  {
    saves = 0;
    restored.clear();
    const std::vector<FormatLadderRung> rungs = {
        Refuse("VIDEO_OUTPUT S_FMT(with stride)"),
        Refuse("VIDEO_OUTPUT S_FMT(no stride)"),
    };

    const FormatLadderResult got = ChooseOutputFormat(rungs, /*fps=*/30, guard);
    if (!Expect(!got.ok, "a ladder no rung answers must fail"))
      return false;
    if (!Expect(restored.empty(),
                "a device the walk did not change must get no S_FMT")) {
      std::cerr << "  restored " << restored.size() << " formats\n";
      return false;
    }
  }

  // A G_FMT rung answers a format without asking the device to take one, thus
  // a walk in which only such rungs answered changed nothing and needs no
  // S_FMT to put anything back. The walk does not even read the held format,
  // because it walks past no rung that could change it.
  {
    saves = 0;
    restored.clear();
    const std::vector<FormatLadderRung> rungs = {
        Answer("VIDEO_OUTPUT G_FMT", unusable, false),
        Answer("VIDEO_CAPTURE G_FMT", unusable, false),
    };

    const FormatLadderResult got = ChooseOutputFormat(rungs, /*fps=*/30, guard);
    if (!Expect(!got.ok, "a ladder of unusable G_FMT rungs must fail"))
      return false;
    if (!Expect(saves == 0, "a walk that changes nothing must read nothing")) {
      std::cerr << "  saved " << saves << " times\n";
      return false;
    }
    if (!Expect(restored.empty(),
                "a rung that changes nothing must earn no S_FMT")) {
      std::cerr << "  restored " << restored.size() << " formats\n";
      return false;
    }
  }

  // The held format is read before the first rung that changes it, and only
  // that once: a walk whose first rung asks nothing of the device still puts
  // back the format the device held before the S_FMT rungs below it.
  {
    saves = 0;
    restored.clear();
    const std::vector<FormatLadderRung> rungs = {
        Answer("VIDEO_OUTPUT G_FMT", unusable, false),
        Answer("VIDEO_OUTPUT S_FMT(with stride)", unusable, true),
        Answer("VIDEO_OUTPUT S_FMT(no stride)", unusable, true),
    };

    const FormatLadderResult got = ChooseOutputFormat(rungs, /*fps=*/30, guard);
    if (!Expect(!got.ok, "a ladder no rung parses must fail"))
      return false;
    if (!Expect(saves == 1, "the walk must read the held format once")) {
      std::cerr << "  saved " << saves << " times\n";
      return false;
    }
    if (!Expect(restored.size() == 1u && restored[0] == 1920,
                "the failed walk must put the held format back")) {
      std::cerr << "  restored " << restored.size() << " formats\n";
      return false;
    }
  }

  // The walk still runs when the caller gives it no way to save or restore.
  {
    const std::vector<FormatLadderRung> rungs = {
        Answer("VIDEO_OUTPUT S_FMT(no stride)", sane, true),
    };

    const FormatLadderResult got = ChooseOutputFormat(rungs, /*fps=*/30);
    if (!Expect(got.ok, "a walk without a restore must still keep its rung"))
      return false;
  }

  return true;
}

// The format the walk saves goes back to the device with S_FMT, thus it must
// be a format the device can be asked to take again. A blank report is not:
// S_FMT of 0x0 does not fail on v4l2loopback, it takes the driver's default
// geometry, so "putting it back" would change the frame every consumer sees.
// The window in which the driver gives that report is the consumer-disconnect
// window, which is also the window in which the walk most often fails.
//
// So the save asks the same question of the held format that the parse asks
// of a rung: `FrameIsUsable`. A format the writer cannot fill is still a
// format the device held, thus only the blank is refused. A save that answers
// no leaves the device alone, which is the right answer for a report the
// writer already decided it cannot read.
bool TestV4l2WriterRestoresOnlyAFormatItCouldUse() {
  using video::ChooseOutputFormat;
  using video::FormatLadderResult;
  using video::FormatLadderRung;
  using video::FormatRestore;
  using video::SavedOutputFmtIsRestorable;

  struct SavedCase {
    const char *name;
    LayoutCase report;
    bool want;
  };

  const SavedCase cases[] = {
      {"the format a live device holds",
       {"1920x1080 YUYV", V4L2_PIX_FMT_YUYV, 1920, 1080, 3840, 4147200, 0u, 0u},
       true},
      // The writer cannot fill MJPEG, but the device held it before the walk
      // asked for anything, thus putting it back is leaving the device alone.
      {"a format the writer cannot fill",
       {"MJPEG", V4L2_PIX_FMT_MJPEG, 640, 480, 0, 100000, 0u, 0u},
       true},
      {"the blank report of a disconnect window",
       {"0x0", V4L2_PIX_FMT_YUYV, 0, 0, 0, 0, 0u, 0u},
       false},
      {"a report of no columns",
       {"0x480", V4L2_PIX_FMT_YUYV, 0, 480, 0, 0, 0u, 0u},
       false},
      {"a report of no rows",
       {"640x0", V4L2_PIX_FMT_YUYV, 640, 0, 1280, 0, 0u, 0u},
       false},
#ifdef V4L2_CAP_VIDEO_OUTPUT_MPLANE
      {"the mplane arm of a live device",
       {"1920x1080 YUYV mplane", V4L2_PIX_FMT_YUYV, 1920, 1080, 3840, 4147200,
        0u, 0u, true},
       true},
      {"the mplane arm of a blank report",
       {"0x0 mplane", V4L2_PIX_FMT_YUYV, 0, 0, 0, 0, 0u, 0u, true},
       false},
#endif
  };

  for (const SavedCase &c : cases) {
    v4l2_format f{};
    FillDriverFormat(&f, c.report);
    const bool got = SavedOutputFmtIsRestorable(f, c.report.mplane);
    if (!Expect(got == c.want, "the save must refuse a blank report alone")) {
      std::cerr << "  " << c.name << ": expected " << c.want << ", got " << got
                << "\n";
      return false;
    }
  }

  // A save that answers no leaves the device alone, even after a rung that
  // changed the format the device holds.
  {
    const LayoutCase unusable = {"MJPEG the writer cannot fill",
                                 V4L2_PIX_FMT_MJPEG,
                                 640,
                                 480,
                                 0,
                                 100000,
                                 0u,
                                 0u};

    auto SetFmt = [](const char *name, const LayoutCase &c) {
      FormatLadderRung r;
      r.name = name;
      r.mplane = c.mplane;
      r.mutates = true;
      r.ask = [c](v4l2_format *outFmt, std::string *) {
        FillDriverFormat(outFmt, c);
        return true;
      };
      return r;
    };

    int restores = 0;
    FormatRestore guard;
    guard.save = [](const FormatLadderRung &, v4l2_format *) { return false; };
    guard.restore = [&](const v4l2_format &) { ++restores; };

    const std::vector<FormatLadderRung> rungs = {
        SetFmt("VIDEO_OUTPUT S_FMT(with stride)", unusable),
        SetFmt("VIDEO_OUTPUT S_FMT(no stride)", unusable),
    };

    const FormatLadderResult got = ChooseOutputFormat(rungs, /*fps=*/30, guard);
    if (!Expect(!got.ok, "a ladder no rung parses must fail"))
      return false;
    if (!Expect(restores == 0,
                "a walk that saved nothing must put nothing back")) {
      std::cerr << "  restored " << restores << " formats\n";
      return false;
    }
  }

  return true;
}

// The restore sends the saved format back with S_FMT under the buffer type
// the save read it from, thus that type must be a type the walk changed. The
// blank the save refuses is the report of a disconnect window, and it is the
// output side that gives it: a save free to look at another type would answer
// with the capture side, which the output rungs never touched, and the
// restore would then S_FMT a type the walk never asked to take anything.
//
// So the save is asked for one buffer type at a time, the type of the rung
// that is about to change it, and the walk puts back only the types a rung
// of that type answered.
bool TestV4l2WriterRestoresOnlyTheBufferTypeARungChanged() {
  using video::ChooseOutputFormat;
  using video::FormatLadderResult;
  using video::FormatLadderRung;
  using video::FormatRestore;

  const LayoutCase held = {"the format the device already holds",
                           V4L2_PIX_FMT_YUYV,
                           1920,
                           1080,
                           3840,
                           4147200,
                           3840u,
                           4147200u};
  const LayoutCase unusable = {"MJPEG the writer cannot fill",
                               V4L2_PIX_FMT_MJPEG,
                               640,
                               480,
                               0,
                               100000,
                               0u,
                               0u};

  // An S_FMT rung of one buffer type that the driver answers with a report.
  auto Answer = [](const char *name, __u32 buf_type, const LayoutCase &c) {
    FormatLadderRung r;
    r.name = name;
    r.buf_type = buf_type;
    r.mutates = true;
    r.ask = [c](v4l2_format *outFmt, std::string *) {
      FillDriverFormat(outFmt, c);
      return true;
    };
    return r;
  };

  // An S_FMT rung of one buffer type that the driver refuses outright.
  auto Refuse = [](const char *name, __u32 buf_type) {
    FormatLadderRung r;
    r.name = name;
    r.buf_type = buf_type;
    r.mutates = true;
    r.ask = [](v4l2_format *, std::string *outErr) {
      if (outErr)
        *outErr = "VIDIOC_S_FMT failed: Invalid argument";
      return false;
    };
    return r;
  };

  // The output side is blank, which is the disconnect window; the capture
  // side holds a live format.
  std::vector<__u32> asked;
  std::vector<__u32> restored;
  FormatRestore guard;
  guard.save = [&](const FormatLadderRung &r, v4l2_format *outFmt) {
    asked.push_back(r.buf_type);
    if (r.buf_type == V4L2_BUF_TYPE_VIDEO_OUTPUT)
      return false;
    FillDriverFormat(outFmt, held);
    // G_FMT answers under the type it was asked about, and the restore sends
    // the struct back as it stands, thus the type rides along with it.
    outFmt->type = r.buf_type;
    return true;
  };
  guard.restore = [&](const v4l2_format &f) { restored.push_back(f.type); };

  // The output rungs are the only ones the driver answered, and the output
  // side had nothing to put back, thus the walk leaves the device alone.
  {
    asked.clear();
    restored.clear();
    const std::vector<FormatLadderRung> rungs = {
        Answer("VIDEO_OUTPUT S_FMT(with stride)", V4L2_BUF_TYPE_VIDEO_OUTPUT,
               unusable),
        Refuse("VIDEO_CAPTURE S_FMT(with stride)", V4L2_BUF_TYPE_VIDEO_CAPTURE),
    };

    const FormatLadderResult got = ChooseOutputFormat(rungs, /*fps=*/30, guard);
    if (!Expect(!got.ok, "a ladder no rung parses must fail"))
      return false;
    if (!Expect(asked.size() == 2u && asked[0] == V4L2_BUF_TYPE_VIDEO_OUTPUT &&
                    asked[1] == V4L2_BUF_TYPE_VIDEO_CAPTURE,
                "the walk must read each buffer type it is about to change")) {
      std::cerr << "  read " << asked.size() << " buffer types\n";
      return false;
    }
    if (!Expect(restored.empty(),
                "a buffer type no rung answered must get no S_FMT")) {
      std::cerr << "  restored " << restored.size() << " formats\n";
      return false;
    }
  }

  // The capture side is the side a rung answered, thus it is the side the
  // walk puts back, and it puts back the format that side held.
  {
    asked.clear();
    restored.clear();
    const std::vector<FormatLadderRung> rungs = {
        Refuse("VIDEO_OUTPUT S_FMT(with stride)", V4L2_BUF_TYPE_VIDEO_OUTPUT),
        Answer("VIDEO_CAPTURE S_FMT(with stride)", V4L2_BUF_TYPE_VIDEO_CAPTURE,
               unusable),
    };

    const FormatLadderResult got = ChooseOutputFormat(rungs, /*fps=*/30, guard);
    if (!Expect(!got.ok, "a ladder no rung parses must fail"))
      return false;
    if (!Expect(restored.size() == 1u &&
                    restored[0] == V4L2_BUF_TYPE_VIDEO_CAPTURE,
                "the walk must put back the format of the type it changed")) {
      std::cerr << "  restored " << restored.size() << " formats\n";
      return false;
    }
  }

  // A type is read once, however many rungs of it the walk goes past.
  {
    asked.clear();
    restored.clear();
    const std::vector<FormatLadderRung> rungs = {
        Answer("VIDEO_CAPTURE S_FMT(with stride)", V4L2_BUF_TYPE_VIDEO_CAPTURE,
               unusable),
        Answer("VIDEO_CAPTURE S_FMT(no stride)", V4L2_BUF_TYPE_VIDEO_CAPTURE,
               unusable),
    };

    const FormatLadderResult got = ChooseOutputFormat(rungs, /*fps=*/30, guard);
    if (!Expect(!got.ok, "a ladder no rung parses must fail"))
      return false;
    if (!Expect(asked.size() == 1u, "the walk must read a buffer type once")) {
      std::cerr << "  read " << asked.size() << " buffer types\n";
      return false;
    }
    if (!Expect(restored.size() == 1u,
                "the walk must put a buffer type back once")) {
      std::cerr << "  restored " << restored.size() << " formats\n";
      return false;
    }
  }

  return true;
}

// The pipeline gives a failed writer open a five second retry budget, for the
// window in which v4l2loopback answers a producer that opened it a moment too
// early. It knows that window by the message the open failed with.
//
// EINVAL is one face of the window. The blank report is the other: the driver
// answers, and the frame it names has no rows, which is the report it gives
// while a consumer disconnects. Both are gone a moment later, thus both must
// read as transient or the budget cannot heal the window it was built for.
//
// A report the driver stands by is not transient. A device that takes no
// write(), a plane count the writer cannot use and a report that contradicts
// itself all say the same thing on every retry.
bool TestV4l2WriterNamesTheRefusalsARetryCanOutlive() {
  using video::ChooseOutputFormat;
  using video::ComposeLadderFailure;
  using video::FormatLadderResult;
  using video::FormatLadderRung;
  using video::OutputOpenErrorIsTransient;
  using video::ParseChosenOutputFmt;

  // The refusal the parse writes for the blank report of a disconnect window,
  // taken from the parse itself so that the two stay one string.
  const LayoutCase blank = {"0x0", V4L2_PIX_FMT_YUYV, 0, 0, 0, 0, 0u, 0u};
  v4l2_format f{};
  FillDriverFormat(&f, blank);
  video::ActualFormat a;
  std::string blankErr;
  if (!Expect(
          !ParseChosenOutputFmt(f, /*mplane=*/false, /*fps=*/30, &a, &blankErr),
          "the parse must refuse a blank report"))
    return false;
  if (!Expect(OutputOpenErrorIsTransient(blankErr),
              "the blank report must read as transient")) {
    std::cerr << "  got '" << blankErr << "'\n";
    return false;
  }

  // The pipeline reads the whole composed failure, not the refusal alone, so
  // the cases below compose it the way the writer composes it rather than
  // write a shape the walk does not produce.
  const char *const header =
      "Failed to set/query format for /dev/video10 (desired=YUYV, 1280x720)\n"
      "querycap.driver=v4l2 loopback card=StudioCast bus=platform\n";

  // An S_FMT rung the driver answers EINVAL, which is the answer every
  // single-plane rung gets on an mplane-only device.
  auto Refuse = [](const char *name) {
    FormatLadderRung r;
    r.name = name;
    r.mutates = true;
    r.ask = [](v4l2_format *, std::string *outErr) {
      if (outErr)
        *outErr = "VIDIOC_S_FMT(VIDEO_OUTPUT) failed: Invalid argument";
      return false;
    };
    return r;
  };

  // An S_FMT rung the driver answers with one report.
  auto Answer = [](const char *name, const LayoutCase &c,
                   std::uint8_t num_planes = 1) {
    FormatLadderRung r;
    r.name = name;
    r.mplane = c.mplane;
    r.mutates = true;
    r.ask = [c, num_planes](v4l2_format *outFmt, std::string *) {
      FillDriverFormat(outFmt, c, num_planes);
      return true;
    };
    return r;
  };

  // A walk stopped by the blank report of a disconnect window is one the
  // budget can heal, and the rung it left behind does not change that.
  {
    const std::vector<FormatLadderRung> rungs = {
        Refuse("VIDEO_OUTPUT S_FMT(with stride)"),
        Answer("VIDEO_OUTPUT S_FMT(no stride)", blank),
    };
    const FormatLadderResult ladder = ChooseOutputFormat(rungs, /*fps=*/30);
    const std::string composed = ComposeLadderFailure(header, rungs, ladder);
    if (!Expect(OutputOpenErrorIsTransient(composed),
                "a walk stopped by a blank report must read as transient")) {
      std::cerr << "  '" << composed << "'\n";
      return false;
    }
  }

  // The refusal that stopped the walk is the one that answers. The attempt
  // log below it names every rung the driver refused, with the errno of each,
  // thus a refusal the driver stands by must stay permanent even though a
  // rung the walk left behind answered EINVAL.
  {
    const LayoutCase contradiction = {"rows the frame size cannot hold",
                                      V4L2_PIX_FMT_YUYV,
                                      640,
                                      480,
                                      1536,
                                      614400,
                                      0u,
                                      0u};
    const std::vector<FormatLadderRung> rungs = {
        Refuse("VIDEO_OUTPUT S_FMT(with stride)"),
        Answer("VIDEO_OUTPUT S_FMT(no stride)", contradiction),
    };
    const FormatLadderResult ladder = ChooseOutputFormat(rungs, /*fps=*/30);
    const std::string composed = ComposeLadderFailure(header, rungs, ladder);
    if (!Expect(!OutputOpenErrorIsTransient(composed),
                "a report that contradicts itself must not read as transient "
                "because a rung the walk left behind answered EINVAL")) {
      std::cerr << "  '" << composed << "'\n";
      return false;
    }
  }

#ifdef V4L2_CAP_VIDEO_OUTPUT_MPLANE
  // The device that answers a plane count the writer cannot use is an
  // mplane-only device, thus its single-plane rungs answer EINVAL and the
  // composed failure carries both. This is the shape the pipeline reads on
  // such a device, and the plane count cannot change while the writer waits.
  {
    const LayoutCase two_planes = {"an mplane report of two planes",
                                   V4L2_PIX_FMT_YUYV,
                                   640,
                                   480,
                                   1280,
                                   614400,
                                   0u,
                                   0u,
                                   true};
    const std::vector<FormatLadderRung> rungs = {
        Refuse("VIDEO_OUTPUT S_FMT(with stride)"),
        Answer("VIDEO_OUTPUT_MPLANE S_FMT(with stride)", two_planes,
               /*num_planes=*/2),
    };
    const FormatLadderResult ladder = ChooseOutputFormat(rungs, /*fps=*/30);
    const std::string composed = ComposeLadderFailure(header, rungs, ladder);
    if (!Expect(!OutputOpenErrorIsTransient(composed),
                "a plane count the writer cannot use must not read as "
                "transient because a rung the walk left behind answered "
                "EINVAL")) {
      std::cerr << "  '" << composed << "'\n";
      return false;
    }
  }
#endif

  struct TransientCase {
    const char *error;
    bool want;
  };

  const TransientCase cases[] = {
      {"VIDIOC_S_FMT failed: Invalid argument", true},
      {"Device does not support write(): it does not advertise "
       "V4L2_CAP_READWRITE.",
       false},
      {"mplane format returned num_planes=2, only one plane is supported",
       false},
      {"Driver reported bytesperline=1536 and sizeimage=614400, but 480 rows "
       "of 1536 bytes do not fit",
       false},
      {"", false},
  };

  for (const TransientCase &c : cases) {
    const bool got = OutputOpenErrorIsTransient(c.error);
    if (!Expect(got == c.want, "a refusal the driver stands by must not be "
                               "retried, and a transient one must be")) {
      std::cerr << "  '" << c.error << "': expected " << c.want << ", got "
                << got << "\n";
      return false;
    }
  }

  return true;
}
} // namespace studiocast::tests
