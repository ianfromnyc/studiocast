#include "core/video/image_ppm.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <sstream>

namespace studiocast::video {
namespace {

// Read the next token in a PPM header, skipping whitespace and comments.
bool ReadPpmToken(std::istream& in, std::string* out) {
  if (!out) return false;
  out->clear();

  while (true) {
    int c = in.peek();
    if (c == EOF) return false;

    // whitespace
    if (std::isspace(static_cast<unsigned char>(c))) {
      (void)in.get();
      continue;
    }

    // comment
    if (c == '#') {
      std::string dummy;
      std::getline(in, dummy);
      continue;
    }

    break;
  }

  std::string tok;
  while (true) {
    int c = in.peek();
    if (c == EOF) break;
    if (std::isspace(static_cast<unsigned char>(c)) || c == '#') break;
    tok.push_back(static_cast<char>(in.get()));
  }
  if (tok.empty()) return false;
  *out = tok;
  return true;
}

int ClampInt(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }

}  // namespace

bool LoadPpmP6Rgb24(const std::filesystem::path& path,
                    int* out_w,
                    int* out_h,
                    std::vector<std::uint8_t>* out_rgb,
                    std::string* error) {
  if (out_w) *out_w = 0;
  if (out_h) *out_h = 0;
  if (out_rgb) out_rgb->clear();

  std::ifstream f(path, std::ios::binary);
  if (!f) {
    if (error) *error = "Failed to open image: " + path.string();
    return false;
  }

  std::string magic;
  if (!ReadPpmToken(f, &magic) || magic != "P6") {
    if (error) *error = "Unsupported image format (expected PPM P6): " + path.string();
    return false;
  }

  std::string tok;
  if (!ReadPpmToken(f, &tok)) {
    if (error) *error = "Invalid PPM header (missing width)";
    return false;
  }
  const int w = std::atoi(tok.c_str());
  if (!ReadPpmToken(f, &tok)) {
    if (error) *error = "Invalid PPM header (missing height)";
    return false;
  }
  const int h = std::atoi(tok.c_str());
  if (!ReadPpmToken(f, &tok)) {
    if (error) *error = "Invalid PPM header (missing maxval)";
    return false;
  }
  const int maxval = std::atoi(tok.c_str());

  if (w <= 0 || h <= 0 || w > 16384 || h > 16384) {
    if (error) *error = "Invalid PPM dimensions";
    return false;
  }
  if (maxval <= 0 || maxval > 255) {
    if (error) *error = "Unsupported PPM maxval (must be <= 255)";
    return false;
  }

  // Consume single whitespace after maxval if present.
  int c = f.peek();
  if (c != EOF && std::isspace(static_cast<unsigned char>(c))) (void)f.get();

  const std::size_t sz = static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 3u;
  std::vector<std::uint8_t> rgb(sz);
  f.read(reinterpret_cast<char*>(rgb.data()), static_cast<std::streamsize>(sz));
  if (f.gcount() != static_cast<std::streamsize>(sz)) {
    if (error) *error = "PPM truncated (not enough pixel data)";
    return false;
  }

  if (out_w) *out_w = w;
  if (out_h) *out_h = h;
  if (out_rgb) *out_rgb = std::move(rgb);
  return true;
}

bool ResizeRgb24Bilinear(const std::uint8_t* src_rgb,
                         int src_w,
                         int src_h,
                         std::size_t src_stride,
                         int dst_w,
                         int dst_h,
                         std::vector<std::uint8_t>* dst_rgb,
                         std::size_t dst_stride,
                         std::string* error) {
  if (!dst_rgb) return false;
  dst_rgb->clear();

  if (!src_rgb || src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) {
    if (error) *error = "Invalid resize dimensions";
    return false;
  }
  if (src_stride < static_cast<std::size_t>(src_w) * 3u || dst_stride < static_cast<std::size_t>(dst_w) * 3u) {
    if (error) *error = "Invalid resize stride";
    return false;
  }

  dst_rgb->resize(dst_stride * static_cast<std::size_t>(dst_h));

  const float scale_x = static_cast<float>(src_w) / static_cast<float>(dst_w);
  const float scale_y = static_cast<float>(src_h) / static_cast<float>(dst_h);

  for (int y = 0; y < dst_h; ++y) {
    const float src_y = (static_cast<float>(y) + 0.5f) * scale_y - 0.5f;
    const int y0 = ClampInt(static_cast<int>(std::floor(src_y)), 0, src_h - 1);
    const int y1 = ClampInt(y0 + 1, 0, src_h - 1);
    const float fy = src_y - static_cast<float>(y0);

    auto* dst_row = dst_rgb->data() + static_cast<std::size_t>(y) * dst_stride;
    const auto* src_row0 = src_rgb + static_cast<std::size_t>(y0) * src_stride;
    const auto* src_row1 = src_rgb + static_cast<std::size_t>(y1) * src_stride;

    for (int x = 0; x < dst_w; ++x) {
      const float src_x = (static_cast<float>(x) + 0.5f) * scale_x - 0.5f;
      const int x0 = ClampInt(static_cast<int>(std::floor(src_x)), 0, src_w - 1);
      const int x1 = ClampInt(x0 + 1, 0, src_w - 1);
      const float fx = src_x - static_cast<float>(x0);

      const auto* p00 = src_row0 + static_cast<std::size_t>(x0) * 3u;
      const auto* p10 = src_row0 + static_cast<std::size_t>(x1) * 3u;
      const auto* p01 = src_row1 + static_cast<std::size_t>(x0) * 3u;
      const auto* p11 = src_row1 + static_cast<std::size_t>(x1) * 3u;

      for (int c = 0; c < 3; ++c) {
        const float v0 = static_cast<float>(p00[c]) + fx * (static_cast<float>(p10[c]) - static_cast<float>(p00[c]));
        const float v1 = static_cast<float>(p01[c]) + fx * (static_cast<float>(p11[c]) - static_cast<float>(p01[c]));
        const float v = v0 + fy * (v1 - v0);
        const int iv = ClampInt(static_cast<int>(std::lround(v)), 0, 255);
        dst_row[static_cast<std::size_t>(x) * 3u + static_cast<std::size_t>(c)] = static_cast<std::uint8_t>(iv);
      }
    }
  }

  return true;
}

}  // namespace studiocast::video
