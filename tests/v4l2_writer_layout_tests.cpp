#include "core/video/v4l2_writer.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <string>

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
// arm follows `c.mplane`, which is what `ParseChosenFormat` has to read.
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
// converter writes into it. The frame size then follows the row.
bool TestV4l2WriterFormatParseReadsBothUnionArms() {
  using video::ActualFormat;
  using video::ParseChosenFormat;
  using video::PixelFormat;

  const LayoutCase cases[] = {
      {"even YUYV width, packed driver stride", V4L2_PIX_FMT_YUYV, 640, 480,
       1280, 614400, 1280u, 614400u},
      {"YUYV padded row is kept", V4L2_PIX_FMT_YUYV, 640, 480, 1536, 737280,
       1536u, 737280u},
      {"odd YUYV width, packed driver stride", V4L2_PIX_FMT_YUYV, 3, 4, 8, 32,
       8u, 32u},
      {"YUYV stride below the packed row is raised", V4L2_PIX_FMT_YUYV, 3, 4, 4,
       16, 8u, 32u},
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
       3, 4, 4, 16, 8u, 32u, true},
      {"odd RGB24 width, packed driver stride, mplane", V4L2_PIX_FMT_RGB24, 3,
       4, 9, 36, 9u, 36u, true},
#endif
  };

  for (const LayoutCase &c : cases) {
    v4l2_format f{};
    FillDriverFormat(&f, c);

    ActualFormat got{};
    std::string err;
    if (!Expect(ParseChosenFormat(f, c.mplane, /*fps=*/30, &got, &err),
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
    if (!Expect(!ParseChosenFormat(f, c.mplane, /*fps=*/30, &got, &err),
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
  using video::ParseChosenFormat;

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
    if (!Expect(!ParseChosenFormat(f, /*mplane=*/true, /*fps=*/30, &got, &err),
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
  return Expect(ParseChosenFormat(ok, /*mplane=*/true, /*fps=*/30, &got, &err),
                "an mplane report of one plane must still be accepted");
#else
  return true;
#endif
}

} // namespace studiocast::tests
