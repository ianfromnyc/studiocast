#include "core/video/v4l2_capture.h"
#include "core/video/capture_error_policy.h"
#include "core/video/v4l2_writer.h"

#include <cstddef>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <vector>

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

// The capture path reads a row with the same converters the writer path
// writes one with, so both must agree on the row size. A YUYV row packs
// pixels in pairs: an odd width still fills the whole final pair, and a row
// of width * 2 bytes makes the odd-width tail read past the frame.
bool TestCaptureRowSizeMatchesTheSharedRowRule() {
  using studiocast::video::CaptureMinBytesPerLine;
  using studiocast::video::MinBytesPerLine;
  using studiocast::video::PixelFormat;

  for (const int width : {1, 2, 3, 7, 16, 17, 640}) {
    const std::size_t yuyv =
        CaptureMinBytesPerLine(width, CapturePixelFormat::yuyv);
    const std::size_t expected_yuyv = MinBytesPerLine(width, PixelFormat::yuyv);
    if (!Expect(yuyv == expected_yuyv,
                "captured YUYV row size must hold the whole final pair")) {
      std::cerr << "  width " << width << ": expected " << expected_yuyv
                << ", got " << yuyv << "\n";
      return false;
    }

    const std::size_t rgb24 =
        CaptureMinBytesPerLine(width, CapturePixelFormat::rgb24);
    const std::size_t expected_rgb24 =
        MinBytesPerLine(width, PixelFormat::rgb24);
    if (!Expect(rgb24 == expected_rgb24,
                "captured RGB24 row size must be three bytes per pixel")) {
      std::cerr << "  width " << width << ": expected " << expected_rgb24
                << ", got " << rgb24 << "\n";
      return false;
    }
  }

  return Expect(CaptureMinBytesPerLine(640, CapturePixelFormat::mjpeg) == 0u,
                "MJPEG is compressed and has no row size") &&
         Expect(CaptureMinBytesPerLine(0, CapturePixelFormat::yuyv) == 0u,
                "a width of zero has no row");
}

} // namespace

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

bool TestV4l2CaptureRowSizeMatchesTheSharedRowRule() {
  return TestCaptureRowSizeMatchesTheSharedRowRule();
}

} // namespace studiocast::tests
