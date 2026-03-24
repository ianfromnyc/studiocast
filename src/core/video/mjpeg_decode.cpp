#include "core/video/mjpeg_decode.h"

#include <algorithm>
#include <csetjmp>
#include <sstream>
#include <string>

#include <jpeglib.h>

namespace studiocast::video {
namespace {

struct JpegErr {
  jpeg_error_mgr pub;
  jmp_buf jmp;
  char msg[JMSG_LENGTH_MAX]{};
};

void JpegErrorExit(j_common_ptr cinfo) {
  auto *err = reinterpret_cast<JpegErr *>(cinfo->err);
  (*cinfo->err->format_message)(cinfo, err->msg);
  longjmp(err->jmp, 1);
}

void JpegEmitMessage(j_common_ptr /*cinfo*/, int /*msg_level*/) {
  // Silence libjpeg warnings/trace messages (stderr) to keep logs and
  // self-tests deterministic.
}

void JpegOutputMessage(j_common_ptr /*cinfo*/) {
  // Silence libjpeg formatted output (stderr).
}

} // namespace

void Rgb24Frame::ResizeTight(int w, int h) {
  width = w;
  height = h;
  stride_bytes = (w > 0) ? (static_cast<std::size_t>(w) * 3u) : 0u;
  const std::size_t total =
      stride_bytes * static_cast<std::size_t>(std::max(0, h));
  buf.resize(total);
}

bool DecodeMjpegToRgb24(const std::uint8_t *mjpg, std::size_t len,
                        Rgb24Frame &out, int &w, int &h, std::string *error) {
  if (error)
    error->clear();
  w = 0;
  h = 0;
  if (!mjpg || len == 0) {
    if (error)
      *error = "Empty MJPEG buffer.";
    return false;
  }

  jpeg_decompress_struct cinfo{};
  JpegErr jerr{};

  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = JpegErrorExit;
  jerr.pub.emit_message = JpegEmitMessage;
  jerr.pub.output_message = JpegOutputMessage;

  if (setjmp(jerr.jmp) != 0) {
    if (error)
      *error = jerr.msg;
    jpeg_destroy_decompress(&cinfo);
    return false;
  }

  jpeg_create_decompress(&cinfo);
  jpeg_mem_src(&cinfo,
               const_cast<unsigned char *>(
                   reinterpret_cast<const unsigned char *>(mjpg)),
               static_cast<unsigned long>(len));

  if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
    if (error)
      *error = "jpeg_read_header failed.";
    jpeg_destroy_decompress(&cinfo);
    return false;
  }

  cinfo.out_color_space = JCS_RGB;

  if (jpeg_start_decompress(&cinfo) != TRUE) {
    if (error)
      *error = "jpeg_start_decompress failed.";
    jpeg_destroy_decompress(&cinfo);
    return false;
  }

  w = static_cast<int>(cinfo.output_width);
  h = static_cast<int>(cinfo.output_height);
  const int comps = static_cast<int>(cinfo.output_components);
  if (w <= 0 || h <= 0 || comps != 3) {
    if (error) {
      std::ostringstream oss;
      oss << "Unexpected decompressor output (w=" << w << ", h=" << h
          << ", comps=" << comps << ").";
      *error = oss.str();
    }
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    w = 0;
    h = 0;
    return false;
  }

  out.ResizeTight(w, h);
  const std::size_t row_stride = static_cast<std::size_t>(w) * 3u;

  while (cinfo.output_scanline < cinfo.output_height) {
    const std::size_t y = static_cast<std::size_t>(cinfo.output_scanline);
    JSAMPROW row[1];
    row[0] = reinterpret_cast<JSAMPROW>(out.data() + y * row_stride);
    const JDIMENSION n = jpeg_read_scanlines(&cinfo, row, 1);
    if (n != 1) {
      if (error)
        *error = "jpeg_read_scanlines failed.";
      jpeg_finish_decompress(&cinfo);
      jpeg_destroy_decompress(&cinfo);
      w = 0;
      h = 0;
      return false;
    }
  }

  jpeg_finish_decompress(&cinfo);
  jpeg_destroy_decompress(&cinfo);
  return true;
}

} // namespace studiocast::video
