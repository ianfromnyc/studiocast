#include "core/video/mjpeg_decode.h"

#include <algorithm>
#include <csetjmp>

#include <jpeglib.h>

namespace studiocast::video {
namespace {

struct JpegErr {
  jpeg_error_mgr pub;
  jmp_buf jmp;
  char msg[JMSG_LENGTH_MAX]{};
};

void JpegErrorExit(j_common_ptr cinfo) {
  auto* err = reinterpret_cast<JpegErr*>(cinfo->err);
  (*cinfo->err->format_message)(cinfo, err->msg);
  longjmp(err->jmp, 1);
}

}  // namespace

void Rgb24Frame::ResizeTight(int w, int h) {
  width = w;
  height = h;
  stride_bytes = (w > 0) ? (static_cast<std::size_t>(w) * 3u) : 0u;
  const std::size_t total = stride_bytes * static_cast<std::size_t>(std::max(0, h));
  buf.resize(total);
}

bool DecodeMjpegToRgb24(const std::uint8_t* mjpg, std::size_t len, Rgb24Frame& out, int& w, int& h) {
  w = 0;
  h = 0;
  if (!mjpg || len == 0) return false;

  jpeg_decompress_struct cinfo{};
  JpegErr jerr{};

  cinfo.err = jpeg_std_error(&jerr.pub);
  jerr.pub.error_exit = JpegErrorExit;

  if (setjmp(jerr.jmp) != 0) {
    jpeg_destroy_decompress(&cinfo);
    return false;
  }

  jpeg_create_decompress(&cinfo);
  jpeg_mem_src(&cinfo, const_cast<unsigned char*>(reinterpret_cast<const unsigned char*>(mjpg)),
               static_cast<unsigned long>(len));

  if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
    jpeg_destroy_decompress(&cinfo);
    return false;
  }

  cinfo.out_color_space = JCS_RGB;

  if (jpeg_start_decompress(&cinfo) != TRUE) {
    jpeg_destroy_decompress(&cinfo);
    return false;
  }

  w = static_cast<int>(cinfo.output_width);
  h = static_cast<int>(cinfo.output_height);
  const int comps = static_cast<int>(cinfo.output_components);
  if (w <= 0 || h <= 0 || comps != 3) {
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

}  // namespace studiocast::video
