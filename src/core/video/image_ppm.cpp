#include "core/video/image_ppm.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <limits>
#include <sstream>

#include <png.h>

namespace studiocast::video {
namespace {

std::string ToLowerAscii(std::string s) {
  for (char &c : s)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

struct PngErrorContext {
  std::string *error = nullptr;
};

void PngErrorFn(png_structp png_ptr, png_const_charp msg) {
  auto *ctx = static_cast<PngErrorContext *>(png_get_error_ptr(png_ptr));
  if (ctx && ctx->error) {
    *ctx->error = (msg && msg[0] != '\0') ? msg : "libpng error";
  }
  longjmp(png_jmpbuf(png_ptr), 1);
}

void PngWarnFn(png_structp /*png_ptr*/, png_const_charp /*msg*/) {}

bool LoadPngRgb24(const std::filesystem::path &path, int *out_w, int *out_h,
                  std::vector<std::uint8_t> *out_rgb, std::string *error) {
  if (out_w)
    *out_w = 0;
  if (out_h)
    *out_h = 0;
  if (out_rgb)
    out_rgb->clear();

  std::FILE *fp = std::fopen(path.c_str(), "rb");
  if (!fp) {
    if (error)
      *error = "Failed to open image: " + path.string();
    return false;
  }

  png_byte sig[8];
  if (std::fread(sig, 1, sizeof(sig), fp) != sizeof(sig) ||
      png_sig_cmp(sig, 0, sizeof(sig)) != 0) {
    std::fclose(fp);
    if (error)
      *error = "Invalid PNG signature: " + path.string();
    return false;
  }

  std::string png_err;
  PngErrorContext err_ctx{&png_err};

  png_structp png_ptr = png_create_read_struct(PNG_LIBPNG_VER_STRING, &err_ctx,
                                               PngErrorFn, PngWarnFn);
  if (!png_ptr) {
    std::fclose(fp);
    if (error)
      *error = "libpng: png_create_read_struct failed";
    return false;
  }
  png_infop info_ptr = png_create_info_struct(png_ptr);
  if (!info_ptr) {
    png_destroy_read_struct(&png_ptr, nullptr, nullptr);
    std::fclose(fp);
    if (error)
      *error = "libpng: png_create_info_struct failed";
    return false;
  }

  if (setjmp(png_jmpbuf(png_ptr))) {
    png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
    std::fclose(fp);
    if (error) {
      *error = png_err.empty() ? "libpng: decode failed" : png_err;
    }
    return false;
  }

  png_init_io(png_ptr, fp);
  png_set_sig_bytes(png_ptr, static_cast<int>(sizeof(sig)));
  png_read_info(png_ptr, info_ptr);

  png_uint_32 w = 0, h = 0;
  int bit_depth = 0;
  int color_type = 0;
  int interlace_type = 0;
  int compression_type = 0;
  int filter_method = 0;
  png_get_IHDR(png_ptr, info_ptr, &w, &h, &bit_depth, &color_type,
               &interlace_type, &compression_type, &filter_method);

  if (w == 0 || h == 0 || w > 16384u || h > 16384u) {
    png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
    std::fclose(fp);
    if (error)
      *error = "Invalid PNG dimensions";
    return false;
  }

  if (bit_depth == 16)
    png_set_strip_16(png_ptr);

  if (color_type == PNG_COLOR_TYPE_PALETTE)
    png_set_palette_to_rgb(png_ptr);
  if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
    png_set_expand_gray_1_2_4_to_8(png_ptr);
  if (png_get_valid(png_ptr, info_ptr, PNG_INFO_tRNS))
    png_set_tRNS_to_alpha(png_ptr);
  if (color_type == PNG_COLOR_TYPE_GRAY ||
      color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
    png_set_gray_to_rgb(png_ptr);

  // Ensure we have an alpha channel so we can composite deterministically.
  png_set_filler(png_ptr, 0xFF, PNG_FILLER_AFTER);

  png_read_update_info(png_ptr, info_ptr);
  const int channels = png_get_channels(png_ptr, info_ptr);
  if (channels != 4) {
    png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
    std::fclose(fp);
    if (error)
      *error = "Unsupported PNG channels (expected RGBA)";
    return false;
  }

  const png_size_t rowbytes = png_get_rowbytes(png_ptr, info_ptr);
  std::vector<png_byte> rgba(rowbytes * h);
  std::vector<png_bytep> rows(h);
  for (png_uint_32 y = 0; y < h; ++y) {
    rows[y] = rgba.data() + static_cast<std::size_t>(y) * rowbytes;
  }

  png_read_image(png_ptr, rows.data());
  png_read_end(png_ptr, nullptr);
  png_destroy_read_struct(&png_ptr, &info_ptr, nullptr);
  std::fclose(fp);

  std::vector<std::uint8_t> rgb(static_cast<std::size_t>(w) *
                                static_cast<std::size_t>(h) * 3u);
  for (png_uint_32 y = 0; y < h; ++y) {
    const auto *row = rows[y];
    for (png_uint_32 x = 0; x < w; ++x) {
      const std::size_t si = static_cast<std::size_t>(x) * 4u;
      const std::size_t di =
          (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
           static_cast<std::size_t>(x)) *
          3u;
      const std::uint32_t a = static_cast<std::uint32_t>(row[si + 3]);
      // Composite on black.
      rgb[di + 0] = static_cast<std::uint8_t>(
          (static_cast<std::uint32_t>(row[si + 0]) * a + 127u) / 255u);
      rgb[di + 1] = static_cast<std::uint8_t>(
          (static_cast<std::uint32_t>(row[si + 1]) * a + 127u) / 255u);
      rgb[di + 2] = static_cast<std::uint8_t>(
          (static_cast<std::uint32_t>(row[si + 2]) * a + 127u) / 255u);
    }
  }

  if (out_w)
    *out_w = static_cast<int>(w);
  if (out_h)
    *out_h = static_cast<int>(h);
  if (out_rgb)
    *out_rgb = std::move(rgb);
  return true;
}

// Read the next token in a PPM header, skipping whitespace and comments.
bool ReadPpmToken(std::istream &in, std::string *out) {
  if (!out)
    return false;
  out->clear();

  while (true) {
    int c = in.peek();
    if (c == EOF)
      return false;

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
    if (c == EOF)
      break;
    if (std::isspace(static_cast<unsigned char>(c)) || c == '#')
      break;
    tok.push_back(static_cast<char>(in.get()));
  }
  if (tok.empty())
    return false;
  *out = tok;
  return true;
}

int ClampInt(int v, int lo, int hi) { return std::max(lo, std::min(hi, v)); }

} // namespace

bool LoadImageRgb24(const std::filesystem::path &path, int *out_w, int *out_h,
                    std::vector<std::uint8_t> *out_rgb, std::string *error) {
  if (out_w)
    *out_w = 0;
  if (out_h)
    *out_h = 0;
  if (out_rgb)
    out_rgb->clear();
  if (error)
    error->clear();

  if (path.empty()) {
    if (error)
      *error = "Image path is empty";
    return false;
  }

  const std::string ext = ToLowerAscii(path.extension().string());
  if (ext == ".ppm") {
    return LoadPpmP6Rgb24(path, out_w, out_h, out_rgb, error);
  }
  if (ext == ".png") {
    return LoadPngRgb24(path, out_w, out_h, out_rgb, error);
  }

  if (error) {
    *error = "Unsupported image format '" + ext +
             "' (supported: .png, .ppm (P6)): " + path.string();
  }
  return false;
}

bool LoadPpmP6Rgb24(const std::filesystem::path &path, int *out_w, int *out_h,
                    std::vector<std::uint8_t> *out_rgb, std::string *error) {
  if (out_w)
    *out_w = 0;
  if (out_h)
    *out_h = 0;
  if (out_rgb)
    out_rgb->clear();

  std::ifstream f(path, std::ios::binary);
  if (!f) {
    if (error)
      *error = "Failed to open image: " + path.string();
    return false;
  }

  std::string magic;
  if (!ReadPpmToken(f, &magic) || magic != "P6") {
    if (error)
      *error = "Unsupported image format (expected PPM P6): " + path.string();
    return false;
  }

  std::string tok;
  if (!ReadPpmToken(f, &tok)) {
    if (error)
      *error = "Invalid PPM header (missing width)";
    return false;
  }
  const int w = std::atoi(tok.c_str());
  if (!ReadPpmToken(f, &tok)) {
    if (error)
      *error = "Invalid PPM header (missing height)";
    return false;
  }
  const int h = std::atoi(tok.c_str());
  if (!ReadPpmToken(f, &tok)) {
    if (error)
      *error = "Invalid PPM header (missing maxval)";
    return false;
  }
  const int maxval = std::atoi(tok.c_str());

  if (w <= 0 || h <= 0 || w > 16384 || h > 16384) {
    if (error)
      *error = "Invalid PPM dimensions";
    return false;
  }
  if (maxval <= 0 || maxval > 255) {
    if (error)
      *error = "Unsupported PPM maxval (must be <= 255)";
    return false;
  }

  // Consume single whitespace after maxval if present.
  int c = f.peek();
  if (c != EOF && std::isspace(static_cast<unsigned char>(c)))
    (void)f.get();

  const std::size_t sz =
      static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 3u;
  std::vector<std::uint8_t> rgb(sz);
  f.read(reinterpret_cast<char *>(rgb.data()),
         static_cast<std::streamsize>(sz));
  if (f.gcount() != static_cast<std::streamsize>(sz)) {
    if (error)
      *error = "PPM truncated (not enough pixel data)";
    return false;
  }

  if (out_w)
    *out_w = w;
  if (out_h)
    *out_h = h;
  if (out_rgb)
    *out_rgb = std::move(rgb);
  return true;
}

bool Rgb24BilinearResizePlan::Configure(int src_w, int src_h, int dst_w,
                                        int dst_h, std::string *error) {
  if (src_w <= 0 || src_h <= 0 || dst_w <= 0 || dst_h <= 0) {
    Clear();
    if (error)
      *error = "Invalid resize dimensions";
    return false;
  }

  if (src_w_ == src_w && src_h_ == src_h && dst_w_ == dst_w &&
      dst_h_ == dst_h) {
    return true;
  }

  std::vector<AxisSample> x_samples(static_cast<std::size_t>(dst_w));
  std::vector<AxisSample> y_samples(static_cast<std::size_t>(dst_h));

  const float scale_x = static_cast<float>(src_w) / static_cast<float>(dst_w);
  const float scale_y = static_cast<float>(src_h) / static_cast<float>(dst_h);

  for (int x = 0; x < dst_w; ++x) {
    const float src_x = (static_cast<float>(x) + 0.5f) * scale_x - 0.5f;
    const int x0 = ClampInt(static_cast<int>(std::floor(src_x)), 0, src_w - 1);
    x_samples[static_cast<std::size_t>(x)] = AxisSample{
        x0, ClampInt(x0 + 1, 0, src_w - 1), src_x - static_cast<float>(x0)};
  }

  for (int y = 0; y < dst_h; ++y) {
    const float src_y = (static_cast<float>(y) + 0.5f) * scale_y - 0.5f;
    const int y0 = ClampInt(static_cast<int>(std::floor(src_y)), 0, src_h - 1);
    y_samples[static_cast<std::size_t>(y)] = AxisSample{
        y0, ClampInt(y0 + 1, 0, src_h - 1), src_y - static_cast<float>(y0)};
  }

  src_w_ = src_w;
  src_h_ = src_h;
  dst_w_ = dst_w;
  dst_h_ = dst_h;
  x_samples_ = std::move(x_samples);
  y_samples_ = std::move(y_samples);
  return true;
}

void Rgb24BilinearResizePlan::Clear() {
  src_w_ = 0;
  src_h_ = 0;
  dst_w_ = 0;
  dst_h_ = 0;
  x_samples_.clear();
  y_samples_.clear();
}

bool Rgb24BilinearResizePlan::Apply(const std::uint8_t *src_rgb,
                                    std::size_t src_stride,
                                    std::vector<std::uint8_t> *dst_rgb,
                                    std::size_t dst_stride,
                                    std::string *error) const {
  if (!dst_rgb)
    return false;
  if (src_w_ <= 0 || src_h_ <= 0 || dst_w_ <= 0 || dst_h_ <= 0 ||
      x_samples_.size() != static_cast<std::size_t>(dst_w_) ||
      y_samples_.size() != static_cast<std::size_t>(dst_h_)) {
    dst_rgb->clear();
    if (error)
      *error = "Resize plan is not configured";
    return false;
  }

  if (!src_rgb) {
    dst_rgb->clear();
    if (error)
      *error = "Invalid resize source";
    return false;
  }
  if (src_stride < static_cast<std::size_t>(src_w_) * 3u ||
      dst_stride < static_cast<std::size_t>(dst_w_) * 3u) {
    dst_rgb->clear();
    if (error)
      *error = "Invalid resize stride";
    return false;
  }

  const std::size_t active_dst_stride = static_cast<std::size_t>(dst_w_) * 3u;
  const std::size_t dst_size = dst_stride * static_cast<std::size_t>(dst_h_);
  if (dst_rgb->size() != dst_size)
    dst_rgb->resize(dst_size);

  for (int y = 0; y < dst_h_; ++y) {
    const AxisSample ys = y_samples_[static_cast<std::size_t>(y)];
    auto *dst_row = dst_rgb->data() + static_cast<std::size_t>(y) * dst_stride;
    const auto *src_row0 =
        src_rgb + static_cast<std::size_t>(ys.i0) * src_stride;
    const auto *src_row1 =
        src_rgb + static_cast<std::size_t>(ys.i1) * src_stride;

    for (int x = 0; x < dst_w_; ++x) {
      const AxisSample xs = x_samples_[static_cast<std::size_t>(x)];
      const auto *p00 = src_row0 + static_cast<std::size_t>(xs.i0) * 3u;
      const auto *p10 = src_row0 + static_cast<std::size_t>(xs.i1) * 3u;
      const auto *p01 = src_row1 + static_cast<std::size_t>(xs.i0) * 3u;
      const auto *p11 = src_row1 + static_cast<std::size_t>(xs.i1) * 3u;

      for (int c = 0; c < 3; ++c) {
        const float v0 =
            static_cast<float>(p00[c]) +
            xs.f * (static_cast<float>(p10[c]) - static_cast<float>(p00[c]));
        const float v1 =
            static_cast<float>(p01[c]) +
            xs.f * (static_cast<float>(p11[c]) - static_cast<float>(p01[c]));
        const float v = v0 + ys.f * (v1 - v0);
        const int iv = ClampInt(static_cast<int>(std::lround(v)), 0, 255);
        dst_row[static_cast<std::size_t>(x) * 3u +
                static_cast<std::size_t>(c)] = static_cast<std::uint8_t>(iv);
      }
    }

    if (dst_stride > active_dst_stride) {
      std::memset(dst_row + active_dst_stride, 0,
                  dst_stride - active_dst_stride);
    }
  }

  return true;
}

bool ResizeRgb24Bilinear(const std::uint8_t *src_rgb, int src_w, int src_h,
                         std::size_t src_stride, int dst_w, int dst_h,
                         std::vector<std::uint8_t> *dst_rgb,
                         std::size_t dst_stride, std::string *error) {
  Rgb24BilinearResizePlan plan;
  if (!plan.Configure(src_w, src_h, dst_w, dst_h, error))
    return false;
  return plan.Apply(src_rgb, src_stride, dst_rgb, dst_stride, error);
}

} // namespace studiocast::video
