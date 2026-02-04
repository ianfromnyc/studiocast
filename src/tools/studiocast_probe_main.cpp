#include <cstdio>
#include <cstdint>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "core/maxine/ar_api.h"
#include "core/maxine/effects/ar_auto_frame_tracker.h"
#include "core/maxine/nvcv_api.h"
#include "core/maxine/vfx_api.h"
#include "core/probe/probe.h"
#include "core/util/strings.h"
#include "core/video/image_ppm.h"
#include "studiocast/version.h"

namespace {
    bool hasArg(int argc, char** argv, std::string_view flag) {
        for (int i = 1; i < argc; ++i) {
            if (argv[i] && std::string_view(argv[i]) == flag) return true;
        }
        return false;
    }

    int RunSelfTest() {
        int failures = 0;

        auto expectEq = [&](const char* name, const std::string& got, const std::string& want) {
            if (got == want) return;
            ++failures;
            std::printf("[FAIL] %s\n  got:  '%s'\n  want: '%s'\n", name, got.c_str(), want.c_str());
        };

        auto expectVecEq = [&](const char* name,
                               const std::vector<std::string>& got,
                               const std::vector<std::string>& want) {
            if (got == want) return;
            ++failures;
            std::printf("[FAIL] %s\n", name);
            std::printf("  got (%zu):\n", got.size());
            for (const auto& s : got) std::printf("    '%s'\n", s.c_str());
            std::printf("  want (%zu):\n", want.size());
            for (const auto& s : want) std::printf("    '%s'\n", s.c_str());
        };

        using studiocast::util::FirstNonEmptyLine;
        using studiocast::util::Split;
        using studiocast::util::SplitLines;
        using studiocast::util::TrimCopy;

        expectEq("TrimCopy", TrimCopy("  hi \n"), "hi");
        expectVecEq("Split", Split("a,b,,c", ','), {"a", "b", "", "c"});
        expectVecEq("SplitLines", SplitLines("a\r\nb\n\nc"), {"a", "b", "", "c"});
        expectEq("FirstNonEmptyLine", FirstNonEmptyLine("\n  \n x \n"), "x");

        {
            studiocast::maxine::NvcvApi api;
            std::string err;
            const bool ok = api.Initialize(studiocast::maxine::NvcvApi::Requirement::Minimal, &err);

            // Self-test must be stable without Maxine installed:
            // - If Maxine is present, initialization may succeed.
            // - If Maxine is absent, we expect a clean failure with a non-empty error message.
            if (!ok && err.empty()) {
                ++failures;
                std::printf("[FAIL] NvcvApi.Initialize returned false but provided no error message\n");
            }
        }

        // Virtual Background CPU-side math sanity checks.
        {
            auto composite_u8 = [](std::uint8_t fg, std::uint8_t bg, std::uint8_t a) -> std::uint8_t {
                // a is matte alpha for foreground.
                const int af = static_cast<int>(a);
                const int ab = 255 - af;
                const int v = static_cast<int>(fg) * af + static_cast<int>(bg) * ab;
                return static_cast<std::uint8_t>((v + 127) / 255);
            };

            // fg=255, bg=0
            if (composite_u8(255, 0, 0) != 0) {
                ++failures;
                std::printf("[FAIL] composite_u8 alpha=0\n");
            }
            if (composite_u8(255, 0, 255) != 255) {
                ++failures;
                std::printf("[FAIL] composite_u8 alpha=255\n");
            }
            const auto mid = composite_u8(255, 0, 128);
            if (mid < 126 || mid > 129) {
                ++failures;
                std::printf("[FAIL] composite_u8 alpha=128 got=%u\n", static_cast<unsigned>(mid));
            }
        }

        // Auto Frame crop math + smoothing (deterministic; no Maxine runtime needed).
        {
            using studiocast::maxine::effects::ArAutoFrameTracker;
            using studiocast::maxine::effects::AutoFrameKnobs;
            using studiocast::maxine::effects::RectF;

            const int w = 1280;
            const int h = 720;
            const float aspect = 16.0f / 9.0f;

            // Center crop at 1x should be full frame for matching aspect.
            {
                const RectF r = ArAutoFrameTracker::CenterCrop(w, h, aspect, 1.0f);
                if (std::abs(r.x) > 1e-3f || std::abs(r.y) > 1e-3f ||
                    std::abs(r.w - 1280.0f) > 1e-3f || std::abs(r.h - 720.0f) > 1e-3f) {
                    ++failures;
                    std::printf("[FAIL] AutoFrame CenterCrop full-frame mismatch\n");
                }
            }

            // Stronger strength should produce a tighter crop for the same box.
            {
                const RectF face = RectF{540.0f, 180.0f, 200.0f, 200.0f};
                AutoFrameKnobs k0; k0.strength = 0; k0.smoothing = 0; k0.headroom = 0.15f;
                AutoFrameKnobs k1; k1.strength = 100; k1.smoothing = 0; k1.headroom = 0.15f;

                const RectF a = ArAutoFrameTracker::ComputeTargetCropFromBoxPx(face, w, h, aspect, k0);
                const RectF b = ArAutoFrameTracker::ComputeTargetCropFromBoxPx(face, w, h, aspect, k1);
                if (!(b.w < a.w && b.h < a.h)) {
                    ++failures;
                    std::printf("[FAIL] AutoFrame strength should tighten crop\n");
                }
            }

            // Smoothing alpha monotonic: smoothing=0 should respond faster than smoothing=100.
            {
                const float a0 = ArAutoFrameTracker::SmoothingAlpha(0);
                const float a1 = ArAutoFrameTracker::SmoothingAlpha(100);
                if (!(a0 > a1 && a0 > 0.5f && a1 < 0.2f)) {
                    ++failures;
                    std::printf("[FAIL] AutoFrame SmoothingAlpha unexpected mapping a0=%f a1=%f\n", a0, a1);
                }
            }
        }

        // PPM loader + resize check (dependency-free replace-image path).
        {
            // 2x2 PPM P6: red, green, blue, white.
            const char* tmpPath = "/tmp/studiocast_selftest_bg.ppm";
            {
                std::ofstream out(tmpPath, std::ios::binary);
                if (!out) {
                    ++failures;
                    std::printf("[FAIL] failed to open tmp PPM for writing\n");
                } else {
                    out << "P6\n2 2\n255\n";
                    const std::uint8_t px[] = {
                        255, 0, 0,   0, 255, 0,
                        0, 0, 255,   255, 255, 255,
                    };
                    out.write(reinterpret_cast<const char*>(px), static_cast<std::streamsize>(sizeof(px)));
                }
            }

            int w = 0, h = 0;
            std::vector<std::uint8_t> rgb;
            std::string imgErr;
            if (!studiocast::video::LoadPpmP6Rgb24(tmpPath, &w, &h, &rgb, &imgErr)) {
                ++failures;
                std::printf("[FAIL] LoadPpmP6Rgb24: %s\n", imgErr.c_str());
            } else {
                if (w != 2 || h != 2 || rgb.size() != 12) {
                    ++failures;
                    std::printf("[FAIL] LoadPpmP6Rgb24 unexpected dims\n");
                }
            }

            std::vector<std::uint8_t> resized;
            std::string resizeErr;
            if (!rgb.empty() && !studiocast::video::ResizeRgb24Bilinear(rgb.data(), 2, 2, 6, 4, 4, &resized, 12, &resizeErr)) {
                ++failures;
                std::printf("[FAIL] ResizeRgb24Bilinear: %s\n", resizeErr.c_str());
            } else if (!resized.empty()) {
                // Top-left should stay close to red.
                const std::uint8_t r = resized[0];
                const std::uint8_t g = resized[1];
                const std::uint8_t b = resized[2];
                if (r < 200 || g > 80 || b > 80) {
                    ++failures;
                    std::printf("[FAIL] ResizeRgb24Bilinear unexpected top-left pixel: %u,%u,%u\n",
                                static_cast<unsigned>(r), static_cast<unsigned>(g), static_cast<unsigned>(b));
                }
            }
        }

        {
            studiocast::maxine::vfx::VfxApi api;
            std::string err;
            const bool ok = api.Initialize(&err);

            // Self-test must be stable without Maxine installed:
            // - If Maxine is present, initialization may succeed.
            // - If Maxine is absent, we expect a clean failure with a non-empty error message.
            if (!ok && err.empty()) {
                ++failures;
                std::printf("[FAIL] VfxApi.Initialize returned false but provided no error message\n");
            }

            // If the library is present, CreateEffect should either succeed or return an error
            // code that maps to a useful string.
            if (ok) {
                studiocast::maxine::vfx::NvVFX_Handle h = nullptr;
                const studiocast::maxine::NvCV_Status st =
                    api.f().NvVFX_CreateEffect(studiocast::maxine::vfx::NVVFX_FX_GREEN_SCREEN, &h);
                if (st == studiocast::maxine::NVCV_SUCCESS) {
                    if (h) {
                        api.f().NvVFX_DestroyEffect(h);
                    }
                } else {
                    const std::string msg = api.StatusToString(st);
                    if (msg.empty()) {
                        ++failures;
                        std::printf("[FAIL] VfxApi.StatusToString returned empty message for status=%d\n", st);
                    }
                }
            }
        }

        {
            studiocast::maxine::ar::ArApi api;
            std::string err;
            const bool ok = api.Initialize(&err);

            // Self-test must be stable without Maxine installed:
            // - If Maxine is present, initialization may succeed.
            // - If Maxine is absent, we expect a clean failure with a non-empty error message.
            if (!ok && err.empty()) {
                ++failures;
                std::printf("[FAIL] ArApi.Initialize returned false but provided no error message\n");
            }

            // If the library is present, Create should either succeed or return an error
            // code that maps to a useful string.
            if (ok) {
                studiocast::maxine::ar::NvAR_FeatureHandle h = nullptr;
                const studiocast::maxine::NvCV_Status st = api.f().NvAR_Create(
                    studiocast::maxine::ar::NVAR_FEATURE_GAZE_REDIRECTION, &h);
                if (st == studiocast::maxine::NVCV_SUCCESS) {
                    if (h) {
                        api.f().NvAR_Destroy(h);
                    }
                } else {
                    const std::string msg = api.StatusToString(st);
                    if (msg.empty()) {
                        ++failures;
                        std::printf("[FAIL] ArApi.StatusToString returned empty message for status=%d\n", st);
                    }
                }
            }
        }

        if (failures == 0) {
            std::printf("SELFTEST OK\n");
            return 0;
        }
        std::printf("SELFTEST FAILED (%d)\n", failures);
        return 1;
    }
}  // namespace

int main(int argc, char** argv) {
    if (hasArg(argc, argv, "--self-test")) {
        return RunSelfTest();
    }

    const bool json = hasArg(argc, argv, "--json");
    const bool verbose = hasArg(argc, argv, "--verbose");
    const bool strict = hasArg(argc, argv, "--strict");

    const auto report = studiocast::probe::Run(verbose);

    if (json) {
        std::printf("%s\n", report.ToJson().c_str());
    } else {
        std::printf("%s\n", report.ToText().c_str());
    }

    if (!strict) return 0;
    return report.AllChecksPassed() ? 0 : 1;
}
