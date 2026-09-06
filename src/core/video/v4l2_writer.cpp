#include "v4l2_writer.h"

#include <cerrno>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <sstream>
#include <string>

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

// One spelling of "these headers know the multi-planar types". The two cap
// macros have shipped together since Linux 2.6.39, but the mplane helpers
// below are shared by the output and the capture type lists, thus a guard on
// one family alone can put a capture-mplane type in a list that no helper can
// answer.
#if defined(V4L2_CAP_VIDEO_OUTPUT_MPLANE) ||                                   \
    defined(V4L2_CAP_VIDEO_CAPTURE_MPLANE)
#define STUDIOCAST_V4L2_HAS_MPLANE 1
#else
#define STUDIOCAST_V4L2_HAS_MPLANE 0
#endif

namespace studiocast::video {
namespace {

std::string ToLowerAscii(std::string s) {
  for (char &c : s) {
    if (c >= 'A' && c <= 'Z')
      c = static_cast<char>(c - 'A' + 'a');
  }
  return s;
}

int IoctlRetry(int fd, unsigned long req, void *arg) {
  for (;;) {
    const int r = ::ioctl(fd, req, arg);
    if (r == 0)
      return 0;
    if (errno == EINTR)
      continue;
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

std::uint32_t FourccFor(PixelFormat fmt) {
  switch (fmt) {
  case PixelFormat::yuyv:
    return V4L2_PIX_FMT_YUYV;
  case PixelFormat::rgb24:
    return V4L2_PIX_FMT_RGB24;
  }
  return V4L2_PIX_FMT_YUYV;
}

std::optional<PixelFormat> PixelFormatFromFourcc(std::uint32_t f) {
  if (f == V4L2_PIX_FMT_YUYV)
    return PixelFormat::yuyv;
  if (f == V4L2_PIX_FMT_RGB24)
    return PixelFormat::rgb24;
  return std::nullopt;
}

bool IsOutputBufType(__u32 t) {
  if (t == V4L2_BUF_TYPE_VIDEO_OUTPUT)
    return true;
#ifdef V4L2_CAP_VIDEO_OUTPUT_MPLANE
  if (t == V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE)
    return true;
#endif
  return false;
}

const char *BufTypeName(__u32 t) {
  switch (t) {
  case V4L2_BUF_TYPE_VIDEO_OUTPUT:
    return "VIDEO_OUTPUT";
  case V4L2_BUF_TYPE_VIDEO_CAPTURE:
    return "VIDEO_CAPTURE";
#ifdef V4L2_CAP_VIDEO_OUTPUT_MPLANE
  case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
    return "VIDEO_OUTPUT_MPLANE";
#endif
#ifdef V4L2_CAP_VIDEO_CAPTURE_MPLANE
  case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
    return "VIDEO_CAPTURE_MPLANE";
#endif
  default:
    return "UNKNOWN";
  }
}

bool DebugV4l2Fps() {
  static const bool enabled =
      (std::getenv("STUDIOCAST_DEBUG_V4L2_FPS") != nullptr);
  return enabled;
}

void V4l2FpsDbg(const std::string &msg) {
  if (!DebugV4l2Fps())
    return;
  std::fprintf(stderr, "[v4l2_fps] %s\n", msg.c_str());
}

struct FpsDecision {
  int fps = 0;     // frames per second
  int tpf_num = 0; // time-per-frame numerator (seconds)
  int tpf_den = 0; // time-per-frame denominator (seconds)
  bool used_fps_fraction = false;
  double fps_from_tpf = 0.0;
  double fps_from_fps_fraction = 0.0;
};

// Some devices (notably certain v4l2loopback configurations) report the V4L2
// timeperframe fraction inverted (i.e. as FPS rather than seconds-per-frame).
// Heuristically choose the interpretation that yields a plausible FPS and is
// closest to the desired value.
FpsDecision DecideFpsFromFrac(int desired_fps, int num, int den) {
  FpsDecision d;
  if (num <= 0 || den <= 0)
    return d;

  const double fps_tpf =
      static_cast<double>(den) / static_cast<double>(num); // spec: 1/fps
  const double fps_frac =
      static_cast<double>(num) / static_cast<double>(den); // inverted

  auto in_range = [](double f) { return f >= 1.0 && f <= 240.0; };

  const bool tpf_ok = in_range(fps_tpf);
  const bool frac_ok = in_range(fps_frac);

  double chosen = 0.0;
  bool use_frac = false;

  if (tpf_ok && !frac_ok) {
    chosen = fps_tpf;
  } else if (!tpf_ok && frac_ok) {
    chosen = fps_frac;
    use_frac = true;
  } else if (tpf_ok && frac_ok) {
    if (desired_fps > 0) {
      const double dt = std::fabs(fps_tpf - static_cast<double>(desired_fps));
      const double df = std::fabs(fps_frac - static_cast<double>(desired_fps));
      if (df + 0.01 < dt) {
        chosen = fps_frac;
        use_frac = true;
      } else {
        chosen = fps_tpf;
      }
    } else {
      chosen = fps_tpf;
    }
  } else {
    chosen = (desired_fps > 0) ? static_cast<double>(desired_fps) : 0.0;
  }

  int fps_i = (chosen > 0.0) ? static_cast<int>(std::floor(chosen + 0.5)) : 0;
  if (fps_i < 1)
    fps_i = 0;
  if (fps_i > 240)
    fps_i = 240;

  d.fps = fps_i;
  if (fps_i > 0) {
    // Canonicalize to time-per-frame (seconds): 1/fps.
    d.tpf_num = 1;
    d.tpf_den = fps_i;
  }
  d.used_fps_fraction = use_frac;
  d.fps_from_tpf = fps_tpf;
  d.fps_from_fps_fraction = fps_frac;
  return d;
}

struct TypeSpec {
  __u32 type = 0;
  bool mplane = false;
};

std::string CapsToString(__u32 caps) {
  std::ostringstream oss;
  oss << "0x" << std::hex << caps << std::dec << " (";
  bool any = false;
  auto add = [&](const char *s) {
    if (any)
      oss << " ";
    oss << s;
    any = true;
  };

  if (caps & V4L2_CAP_VIDEO_CAPTURE)
    add("CAPTURE");
#ifdef V4L2_CAP_VIDEO_CAPTURE_MPLANE
  if (caps & V4L2_CAP_VIDEO_CAPTURE_MPLANE)
    add("CAPTURE_MPLANE");
#endif
  if (caps & V4L2_CAP_VIDEO_OUTPUT)
    add("OUTPUT");
#ifdef V4L2_CAP_VIDEO_OUTPUT_MPLANE
  if (caps & V4L2_CAP_VIDEO_OUTPUT_MPLANE)
    add("OUTPUT_MPLANE");
#endif
  if (caps & V4L2_CAP_READWRITE)
    add("READWRITE");
  if (caps & V4L2_CAP_STREAMING)
    add("STREAMING");
  if (!any)
    oss << "none";
  oss << ")";
  return oss.str();
}

bool TrySetFmtSinglePlane(int fd, __u32 bufType, int width, int height,
                          PixelFormat desired, bool setStrideAndSize,
                          v4l2_format *outFmt, std::string *outErr) {
  v4l2_format f{};
  f.type = bufType;
  f.fmt.pix.width = static_cast<__u32>(width);
  f.fmt.pix.height = static_cast<__u32>(height);
  f.fmt.pix.pixelformat = FourccFor(desired);
  f.fmt.pix.field = V4L2_FIELD_ANY; // more permissive than NONE

  if (setStrideAndSize) {
    const std::size_t bpl = MinBytesPerLine(width, desired);
    f.fmt.pix.bytesperline = static_cast<__u32>(bpl);
    f.fmt.pix.sizeimage =
        static_cast<__u32>(bpl * static_cast<std::size_t>(height));
  } else {
    f.fmt.pix.bytesperline = 0;
    f.fmt.pix.sizeimage = 0;
  }

  if (IoctlRetry(fd, VIDIOC_S_FMT, &f) == 0) {
    if (outFmt)
      *outFmt = f;
    return true;
  }

  if (outErr) {
    std::ostringstream oss;
    oss << "VIDIOC_S_FMT(" << BufTypeName(bufType)
        << ") failed: " << std::strerror(errno);
    *outErr = oss.str();
  }
  return false;
}

bool TryGetFmtSinglePlane(int fd, __u32 bufType, v4l2_format *outFmt,
                          std::string *outErr) {
  v4l2_format f{};
  f.type = bufType;

  if (IoctlRetry(fd, VIDIOC_G_FMT, &f) == 0) {
    if (outFmt)
      *outFmt = f;
    return true;
  }

  if (outErr) {
    std::ostringstream oss;
    oss << "VIDIOC_G_FMT(" << BufTypeName(bufType)
        << ") failed: " << std::strerror(errno);
    *outErr = oss.str();
  }
  return false;
}

#if STUDIOCAST_V4L2_HAS_MPLANE
bool TrySetFmtMPlane(int fd, __u32 bufType, int width, int height,
                     PixelFormat desired, bool setStrideAndSize,
                     v4l2_format *outFmt, std::string *outErr) {
  v4l2_format f{};
  f.type = bufType;
  f.fmt.pix_mp.width = static_cast<__u32>(width);
  f.fmt.pix_mp.height = static_cast<__u32>(height);
  f.fmt.pix_mp.pixelformat = FourccFor(desired);
  f.fmt.pix_mp.field = V4L2_FIELD_ANY;
  f.fmt.pix_mp.num_planes = 1;

  if (setStrideAndSize) {
    const std::size_t bpl = MinBytesPerLine(width, desired);
    f.fmt.pix_mp.plane_fmt[0].bytesperline = static_cast<__u32>(bpl);
    f.fmt.pix_mp.plane_fmt[0].sizeimage =
        static_cast<__u32>(bpl * static_cast<std::size_t>(height));
  } else {
    f.fmt.pix_mp.plane_fmt[0].bytesperline = 0;
    f.fmt.pix_mp.plane_fmt[0].sizeimage = 0;
  }

  if (IoctlRetry(fd, VIDIOC_S_FMT, &f) == 0) {
    if (outFmt)
      *outFmt = f;
    return true;
  }

  if (outErr) {
    std::ostringstream oss;
    oss << "VIDIOC_S_FMT(" << BufTypeName(bufType)
        << ") failed: " << std::strerror(errno);
    *outErr = oss.str();
  }
  return false;
}

bool TryGetFmtMPlane(int fd, __u32 bufType, v4l2_format *outFmt,
                     std::string *outErr) {
  v4l2_format f{};
  f.type = bufType;

  if (IoctlRetry(fd, VIDIOC_G_FMT, &f) == 0) {
    if (outFmt)
      *outFmt = f;
    return true;
  }

  if (outErr) {
    std::ostringstream oss;
    oss << "VIDIOC_G_FMT(" << BufTypeName(bufType)
        << ") failed: " << std::strerror(errno);
    *outErr = oss.str();
  }
  return false;
}
#endif

bool TrySetFmtAny(int fd, const TypeSpec &t, int width, int height,
                  PixelFormat desired, bool setStrideAndSize,
                  v4l2_format *outFmt, std::string *outErr) {
  if (!t.mplane) {
    return TrySetFmtSinglePlane(fd, t.type, width, height, desired,
                                setStrideAndSize, outFmt, outErr);
  }
#if STUDIOCAST_V4L2_HAS_MPLANE
  return TrySetFmtMPlane(fd, t.type, width, height, desired, setStrideAndSize,
                         outFmt, outErr);
#else
  if (outErr)
    *outErr = "mplane types not supported by headers";
  return false;
#endif
}

bool TryGetFmtAny(int fd, const TypeSpec &t, v4l2_format *outFmt,
                  std::string *outErr) {
  if (!t.mplane) {
    return TryGetFmtSinglePlane(fd, t.type, outFmt, outErr);
  }
#if STUDIOCAST_V4L2_HAS_MPLANE
  return TryGetFmtMPlane(fd, t.type, outFmt, outErr);
#else
  if (outErr)
    *outErr = "mplane types not supported by headers";
  return false;
#endif
}

} // namespace

bool ParseChosenOutputFmt(const v4l2_format &f, bool mplane, int fps,
                          ActualFormat *out, std::string *outErr) {
  if (!out)
    return false;

  int w = 0, h = 0;
  std::uint32_t fourcc = 0;
  std::size_t bpl = 0;
  std::size_t size = 0;

  if (!mplane) {
    w = static_cast<int>(f.fmt.pix.width);
    h = static_cast<int>(f.fmt.pix.height);
    fourcc = f.fmt.pix.pixelformat;
    bpl = static_cast<std::size_t>(f.fmt.pix.bytesperline);
    size = static_cast<std::size_t>(f.fmt.pix.sizeimage);
  } else {
#if STUDIOCAST_V4L2_HAS_MPLANE
    w = static_cast<int>(f.fmt.pix_mp.width);
    h = static_cast<int>(f.fmt.pix_mp.height);
    fourcc = f.fmt.pix_mp.pixelformat;
    // The writer gives the driver a frame with write(), and the kernel
    // refuses that I/O method on a buffer of more than one plane:
    // `__vb2_init_fileio` answers -EBUSY when `vb->num_planes != 1`. A report
    // of no planes has nothing to read at all, because the arm below reads
    // `plane_fmt[0]` alone. Name the count here: without this the open
    // succeeds and every write() fails with "Device or resource busy", which
    // says nothing about the layout that caused it.
    if (f.fmt.pix_mp.num_planes != 1) {
      if (outErr)
        *outErr =
            "mplane format returned num_planes=" +
            std::to_string(static_cast<unsigned>(f.fmt.pix_mp.num_planes)) +
            ", only one plane is supported";
      return false;
    }
    bpl = static_cast<std::size_t>(f.fmt.pix_mp.plane_fmt[0].bytesperline);
    size = static_cast<std::size_t>(f.fmt.pix_mp.plane_fmt[0].sizeimage);
#else
    if (outErr)
      *outErr = "mplane not supported";
    return false;
#endif
  }

  const auto pf = PixelFormatFromFourcc(fourcc);
  if (!pf) {
    if (outErr) {
      *outErr = "Device negotiated unsupported pixel format '" +
                FourccToString(fourcc) + "'. Supported: YUYV, RGB24.";
    }
    return false;
  }

  ActualFormat a;
  a.width = w;
  a.height = h;
  a.fps = fps;
  a.fps_num = 1;
  a.fps_den = fps;
  a.format = *pf;
  a.pixfmt_fourcc = fourcc;
  a.pixfmt = FourccToString(fourcc);

  const std::size_t rows =
      a.height > 0 ? static_cast<std::size_t>(a.height) : 0u;

  // Only a row too short to hold the pixels is raised, because the converter
  // writes the whole packed row and the driver cannot have meant less.
  const std::size_t driverBpl = bpl;
  const std::size_t minBpl = MinBytesPerLine(a.width, a.format);
  if (bpl < minBpl)
    bpl = minBpl;
  a.bytes_per_line = bpl;

  // The rows the writer walks must fit the frame the driver sized.
  // `WriteFrame` sizes every write() from `size_image`, so raising that value
  // to match a longer row only hides the disagreement, and the writer then
  // pushes more bytes than the frame the driver sized. On a v4l2loopback
  // output the surplus runs into the next frame, and the picture is torn from
  // then on.
  //
  // The measure comes after the raise, because the raised row is the row the
  // writer walks. A stride below the packed row is itself a contradiction in
  // the report of a packed format, thus the frame that stride implies gets no
  // more trust than a stride the driver padded.
  //
  // A frame size of 0 is not that disagreement: it is no report at all, and
  // the raise below gives it the value the row implies.
  const std::size_t minSize = bpl * rows;
  if (size > 0 && minSize > size) {
    if (outErr)
      *outErr = "Driver reported bytesperline=" + std::to_string(driverBpl) +
                " and sizeimage=" + std::to_string(size) + ", but " +
                std::to_string(a.height) + " rows of " + std::to_string(bpl) +
                " bytes do not fit";
    return false;
  }

  if (size < minSize)
    size = minSize;
  a.size_image = size;

  *out = a;
  return true;
}

namespace {

struct NegotiationResult {
  bool ok = false;
  ActualFormat actual{};
  __u32 chosen_buf_type = 0;
  bool chosen_mplane = false;
  std::string error;
};

NegotiationResult NegotiateFormat(int fd, const std::string &device, int width,
                                  int height, int fps, PixelFormat desiredFmt) {
  NegotiationResult res;

  v4l2_capability cap{};
  if (IoctlRetry(fd, VIDIOC_QUERYCAP, &cap) != 0) {
    res.error = "VIDIOC_QUERYCAP failed: " + std::string(std::strerror(errno));
    return res;
  }

  __u32 caps = cap.capabilities;
  if (caps & V4L2_CAP_DEVICE_CAPS)
    caps = cap.device_caps;

  // Prefer output types first; then capture types. Try mplane variants too.
  const TypeSpec types[] = {
      {V4L2_BUF_TYPE_VIDEO_OUTPUT, false},
#ifdef V4L2_CAP_VIDEO_OUTPUT_MPLANE
      {V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, true},
#endif
      {V4L2_BUF_TYPE_VIDEO_CAPTURE, false},
#ifdef V4L2_CAP_VIDEO_CAPTURE_MPLANE
      {V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, true},
#endif
  };

  // Try S_FMT (stride then no-stride) across types
  std::ostringstream attemptLog;
  v4l2_format chosen{};
  bool chosenMplane = false;
  bool ok = false;

  for (const auto &t : types) {
    std::string e1, e2;
    v4l2_format f{};
    if (TrySetFmtAny(fd, t, width, height, desiredFmt, true, &f, &e1) ||
        TrySetFmtAny(fd, t, width, height, desiredFmt, false, &f, &e2)) {
      chosen = f;
      chosenMplane = t.mplane;
      res.chosen_buf_type = t.type;
      res.chosen_mplane = t.mplane;
      ok = true;
      break;
    }

    attemptLog << "Tried " << BufTypeName(t.type)
               << " S_FMT(with stride): " << (e1.empty() ? "(no detail)" : e1)
               << "\n";
    attemptLog << "Tried " << BufTypeName(t.type)
               << " S_FMT(no stride):   " << (e2.empty() ? "(no detail)" : e2)
               << "\n";
  }

  // If set failed everywhere, try G_FMT
  if (!ok) {
    for (const auto &t : types) {
      std::string ge;
      v4l2_format f{};
      if (TryGetFmtAny(fd, t, &f, &ge)) {
        chosen = f;
        chosenMplane = t.mplane;
        res.chosen_buf_type = t.type;
        res.chosen_mplane = t.mplane;
        ok = true;
        break;
      }
      attemptLog << "Tried " << BufTypeName(t.type)
                 << " G_FMT: " << (ge.empty() ? "(no detail)" : ge) << "\n";
    }
  }

  if (!ok) {
    std::ostringstream oss;
    oss << "Failed to set/query format for " << device
        << " (desired=" << PixelFormatName(desiredFmt) << ", " << width << "x"
        << height << ")\n"
        << "querycap.driver=" << cap.driver << " card=" << cap.card
        << " bus=" << cap.bus_info << "\n"
        << "querycap.caps=" << CapsToString(caps) << "\n"
        << attemptLog.str();
    res.error = oss.str();
    return res;
  }

  // Best-effort FPS set/query.
  //
  // V4L2 specifies time-per-frame (numerator/denominator) in seconds, but some
  // devices report it inverted (as FPS). Try the canonical 1/fps first, then
  // fall back to fps/1 if the driver appears to interpret the fraction as FPS.
  auto SetParm = [&](int num, int den) {
    v4l2_streamparm sp{};
    sp.type = (res.chosen_buf_type != 0)
                  ? res.chosen_buf_type
                  : static_cast<__u32>(V4L2_BUF_TYPE_VIDEO_OUTPUT);
    if (IsOutputBufType(sp.type)) {
      sp.parm.output.timeperframe.numerator = static_cast<__u32>(num);
      sp.parm.output.timeperframe.denominator = static_cast<__u32>(den);
    } else {
      sp.parm.capture.timeperframe.numerator = static_cast<__u32>(num);
      sp.parm.capture.timeperframe.denominator = static_cast<__u32>(den);
    }
    return IoctlRetry(fd, VIDIOC_S_PARM, &sp) == 0;
  };

  auto GetParm = [&](int *outNum, int *outDen, __u32 *outType) -> bool {
    v4l2_streamparm gp{};
    gp.type = (res.chosen_buf_type != 0)
                  ? res.chosen_buf_type
                  : static_cast<__u32>(V4L2_BUF_TYPE_VIDEO_OUTPUT);
    if (IoctlRetry(fd, VIDIOC_G_PARM, &gp) != 0)
      return false;
    const v4l2_fract tpf = IsOutputBufType(gp.type)
                               ? gp.parm.output.timeperframe
                               : gp.parm.capture.timeperframe;
    if (outNum)
      *outNum = static_cast<int>(tpf.numerator);
    if (outDen)
      *outDen = static_cast<int>(tpf.denominator);
    if (outType)
      *outType = gp.type;
    return true;
  };

  int negotiatedFps = fps;
  int fpsNum = 1;
  int fpsDen = fps;

  // Try canonical 1/fps.
  (void)SetParm(1, fps);

  int qnum = 0, qden = 0;
  __u32 qtype = 0;
  FpsDecision best{};
  bool haveBest = false;

  if (GetParm(&qnum, &qden, &qtype)) {
    best = DecideFpsFromFrac(fps, qnum, qden);
    if (best.fps > 0) {
      negotiatedFps = best.fps;
      fpsNum = best.tpf_num;
      fpsDen = best.tpf_den;
      haveBest = true;
    }

    if (DebugV4l2Fps()) {
      std::ostringstream oss;
      oss << "writer " << device << " G_PARM(type=" << BufTypeName(qtype) << ")"
          << " desired=" << fps << " raw=" << qnum << "/" << qden
          << " fps(tpf)=" << best.fps_from_tpf
          << " fps(inv)=" << best.fps_from_fps_fraction
          << " chosen=" << negotiatedFps
          << (best.used_fps_fraction ? " (inv)" : " (tpf)");
      V4l2FpsDbg(oss.str());
    }

    // If the device appears to be using the inverted convention, try setting
    // fps/1.
    if (best.used_fps_fraction) {
      (void)SetParm(fps, 1);

      int qnum2 = 0, qden2 = 0;
      __u32 qtype2 = 0;
      if (GetParm(&qnum2, &qden2, &qtype2)) {
        const auto d2 = DecideFpsFromFrac(fps, qnum2, qden2);

        auto diff = [&](const FpsDecision &d) -> int {
          if (fps <= 0 || d.fps <= 0)
            return 9999;
          const int a = d.fps - fps;
          return (a < 0) ? -a : a;
        };

        if (!haveBest || diff(d2) < diff(best)) {
          best = d2;
          if (best.fps > 0)
            negotiatedFps = best.fps;
          if (best.tpf_num > 0)
            fpsNum = best.tpf_num;
          if (best.tpf_den > 0)
            fpsDen = best.tpf_den;
        }

        if (DebugV4l2Fps()) {
          std::ostringstream oss;
          oss << "writer " << device
              << " G_PARM(after alt set, type=" << BufTypeName(qtype2) << ")"
              << " raw=" << qnum2 << "/" << qden2
              << " fps(tpf)=" << d2.fps_from_tpf
              << " fps(inv)=" << d2.fps_from_fps_fraction
              << " chosen=" << negotiatedFps
              << (best.used_fps_fraction ? " (inv)" : " (tpf)");
          V4l2FpsDbg(oss.str());
        }
      }
    }
  }

  std::string perr;
  ActualFormat a;
  if (!ParseChosenOutputFmt(chosen, chosenMplane, negotiatedFps, &a, &perr)) {
    res.error = "Format negotiation succeeded but parsing failed: " + perr;
    return res;
  }

  a.fps_num = fpsNum;
  a.fps_den = fpsDen;

  res.ok = true;
  res.actual = a;
  return res;
}

bool TryOpenNegotiate(const std::string &device, int openFlags, int width,
                      int height, int fps, PixelFormat fmt, int *outFd,
                      ActualFormat *outActual, std::string *outErr) {
  const int fd = ::open(device.c_str(), openFlags | O_CLOEXEC);
  if (fd < 0) {
    if (outErr)
      *outErr = "open() failed: " + std::string(std::strerror(errno));
    return false;
  }

  auto neg = NegotiateFormat(fd, device, width, height, fps, fmt);
  if (!neg.ok) {
    if (outErr)
      *outErr = neg.error;
    ::close(fd);
    return false;
  }

  if (outFd)
    *outFd = fd;
  if (outActual)
    *outActual = neg.actual;
  return true;
}

} // namespace

std::size_t MinBytesPerLine(int width, PixelFormat fmt) {
  if (width <= 0)
    return 0u;
  const auto w = static_cast<std::size_t>(width);
  switch (fmt) {
  case PixelFormat::yuyv:
    // Round the width up to the next pixel pair: the last pair is written
    // whole even when the width is odd.
    return ((w + 1u) / 2u) * 4u;
  case PixelFormat::rgb24:
    return w * 3u;
  }
  return ((w + 1u) / 2u) * 4u;
}

std::string PixelFormatName(PixelFormat fmt) {
  switch (fmt) {
  case PixelFormat::yuyv:
    return "yuyv";
  case PixelFormat::rgb24:
    return "rgb24";
  }
  return "yuyv";
}

std::optional<PixelFormat> ParsePixelFormat(const std::string &s) {
  const auto t = ToLowerAscii(s);
  if (t == "yuyv" || t == "yuy2")
    return PixelFormat::yuyv;
  if (t == "rgb24" || t == "rgb")
    return PixelFormat::rgb24;
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

bool V4l2Writer::Open(const std::string &device, int width, int height, int fps,
                      PixelFormat fmt, std::string *error) {
  Close();

  if (device.empty()) {
    if (error)
      *error = "Device path is empty.";
    return false;
  }
  if (width <= 0 || height <= 0) {
    if (error)
      *error = "Invalid width/height.";
    return false;
  }
  if (fps <= 0 || fps > 240) {
    if (error)
      *error = "Invalid fps (1..240).";
    return false;
  }

  int fd = -1;
  ActualFormat a{};
  std::string err1, err2;

  // Prefer O_WRONLY (important for exclusive_caps setups and avoids grabbing
  // the read side).
  if (TryOpenNegotiate(device, O_WRONLY, width, height, fps, fmt, &fd, &a,
                       &err1)) {
    fd_ = fd;
    actual_ = a;
    return true;
  }

  // Fallback: try O_RDWR (some implementations/drivers only accept this).
  if (TryOpenNegotiate(device, O_RDWR, width, height, fps, fmt, &fd, &a,
                       &err2)) {
    fd_ = fd;
    actual_ = a;
    return true;
  }

  if (error) {
    std::ostringstream oss;
    oss << "Failed to open/negotiate V4L2 format for " << device
        << " (desired=" << PixelFormatName(fmt) << ", " << width << "x"
        << height << ")\n\n"
        << "Attempt 1 (O_WRONLY):\n"
        << (err1.empty() ? "(no detail)" : err1) << "\n\n"
        << "Attempt 2 (O_RDWR):\n"
        << (err2.empty() ? "(no detail)" : err2);
    *error = oss.str();
  }
  return false;
}

bool V4l2Writer::RefreshActual(std::string *error) {
  if (fd_ < 0) {
    if (error)
      *error = "Writer not open.";
    return false;
  }

  // Prefer output types, but allow capture types as a fallback. Some devices
  // expose both and/or allow querying only one side.
  const TypeSpec types[] = {
      {V4L2_BUF_TYPE_VIDEO_OUTPUT, false},
#ifdef V4L2_CAP_VIDEO_OUTPUT_MPLANE
      {V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, true},
#endif
      {V4L2_BUF_TYPE_VIDEO_CAPTURE, false},
#ifdef V4L2_CAP_VIDEO_CAPTURE_MPLANE
      {V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, true},
#endif
  };

  v4l2_format chosen{};
  bool chosenMplane = false;
  __u32 chosenType = 0;
  bool ok = false;

  std::ostringstream attempts;
  for (const auto &t : types) {
    std::string ge;
    v4l2_format f{};
    if (TryGetFmtAny(fd_, t, &f, &ge)) {
      chosen = f;
      chosenMplane = t.mplane;
      chosenType = t.type;
      ok = true;
      break;
    }
    attempts << "Tried " << BufTypeName(t.type)
             << " G_FMT: " << (ge.empty() ? "(no detail)" : ge) << "\n";
  }

  if (!ok) {
    if (error) {
      *error = "Failed to query active V4L2 format (VIDIOC_G_FMT).\n" +
               attempts.str();
    }
    return false;
  }

  const int desiredFps = (actual_.fps > 0) ? actual_.fps : 30;
  int negotiatedFps = desiredFps;
  int fpsNum = (actual_.fps_num > 0) ? actual_.fps_num : 1;
  int fpsDen = (actual_.fps_den > 0) ? actual_.fps_den : desiredFps;

  // Best-effort fps query.
  v4l2_streamparm gp{};
  gp.type = (chosenType != 0) ? chosenType
                              : static_cast<__u32>(V4L2_BUF_TYPE_VIDEO_OUTPUT);
  if (IoctlRetry(fd_, VIDIOC_G_PARM, &gp) == 0) {
    const v4l2_fract tpf = IsOutputBufType(gp.type)
                               ? gp.parm.output.timeperframe
                               : gp.parm.capture.timeperframe;
    const int num = static_cast<int>(tpf.numerator);
    const int den = static_cast<int>(tpf.denominator);
    if (num > 0 && den > 0) {
      const auto d = DecideFpsFromFrac(desiredFps, num, den);
      if (d.fps > 0) {
        negotiatedFps = d.fps;
        fpsNum = d.tpf_num;
        fpsDen = d.tpf_den;
      }

      if (DebugV4l2Fps()) {
        std::ostringstream oss;
        oss << "writer RefreshActual G_PARM(type=" << BufTypeName(gp.type)
            << ")"
            << " desired=" << desiredFps << " raw=" << num << "/" << den
            << " fps(tpf)=" << d.fps_from_tpf
            << " fps(inv)=" << d.fps_from_fps_fraction
            << " chosen=" << negotiatedFps
            << (d.used_fps_fraction ? " (inv)" : " (tpf)");
        V4l2FpsDbg(oss.str());
      }
    }
  }

  ActualFormat a;
  std::string perr;
  if (!ParseChosenOutputFmt(chosen, chosenMplane, negotiatedFps, &a, &perr)) {
    if (error)
      *error = "Queried format parsing failed: " + perr;
    return false;
  }

  // Some v4l2loopback configurations transiently report a "blank" format
  // (e.g. width/height=0) during consumer disconnect / renegotiation windows.
  // Treat that as a failed refresh so we don't overwrite a previously-valid
  // cached format and trigger output renegotiation thrash.
  if (a.width <= 0 || a.height <= 0) {
    if (error) {
      std::ostringstream oss;
      oss << "Queried format invalid: " << a.width << "x" << a.height
          << " pixfmt=" << a.pixfmt;
      *error = oss.str();
    }
    return false;
  }

  a.fps_num = fpsNum;
  a.fps_den = fpsDen;
  actual_ = a;
  return true;
}

bool V4l2Writer::Renegotiate(const std::string &device, int width, int height,
                             int fps, PixelFormat fmt, std::string *error) {
  if (fd_ < 0) {
    if (error)
      *error = "Writer not open.";
    return false;
  }
  if (device.empty()) {
    if (error)
      *error = "Device path is empty.";
    return false;
  }
  if (width <= 0 || height <= 0) {
    if (error)
      *error = "Invalid width/height.";
    return false;
  }
  if (fps <= 0 || fps > 240) {
    if (error)
      *error = "Invalid fps (1..240).";
    return false;
  }

  auto neg = NegotiateFormat(fd_, device, width, height, fps, fmt);
  if (!neg.ok) {
    if (error)
      *error = neg.error;
    return false;
  }

  actual_ = neg.actual;
  return true;
}

bool V4l2Writer::WriteFrame(const std::uint8_t *data, std::size_t bytes,
                            std::string *error) {
  if (fd_ < 0) {
    if (error)
      *error = "Writer not open.";
    return false;
  }
  if (!data) {
    if (error)
      *error = "Frame data is null.";
    return false;
  }
  if (bytes < actual_.size_image) {
    if (error)
      *error = "Frame buffer too small for size_image.";
    return false;
  }

  const std::size_t toWrite = actual_.size_image;
  std::size_t offset = 0;

  while (offset < toWrite) {
    const std::size_t chunk = toWrite - offset;
    const ssize_t wrote = ::write(fd_, data + offset, chunk);
    if (wrote < 0) {
      if (errno == EINTR)
        continue;
      if (error)
        *error = std::string("write() failed: ") + std::strerror(errno);
      return false;
    }
    if (wrote == 0) {
      if (error)
        *error = "write() returned 0 (unexpected).";
      return false;
    }
    offset += static_cast<std::size_t>(wrote);
  }

  return true;
}

} // namespace studiocast::video
