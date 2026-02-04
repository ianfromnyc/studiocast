#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "core/config/daemon_config.h"
#include "core/maxine/ar_api.h"
#include "core/maxine/effects/ar_auto_frame_tracker.h"
#include "core/maxine/nvcv_api.h"
#include "core/maxine/vfx_api.h"
#include "core/probe/probe.h"
#include "core/util/json.h"
#include "core/util/strings.h"
#include "core/video/camera_effects_json.h"
#include "core/video/image_ppm.h"
#include "core/video/effects/effect_types.h"
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

        auto expectTrue = [&](const char* name, bool v) {
            if (v) return;
            ++failures;
            std::printf("[FAIL] %s\n", name);
        };

        auto expectIntEq = [&](const char* name, int got, int want) {
            if (got == want) return;
            ++failures;
            std::printf("[FAIL] %s\n  got:  %d\n  want: %d\n", name, got, want);
        };

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

        // Daemon config schema migration + round-trip.
        {
            namespace fs = std::filesystem;

            const char* oldXdg = std::getenv("XDG_CONFIG_HOME");
            const std::string oldXdgStr = oldXdg ? std::string(oldXdg) : std::string();

            char tmpl[] = "/tmp/studiocast_selftest_conf_XXXXXX";
            char* dir = ::mkdtemp(tmpl);
            if (!dir) {
                ++failures;
                std::printf("[FAIL] mkdtemp failed\n");
            } else {
                ::setenv("XDG_CONFIG_HOME", dir, 1);

                std::error_code ec;
                fs::create_directories(fs::path(dir) / "studiocast", ec);
                if (ec) {
                    ++failures;
                    std::printf("[FAIL] create_directories: %s\n", ec.message().c_str());
                }

                const fs::path confPath = fs::path(dir) / "studiocast" / "daemon.conf";

                // Legacy background keys should migrate to `video.effects.*`.
                {
                    std::ofstream out(confPath);
                    out << "video.mirror = true\n";
                    out << "video.background = blur\n";
                    out << "video.background_backend = cpu\n";
                    out << "video.background_strength = 13\n";
                    out << "video.background_remove_color = #112233\n";
                    out << "video.background_replace_image = /tmp/x.ppm\n";
                    out << "video.eye_contact = true\n";
                    out << "video.eye_contact_strength = 77\n";
                    out << "video.eye_contact_look_away = false\n";
                    out << "video.virtual_key_light = true\n";
                    out << "video.virtual_key_light_intensity = 42\n";
                    out << "video.virtual_key_light_temperature = warm\n";
                    out << "video.vignette = true\n";
                    out << "video.vignette_intensity = 9\n";
                    out << "video.vignette_center_on_face = false\n";
                }

                const auto dc = studiocast::config::LoadDaemonConfig();
                expectEq("daemon_config migrate vb mode", dc.video_effects_virtual_background_mode, "blur");
                expectIntEq("daemon_config migrate vb blur_strength", dc.video_effects_virtual_background_blur_strength, 13);
                expectEq("daemon_config migrate vb remove_color", dc.video_effects_virtual_background_remove_color, "#112233");
                expectEq("daemon_config migrate vb replace_path", dc.video_effects_virtual_background_replace_path, "/tmp/x.ppm");
                expectTrue("daemon_config migrate mirror", dc.video_mirror);
                expectTrue("daemon_config migrate eye_contact enabled", dc.video_effects_eye_contact_enabled);
                expectIntEq("daemon_config migrate eye_contact strength", dc.video_effects_eye_contact_strength, 77);
                expectTrue("daemon_config migrate key_light enabled", dc.video_effects_virtual_key_light_enabled);
                expectIntEq("daemon_config migrate key_light intensity", dc.video_effects_virtual_key_light_intensity, 42);

                const auto vc = studiocast::config::ToVideoServiceConfig(dc);
                expectTrue("ToVideoServiceConfig mirror", vc.pipeline.effects.mirror);
                expectTrue("ToVideoServiceConfig background blur",
                           vc.pipeline.effects.background == studiocast::video::effects::BackgroundEffect::blur);
                expectIntEq("ToVideoServiceConfig background_strength", vc.pipeline.effects.background_strength, 13);
                expectTrue("ToVideoServiceConfig background_backend not cpu",
                           vc.pipeline.effects.background_backend != studiocast::video::effects::EffectBackend::cpu);
                expectTrue("ToVideoServiceConfig eye_contact enabled", vc.pipeline.effects.eye_contact.enabled);

                // New schema should round-trip through Save/Load.
                {
                    std::string err;
                    if (!studiocast::config::SaveDaemonConfig(dc, &err)) {
                        ++failures;
                        std::printf("[FAIL] SaveDaemonConfig: %s\n", err.c_str());
                    }
                    const auto dc2 = studiocast::config::LoadDaemonConfig();
                    const auto vc2 = studiocast::config::ToVideoServiceConfig(dc2);
                    expectTrue("roundtrip background blur",
                               vc2.pipeline.effects.background == studiocast::video::effects::BackgroundEffect::blur);
                    expectIntEq("roundtrip background_strength", vc2.pipeline.effects.background_strength, 13);
                    expectTrue("roundtrip eye_contact enabled", vc2.pipeline.effects.eye_contact.enabled);
                    expectTrue("roundtrip key_light enabled", vc2.pipeline.effects.virtual_key_light.enabled);
                }

                // Legacy auto_frame should migrate to `video.effects.auto_frame.*`.
                {
                    std::ofstream out(confPath);
                    out << "video.background = auto_frame\n";
                    out << "video.auto_frame_strength = 88\n";
                    out << "video.auto_frame_smoothing = 12\n";
                    out << "video.auto_frame_headroom = 0.33\n";
                }

                const auto dc_af = studiocast::config::LoadDaemonConfig();
                expectTrue("daemon_config migrate auto_frame enabled", dc_af.video_effects_auto_frame_enabled);
                expectIntEq("daemon_config migrate auto_frame zoom", dc_af.video_effects_auto_frame_zoom, 88);
                const auto vc_af = studiocast::config::ToVideoServiceConfig(dc_af);
                expectTrue("ToVideoServiceConfig auto_frame",
                           vc_af.pipeline.effects.background == studiocast::video::effects::BackgroundEffect::auto_frame);

                // Restore env.
                if (oldXdg) {
                    ::setenv("XDG_CONFIG_HOME", oldXdgStr.c_str(), 1);
                } else {
                    ::unsetenv("XDG_CONFIG_HOME");
                }
            }
        }

        // Effects JSON patch (line-based IPC helper).
        {
            studiocast::video::CameraEffects fx;
            std::string jerr;

            const std::string pretty =
                "{\n"
                "  \"virtual_background\": {\n"
                "    \"mode\": \"replace\",\n"
                "    \"replace_path\": \"/tmp/some path/with spaces/bg.ppm\"\n"
                "  }\n"
                "}\n";

            const std::string minified = studiocast::util::json::Minify(pretty);
            expectTrue("json minify keeps spaces in strings",
                       minified.find("/tmp/some path/with spaces/bg.ppm") != std::string::npos);

            if (!studiocast::video::ApplyCameraEffectsPatchJsonText(minified, &fx, &jerr)) {
                ++failures;
                std::printf("[FAIL] ApplyCameraEffectsPatchJsonText: %s\n", jerr.c_str());
            } else {
                expectEq("effects patch replace_path", fx.background_replace_image.string(), "/tmp/some path/with spaces/bg.ppm");
                expectTrue("effects patch background replace",
                           fx.background == studiocast::video::effects::BackgroundEffect::replace);
            }

            const std::string af = "{\"auto_frame\":{\"enabled\":true,\"zoom\":77}}";
            jerr.clear();
            if (!studiocast::video::ApplyCameraEffectsPatchJsonText(af, &fx, &jerr)) {
                ++failures;
                std::printf("[FAIL] ApplyCameraEffectsPatchJsonText auto_frame: %s\n", jerr.c_str());
            } else {
                expectTrue("effects patch auto_frame background",
                           fx.background == studiocast::video::effects::BackgroundEffect::auto_frame);
                expectIntEq("effects patch auto_frame zoom", fx.auto_frame.strength, 77);
            }

            const std::string blur = "{\"virtual_background\":{\"mode\":\"blur\",\"blur_strength\":9}}";
            jerr.clear();
            if (!studiocast::video::ApplyCameraEffectsPatchJsonText(blur, &fx, &jerr)) {
                ++failures;
                std::printf("[FAIL] ApplyCameraEffectsPatchJsonText blur: %s\n", jerr.c_str());
            } else {
                expectTrue("effects patch blur disables auto_frame",
                           fx.background == studiocast::video::effects::BackgroundEffect::blur);
                expectIntEq("effects patch blur_strength", fx.background_strength, 9);
            }

            // Serializer output should be valid JSON and re-applicable.
            const std::string fxJson = studiocast::video::CameraEffectsToJson(fx);
            studiocast::util::json::Value parsed;
            jerr.clear();
            if (!studiocast::util::json::Parse(fxJson, &parsed, &jerr)) {
                ++failures;
                std::printf("[FAIL] CameraEffectsToJson parseable: %s\n", jerr.c_str());
            }

            studiocast::video::CameraEffects fx2;
            const std::string wrapper = std::string("{\"video_effects\":") + fxJson + "}";
            jerr.clear();
            if (!studiocast::video::ApplyCameraEffectsPatchJsonText(wrapper, &fx2, &jerr)) {
                ++failures;
                std::printf("[FAIL] CameraEffectsToJson roundtrip apply: %s\n", jerr.c_str());
            }
            expectTrue("CameraEffectsToJson roundtrip background", fx2.background == fx.background);
            expectEq("CameraEffectsToJson roundtrip path", fx2.background_replace_image.string(), fx.background_replace_image.string());
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
