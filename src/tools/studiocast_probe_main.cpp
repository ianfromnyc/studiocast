#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "core/audio/pulse/pactl.h"
#include "core/config/daemon_config.h"
#include "core/maxine/availability.h"
#include "core/maxine/afx/afx_effect.h"
#include "core/maxine/afx_api.h"
#include "core/maxine/ar_api.h"
#include "core/maxine/effects/ar_auto_frame_tracker.h"
#include "core/maxine/maxine_manager.h"
#include "core/maxine/gpu_selection.h"
#include "core/maxine/paths.h"
#include "core/maxine/reason_codes.h"
#include "core/maxine/nvcv_api.h"
#include "core/maxine/vfx_api.h"
#include "core/probe/probe.h"
#include "core/util/json.h"
#include "core/util/strings.h"
#include "core/audio/effects/broadcast_audio_effects_json.h"
#include "core/audio/effects/broadcast_audio_effects_plan.h"
#include "core/video/broadcast_camera_effects_legacy_adapter.h"
#include "core/video/broadcast_camera_effects_json.h"
#include "core/video/camera_effects_json.h"
#include "core/video/capture_error_policy.h"
#include "core/video/legacy_camera_effects.h"
#include "core/video/effects/broadcast_effect_maxine_gate.h"
#include "core/video/effects/broadcast_effect_contract.h"
#include "core/video/effects/broadcast_effect_rules.h"
#include "core/video/effects/broadcast_effects_json.h"
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

        auto expectContains = [&](const char* name, const std::string& got, const std::string& needle) {
            if (got.find(needle) != std::string::npos) return;
            ++failures;
            std::printf("[FAIL] %s\n  did not find: '%s'\n  in: '%s'\n", name, needle.c_str(), got.c_str());
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

        // Capture error policy (pure logic; used by the camera pipeline).
        {
            using studiocast::video::IsRecoverableCaptureAcquireFailure;
            expectTrue("IsRecoverableCaptureAcquireFailure(timeout)",
                       IsRecoverableCaptureAcquireFailure("Timed out waiting for camera frame."));
            expectTrue("IsRecoverableCaptureAcquireFailure(empty)", IsRecoverableCaptureAcquireFailure(""));
            expectTrue("IsRecoverableCaptureAcquireFailure(fatal) == false",
                       !IsRecoverableCaptureAcquireFailure("poll failed: EIO"));
        }

        // `pactl info` parsing helper (deterministic; avoids needing pactl in self-test).
        {
            const std::string info =
                "Server String: /run/user/1000/pulse/native\n"
                "Default Sink:  alsa_output.pci-0000_00_1f.3.analog-stereo  \n"
                "Default Source:\talsa_input.pci-0000_00_1f.3.analog-stereo\n";

            const auto sink = studiocast::audio::pulse::ParseDefaultFromPactlInfo(info, "Default Sink:");
            expectEq("ParseDefaultFromPactlInfo(Default Sink)", sink ? *sink : "", "alsa_output.pci-0000_00_1f.3.analog-stereo");

            const auto src = studiocast::audio::pulse::ParseDefaultFromPactlInfo(info, "Default Source:");
            expectEq("ParseDefaultFromPactlInfo(Default Source)", src ? *src : "", "alsa_input.pci-0000_00_1f.3.analog-stereo");

            const auto missing = studiocast::audio::pulse::ParseDefaultFromPactlInfo(info, "Does Not Exist:");
            expectTrue("ParseDefaultFromPactlInfo(missing) is nullopt", !missing.has_value());
        }

        // Canonical effects model sanity: CPU backend must never be persisted/returned as a real option.
        {
            studiocast::video::CameraEffects legacy;
            legacy.background_backend = studiocast::video::effects::EffectBackend::cpu;

            const auto bfx = studiocast::video::ToBroadcastCameraEffects(legacy);
            expectTrue("ToBroadcastCameraEffects maps legacy cpu backend -> engine auto",
                       bfx.engine == studiocast::video::effects::EffectsEnginePreference::auto_select);

            const auto roundtrip = studiocast::video::ToLegacyCameraEffects(bfx);
            expectTrue("ToLegacyCameraEffects never returns cpu backend",
                       roundtrip.background_backend != studiocast::video::effects::EffectBackend::cpu);
        }

        // Canonical Broadcast-style audio effects JSON round-trip + validation.
        {
            using studiocast::audio::effects::BroadcastAudioEffects;
            using studiocast::audio::effects::BroadcastAudioEffectsJsonParseOptions;
            using studiocast::audio::effects::BroadcastAudioEffectsToJson;
            using studiocast::audio::effects::ParseBroadcastAudioEffectsJsonText;
            using studiocast::audio::effects::SuperresMode;

            BroadcastAudioEffects fx;
            fx.microphone.noise_removal_enabled = true;
            fx.microphone.room_echo_removal_enabled = true;
            fx.microphone.strength = 42;
            fx.microphone.studio_voice_enabled = false;
            fx.microphone.aec.enabled = true;
            fx.microphone.aec.reference_source = "monitor_source0";
            fx.microphone.superres.enabled = true;
            fx.microphone.superres.mode = SuperresMode::k8kTo16k;
            fx.speaker.noise_removal_enabled = true;
            fx.speaker.strength = 33;
            fx.speaker.superres.enabled = true;
            fx.speaker.superres.mode = SuperresMode::k16kTo48k;

            const auto json = BroadcastAudioEffectsToJson(fx);
            expectContains("BroadcastAudioEffectsToJson includes mic.aec", json, "\"aec\":{");
            expectContains("BroadcastAudioEffectsToJson includes aec reference_source", json,
                           "\"reference_source\":\"monitor_source0\"");
            expectContains("BroadcastAudioEffectsToJson includes mic.superres mode", json,
                           "\"microphone\":{");
            expectContains("BroadcastAudioEffectsToJson includes mic.superres mode", json,
                           "\"mode\":\"8k_to_16k\"");
            BroadcastAudioEffects parsed;
            std::vector<std::string> warnings;
            std::string error;
            expectTrue("ParseBroadcastAudioEffectsJsonText: round-trip ok",
                       ParseBroadcastAudioEffectsJsonText(json, &parsed, BroadcastAudioEffectsJsonParseOptions{}, &warnings, &error));
            expectEq("ParseBroadcastAudioEffectsJsonText: error empty", error, "");
            expectVecEq("ParseBroadcastAudioEffectsJsonText: warnings empty", warnings, {});
            expectTrue("BroadcastAudioEffects JSON round-trip equality", parsed == fx);

            // Superres mode validation.
            {
                const std::string badMode =
                    "{"\
                    "\"schema_version\":2,"\
                    "\"microphone\":{"\
                    "\"superres\":{"\
                    "\"enabled\":true,"\
                    "\"mode\":\"12k_to_48k\""\
                    "}"\
                    "}"\
                    "}";

                BroadcastAudioEffects parsedBadMode;
                warnings.clear();
                error.clear();
                expectTrue("ParseBroadcastAudioEffectsJsonText: invalid superres mode rejects",
                           !ParseBroadcastAudioEffectsJsonText(badMode,
                                                              &parsedBadMode,
                                                              BroadcastAudioEffectsJsonParseOptions{},
                                                              &warnings,
                                                              &error));
                expectContains("ParseBroadcastAudioEffectsJsonText: invalid superres mode error text",
                               error,
                               "superres.mode");
            }

            // Studio Voice must be mutually exclusive with mic noise/echo removal.
            const std::string bad =
                "{"
                "\"schema_version\":1,"
                "\"microphone\":{"
                "\"noise_removal_enabled\":true,"
                "\"room_echo_removal_enabled\":false,"
                "\"strength\":50,"
                "\"studio_voice_enabled\":true"
                "},"
                "\"speaker\":{"
                "\"noise_removal_enabled\":false,"
                "\"strength\":50"
                "}"
                "}";

            BroadcastAudioEffects parsedBad;
            warnings.clear();
            error.clear();
            expectTrue("ParseBroadcastAudioEffectsJsonText: exclusivity rejects",
                       !ParseBroadcastAudioEffectsJsonText(bad, &parsedBad, BroadcastAudioEffectsJsonParseOptions{}, &warnings, &error));
            expectContains("ParseBroadcastAudioEffectsJsonText: exclusivity error text", error, "mutually exclusive");
        }

        // Effect ordering + compatibility rules (single source of truth).
        {
            using studiocast::video::effects::BuildBroadcastEffectsPlan;
            using studiocast::video::effects::BroadcastCameraEffects;
            using studiocast::video::effects::VirtualBackgroundMode;

            const auto disabledHas = [](const studiocast::video::effects::BroadcastEffectsPlan& plan,
                                        std::string_view id) {
                for (const auto& d : plan.disabled) {
                    if (d.id == id) return true;
                }
                return false;
            };

            // VB replace requires replace_path.
            {
                BroadcastCameraEffects fx;
                fx.virtual_background.mode = VirtualBackgroundMode::replace;
                fx.virtual_background.replace_path.clear();

                const auto plan = BuildBroadcastEffectsPlan(fx);
                expectVecEq("EffectPlan: vb replace missing path -> no stages", plan.ordered_effect_ids, {});
                expectTrue("EffectPlan: vb replace missing path -> disabled list contains vb.replace",
                           disabledHas(plan, studiocast::video::effects::contract::kEffectIdVirtualBackgroundReplace));
            }

            // Auto Frame and Virtual Background are mutually exclusive; Auto Frame wins.
            {
                BroadcastCameraEffects fx;
                fx.auto_frame.enabled = true;
                fx.virtual_background.mode = VirtualBackgroundMode::blur;

                const auto plan = BuildBroadcastEffectsPlan(fx);
                expectVecEq("EffectPlan: auto_frame wins over vb.blur",
                            plan.ordered_effect_ids,
                            {std::string(studiocast::video::effects::contract::kEffectIdAutoFrame)});
                expectTrue("EffectPlan: vb.blur disabled when auto_frame enabled",
                           disabledHas(plan, studiocast::video::effects::contract::kEffectIdVirtualBackgroundBlur));
            }

            // Ordering + vignette attachment target.
            {
                BroadcastCameraEffects fx;
                fx.eye_contact.enabled = true;
                fx.virtual_background.mode = VirtualBackgroundMode::blur;
                fx.vignette.enabled = true;
                fx.vignette.intensity = 50;
                fx.mirror = true;

                const auto plan = BuildBroadcastEffectsPlan(fx);
                expectVecEq("EffectPlan: ordering eye_contact -> vb.blur -> vignette -> mirror",
                            plan.ordered_effect_ids,
                            {std::string(studiocast::video::effects::contract::kEffectIdEyeContact),
                             std::string(studiocast::video::effects::contract::kEffectIdVirtualBackgroundBlur),
                             std::string(studiocast::video::effects::contract::kEffectIdVignette),
                             std::string(studiocast::video::effects::contract::kEffectIdMirror)});
                expectEq("EffectPlan: vignette attaches to last GPU stage (vb.blur)",
                         plan.vignette_attach_to_effect_id,
                         std::string(studiocast::video::effects::contract::kEffectIdVirtualBackgroundBlur));
            }

            // Standalone vignette when no other GPU stage is enabled.
            {
                BroadcastCameraEffects fx;
                fx.vignette.enabled = true;
                fx.vignette.intensity = 25;
                const auto plan = BuildBroadcastEffectsPlan(fx);
                expectVecEq("EffectPlan: vignette-only stage", plan.ordered_effect_ids,
                            {std::string(studiocast::video::effects::contract::kEffectIdVignette)});
                expectEq("EffectPlan: vignette-only attach target empty", plan.vignette_attach_to_effect_id, "");
            }
        }

        {
            studiocast::video::effects::EffectsEnginePreference ep{};
            expectTrue("ParseEffectsEnginePreference(maxine)",
                       studiocast::video::effects::ParseEffectsEnginePreference("maxine", &ep) &&
                           ep == studiocast::video::effects::EffectsEnginePreference::maxine);
            expectTrue("ParseEffectsEnginePreference rejects cpu",
                       !studiocast::video::effects::ParseEffectsEnginePreference("cpu", &ep));
        }

        {
            studiocast::video::effects::BroadcastCameraEffects parsed;
            studiocast::video::effects::BroadcastEffectsJsonParseOptions options;
            options.allow_unknown_keys = true;
            options.allow_compat_keys = true;
            std::vector<std::string> warnings;
            std::string perr;

            expectTrue("ParseBroadcastCameraEffectsJsonText engine=maxine",
                       studiocast::video::effects::ParseBroadcastCameraEffectsJsonText(
                           "{\"engine\":\"maxine\"}", &parsed, options, &warnings, &perr) &&
                           parsed.engine == studiocast::video::effects::EffectsEnginePreference::maxine);

            warnings.clear();
            perr.clear();
            expectTrue("ParseBroadcastCameraEffectsJsonText rejects engine=cpu",
                       !studiocast::video::effects::ParseBroadcastCameraEffectsJsonText(
                           "{\"engine\":\"cpu\"}", &parsed, options, &warnings, &perr));
        }

        {
            studiocast::video::CameraEffects legacy;
            std::string perr;

            expectTrue("ApplyCameraEffectsPatchJsonText rejects engine=cpu",
                       !studiocast::video::ApplyCameraEffectsPatchJsonText("{\"engine\":\"cpu\"}", &legacy, &perr));

            perr.clear();
            expectTrue("ApplyCameraEffectsPatchJsonText rejects background_backend=cpu",
                       !studiocast::video::ApplyCameraEffectsPatchJsonText("{\"background_backend\":\"cpu\"}", &legacy, &perr));

            perr.clear();
            expectTrue("ApplyCameraEffectsPatchJsonText accepts background_backend=maxine",
                       studiocast::video::ApplyCameraEffectsPatchJsonText("{\"background_backend\":\"maxine\"}", &legacy, &perr));

            const auto bfx = studiocast::video::ToBroadcastCameraEffects(legacy);
            expectTrue("Legacy patch converts to Broadcast engine=maxine",
                       bfx.engine == studiocast::video::effects::EffectsEnginePreference::maxine);
        }

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

        {
            studiocast::maxine::afx::AfxApi api;
            std::string err;
            const bool ok = api.Initialize(&err);

            // Self-test must be stable without Maxine installed:
            // - If Maxine is present, initialization may succeed.
            // - If Maxine is absent, we expect a clean failure with a non-empty error message.
            if (!ok && err.empty()) {
                ++failures;
                std::printf("[FAIL] AfxApi.Initialize returned false but provided no error message\n");
            }

            // If the library is present, ensure required symbols resolved.
            if (ok) {
                if (!api.f().NvAFX_CreateEffect || !api.f().NvAFX_DestroyEffect ||
                    !api.f().NvAFX_SetU32 || !api.f().NvAFX_SetFloat || !api.f().NvAFX_SetString ||
                    !api.f().NvAFX_GetU32 || !api.f().NvAFX_Load || !api.f().NvAFX_Run) {
                    ++failures;
                    std::printf("[FAIL] AfxApi.Initialize succeeded but required symbols are missing\n");
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

                // Legacy background keys should migrate into canonical `video.effects.json`.
                {
                    std::ofstream out(confPath);
                    out << "video.mirror = true\n";
                    out << "video.background = blur\n";
                    out << "video.background_backend = maxine\n";
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
                expectEq("daemon_config migrate vb mode",
                         studiocast::video::effects::ToString(dc.video_effects.virtual_background.mode),
                         "blur");
                expectIntEq("daemon_config migrate vb blur_strength", dc.video_effects.virtual_background.strength, 13);
                expectEq("daemon_config migrate vb remove_color", dc.video_effects.virtual_background.remove_color, "#112233");
                expectEq("daemon_config migrate vb replace_path", dc.video_effects.virtual_background.replace_path, "/tmp/x.ppm");
                expectTrue("daemon_config migrate mirror", dc.video_effects.mirror);
                expectTrue("daemon_config migrate eye_contact enabled", dc.video_effects.eye_contact.enabled);
                expectIntEq("daemon_config migrate eye_contact strength", dc.video_effects.eye_contact.strength, 77);
                expectTrue("daemon_config migrate key_light enabled", dc.video_effects.virtual_key_light.enabled);
                expectIntEq("daemon_config migrate key_light intensity", dc.video_effects.virtual_key_light.intensity, 42);

                const auto vc = studiocast::config::ToVideoServiceConfig(dc);
                expectTrue("ToVideoServiceConfig mirror", vc.pipeline.effects.mirror);
                expectTrue("ToVideoServiceConfig vb blur",
                           vc.pipeline.effects.virtual_background.mode ==
                               studiocast::video::effects::VirtualBackgroundMode::blur);
                expectIntEq("ToVideoServiceConfig vb strength", vc.pipeline.effects.virtual_background.strength, 13);
                expectTrue("ToVideoServiceConfig engine maxine",
                           vc.pipeline.effects.engine == studiocast::video::effects::EffectsEnginePreference::maxine);
                expectTrue("ToVideoServiceConfig eye_contact enabled", vc.pipeline.effects.eye_contact.enabled);

                // Audio config: ensure new persisted fields are wired.
                {
                    auto dc_audio = dc;
                    dc_audio.audio_enabled = true;
                    dc_audio.audio_create_virtual_mic = true;
                    dc_audio.audio_source = "dummy_source";
                    dc_audio.audio_effects.microphone.noise_removal_enabled = true;
                    dc_audio.audio_effects.microphone.room_echo_removal_enabled = true;
                    dc_audio.audio_effects.microphone.strength = 55;
                    dc_audio.audio_effects.microphone.studio_voice_enabled = false;

                    const auto ac = studiocast::config::ToAudioServiceConfig(dc_audio);
                    expectTrue("ToAudioServiceConfig enabled", ac.enabled);
                    expectTrue("ToAudioServiceConfig create_virtual_mic", ac.create_virtual_mic);
                    expectEq("ToAudioServiceConfig source", ac.source_name, "dummy_source");
                    expectTrue("ToAudioServiceConfig noise enabled",
                               ac.effects.microphone.noise_removal_enabled);
                    expectTrue("ToAudioServiceConfig echo enabled",
                               ac.effects.microphone.room_echo_removal_enabled);
                    expectIntEq("ToAudioServiceConfig strength", ac.effects.microphone.strength, 55);
                }

                // New schema should round-trip through Save/Load.
                {
                    // Ensure audio fields are persisted too.
                    auto dc_save = dc;
                    dc_save.audio_enabled = true;
                    dc_save.audio_create_virtual_mic = true;
                    dc_save.audio_source = "dummy_source";
                    dc_save.audio_effects.microphone.noise_removal_enabled = true;
                    dc_save.audio_effects.microphone.room_echo_removal_enabled = true;
                    dc_save.audio_effects.microphone.strength = 55;

                    std::string err;
                    if (!studiocast::config::SaveDaemonConfig(dc_save, &err)) {
                        ++failures;
                        std::printf("[FAIL] SaveDaemonConfig: %s\n", err.c_str());
                    }

                    // Saved config should contain only the canonical effects blob (no legacy per-effect keys).
                    {
                        std::ifstream in(confPath);
                        const std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                        expectTrue("saved config has video.effects.json", content.find("video.effects.json") != std::string::npos);
                        expectTrue("saved config has audio.effects.json", content.find("audio.effects.json") != std::string::npos);
                        expectTrue("saved config removes video.mirror", content.find("video.mirror") == std::string::npos);
                        expectTrue("saved config removes video.background", content.find("video.background") == std::string::npos);
                        expectTrue("saved config removes video.effects.virtual_background",
                                   content.find("video.effects.virtual_background") == std::string::npos);
                    }

                    const auto dc2 = studiocast::config::LoadDaemonConfig();
                    const auto vc2 = studiocast::config::ToVideoServiceConfig(dc2);
                    expectTrue("roundtrip vb blur",
                               vc2.pipeline.effects.virtual_background.mode ==
                                   studiocast::video::effects::VirtualBackgroundMode::blur);
                    expectIntEq("roundtrip vb strength", vc2.pipeline.effects.virtual_background.strength, 13);
                    expectTrue("roundtrip eye_contact enabled", vc2.pipeline.effects.eye_contact.enabled);
                    expectTrue("roundtrip key_light enabled", vc2.pipeline.effects.virtual_key_light.enabled);

                    const auto ac2 = studiocast::config::ToAudioServiceConfig(dc2);
                    expectTrue("roundtrip audio enabled", ac2.enabled);
                    expectTrue("roundtrip audio create_virtual_mic", ac2.create_virtual_mic);
                    expectEq("roundtrip audio source", ac2.source_name, "dummy_source");
                    expectTrue("roundtrip audio noise enabled", ac2.effects.microphone.noise_removal_enabled);
                    expectTrue("roundtrip audio echo enabled", ac2.effects.microphone.room_echo_removal_enabled);
                    expectIntEq("roundtrip audio strength", ac2.effects.microphone.strength, 55);
                }

                // Audio effects JSON parsing should tolerate unknown keys (forward/backward drift).
                {
                    std::ofstream out(confPath);
                    out << "audio.enabled = true\n";
                    out << "audio.source = dummy_source\n";
                    out << "audio.effects.json = {\"schema_version\":1,\"microphone\":{\"noise_removal_enabled\":true,\"room_echo_removal_enabled\":false,\"strength\":42,\"studio_voice_enabled\":false,\"future_key\":123},\"speaker\":{\"enabled\":false}}\n";
                }
                {
                    const auto dcu = studiocast::config::LoadDaemonConfig();
                    const auto acu = studiocast::config::ToAudioServiceConfig(dcu);
                    expectTrue("audio unknown-key parse enabled", acu.enabled);
                    expectEq("audio unknown-key parse source", acu.source_name, "dummy_source");
                    expectTrue("audio unknown-key parse noise enabled", acu.effects.microphone.noise_removal_enabled);
                    expectIntEq("audio unknown-key parse strength", acu.effects.microphone.strength, 42);
                }

                // Legacy auto_frame should migrate into the canonical effects blob.
                {
                    std::ofstream out(confPath);
                    out << "video.background = auto_frame\n";
                    out << "video.auto_frame_strength = 88\n";
                    out << "video.auto_frame_smoothing = 12\n";
                    out << "video.auto_frame_headroom = 0.33\n";
                }

                const auto dc_af = studiocast::config::LoadDaemonConfig();
                expectTrue("daemon_config migrate auto_frame enabled", dc_af.video_effects.auto_frame.enabled);
                expectIntEq("daemon_config migrate auto_frame zoom", dc_af.video_effects.auto_frame.strength, 88);
                const auto vc_af = studiocast::config::ToVideoServiceConfig(dc_af);
                expectTrue("ToVideoServiceConfig auto_frame enabled", vc_af.pipeline.effects.auto_frame.enabled);

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
            studiocast::video::effects::BroadcastCameraEffects fx;
            std::string jerr;

            // Canonical contract: effect IDs are keys.
            const std::string pretty =
                "{\n"
                "  \"virtual_background.replace\": {\n"
                "    \"enabled\": true,\n"
                "    \"replace_path\": \"/tmp/some path/with spaces/bg.ppm\"\n"
                "  }\n"
                "}\n";

            const std::string minified = studiocast::util::json::Minify(pretty);
            expectTrue("json minify keeps spaces in strings",
                       minified.find("/tmp/some path/with spaces/bg.ppm") != std::string::npos);

            if (!studiocast::video::ApplyBroadcastCameraEffectsPatchJsonText(minified, &fx, &jerr)) {
                ++failures;
                std::printf("[FAIL] ApplyBroadcastCameraEffectsPatchJsonText: %s\n", jerr.c_str());
            } else {
                expectTrue("effects patch vb replace",
                           fx.virtual_background.mode == studiocast::video::effects::VirtualBackgroundMode::replace);
                expectEq("effects patch replace_path", fx.virtual_background.replace_path, "/tmp/some path/with spaces/bg.ppm");
            }

            const std::string af = "{\"auto_frame\":{\"enabled\":true,\"strength\":77}}";
            jerr.clear();
            if (!studiocast::video::ApplyBroadcastCameraEffectsPatchJsonText(af, &fx, &jerr)) {
                ++failures;
                std::printf("[FAIL] ApplyBroadcastCameraEffectsPatchJsonText auto_frame: %s\n", jerr.c_str());
            } else {
                expectTrue("effects patch auto_frame enabled", fx.auto_frame.enabled);
                expectIntEq("effects patch auto_frame zoom", fx.auto_frame.strength, 77);
                expectTrue("effects patch auto_frame disables vb",
                           fx.virtual_background.mode == studiocast::video::effects::VirtualBackgroundMode::none);
            }

            const std::string blur = "{\"virtual_background.blur\":{\"enabled\":true,\"strength\":9}}";
            jerr.clear();
            if (!studiocast::video::ApplyBroadcastCameraEffectsPatchJsonText(blur, &fx, &jerr)) {
                ++failures;
                std::printf("[FAIL] ApplyBroadcastCameraEffectsPatchJsonText blur: %s\n", jerr.c_str());
            } else {
                expectTrue("effects patch blur disables auto_frame", !fx.auto_frame.enabled);
                expectTrue("effects patch vb blur",
                           fx.virtual_background.mode == studiocast::video::effects::VirtualBackgroundMode::blur);
                expectIntEq("effects patch blur_strength", fx.virtual_background.strength, 9);
            }

            // Serializer output should be valid JSON and re-applicable.
            const std::string fxJson = studiocast::video::BroadcastCameraEffectsContractToJson(fx);
            studiocast::util::json::Value parsed;
            jerr.clear();
            if (!studiocast::util::json::Parse(fxJson, &parsed, &jerr)) {
                ++failures;
                std::printf("[FAIL] BroadcastCameraEffectsContractToJson parseable: %s\n", jerr.c_str());
            }

            studiocast::video::effects::BroadcastCameraEffects fx2;
            const std::string wrapper = std::string("{\"video_effects\":") + fxJson + "}";
            jerr.clear();
            if (!studiocast::video::ApplyBroadcastCameraEffectsPatchJsonText(wrapper, &fx2, &jerr)) {
                ++failures;
                std::printf("[FAIL] BroadcastCameraEffectsContractToJson roundtrip apply: %s\n", jerr.c_str());
            }
            expectTrue("BroadcastCameraEffectsContractToJson roundtrip mode",
                       fx2.virtual_background.mode == fx.virtual_background.mode);
            expectEq("BroadcastCameraEffectsContractToJson roundtrip path",
                     fx2.virtual_background.replace_path,
                     fx.virtual_background.replace_path);
        }

        // BroadcastCameraEffects canonical JSON round-trip + strict validation.
        {
            using studiocast::video::effects::BroadcastCameraEffects;
            using studiocast::video::effects::BroadcastCameraEffectsToJson;
            using studiocast::video::effects::BroadcastEffectsJsonParseOptions;
            using studiocast::video::effects::EffectsEnginePreference;
            using studiocast::video::effects::ParseBroadcastCameraEffectsJsonText;
            using studiocast::video::effects::VirtualBackgroundMode;

            BroadcastCameraEffects fx;
            fx.schema_version = studiocast::video::effects::kBroadcastEffectsSchemaVersion;
            fx.mirror = true;
            fx.engine = EffectsEnginePreference::maxine;
            fx.virtual_background.mode = VirtualBackgroundMode::replace;
            fx.virtual_background.strength = 9;
            fx.virtual_background.replace_path = "/tmp/bg.ppm";
            fx.auto_frame.enabled = false;
            fx.eye_contact.enabled = true;
            fx.eye_contact.strength = 77;
            fx.eye_contact.look_away_enabled = false;
            fx.video_noise_removal.enabled = true;
            fx.video_noise_removal.strength = 22;
            fx.virtual_key_light.enabled = true;
            fx.virtual_key_light.intensity = 42;
            fx.virtual_key_light.temperature = 5000;
            fx.vignette.enabled = true;
            fx.vignette.intensity = 33;

            const std::string json = BroadcastCameraEffectsToJson(fx);
            BroadcastCameraEffects out;
            std::vector<std::string> warnings;
            std::string err;

            BroadcastEffectsJsonParseOptions opt;
            opt.allow_unknown_keys = false;
            opt.allow_compat_keys = true;
            if (!ParseBroadcastCameraEffectsJsonText(json, &out, opt, &warnings, &err)) {
                ++failures;
                std::printf("[FAIL] ParseBroadcastCameraEffectsJsonText roundtrip: %s\n", err.c_str());
            } else {
                expectTrue("BroadcastCameraEffects roundtrip equals", out == fx);
                expectTrue("BroadcastCameraEffects roundtrip no warnings", warnings.empty());
            }

            // Unknown key strict vs compat.
            {
                const std::string u = "{\"schema_version\":1,\"unknown\":123}";

                BroadcastCameraEffects tmp;
                warnings.clear();
                err.clear();
                opt.allow_unknown_keys = false;
                if (ParseBroadcastCameraEffectsJsonText(u, &tmp, opt, &warnings, &err)) {
                    ++failures;
                    std::printf("[FAIL] BroadcastCameraEffects unknown key should fail in strict mode\n");
                } else {
                    expectContains("BroadcastCameraEffects unknown strict msg", err, "unknown key");
                }

                warnings.clear();
                err.clear();
                opt.allow_unknown_keys = true;
                if (!ParseBroadcastCameraEffectsJsonText(u, &tmp, opt, &warnings, &err)) {
                    ++failures;
                    std::printf("[FAIL] BroadcastCameraEffects unknown key should pass in compat mode: %s\n", err.c_str());
                } else {
                    expectTrue("BroadcastCameraEffects unknown compat warns", !warnings.empty());
                }
            }

            // Validation: virtual_background.mode
            {
                const std::string badMode =
                    "{\"schema_version\":1,\"virtual_background\":{\"mode\":\"wat\"}}";
                BroadcastCameraEffects tmp;
                warnings.clear();
                err.clear();
                opt.allow_unknown_keys = false;
                if (ParseBroadcastCameraEffectsJsonText(badMode, &tmp, opt, &warnings, &err)) {
                    ++failures;
                    std::printf("[FAIL] BroadcastCameraEffects bad mode should fail\n");
                } else {
                    expectContains("BroadcastCameraEffects bad mode msg", err, "virtual_background.mode");
                }
            }

            // Validation: conflict between auto_frame and virtual_background.
            {
                const std::string conflict =
                    "{\"schema_version\":1,\"auto_frame\":{\"enabled\":true},\"virtual_background\":{\"mode\":\"blur\"}}";
                BroadcastCameraEffects tmp;
                warnings.clear();
                err.clear();
                if (ParseBroadcastCameraEffectsJsonText(conflict, &tmp, opt, &warnings, &err)) {
                    ++failures;
                    std::printf("[FAIL] BroadcastCameraEffects conflict should fail\n");
                } else {
                    expectContains("BroadcastCameraEffects conflict msg", err, "auto_frame.enabled");
                }
            }

            // Compatibility: temperature_preset alias.
            {
                const std::string preset =
                    "{\"schema_version\":1,\"virtual_key_light\":{\"temperature_preset\":\"warm\"}}";
                BroadcastCameraEffects tmp;
                warnings.clear();
                err.clear();
                opt.allow_unknown_keys = false;
                opt.allow_compat_keys = true;
                if (!ParseBroadcastCameraEffectsJsonText(preset, &tmp, opt, &warnings, &err)) {
                    ++failures;
                    std::printf("[FAIL] BroadcastCameraEffects temperature_preset compat: %s\n", err.c_str());
                } else {
                    expectTrue("BroadcastCameraEffects preset sets temp", tmp.virtual_key_light.temperature != 4500);
                    expectTrue("BroadcastCameraEffects preset warns", !warnings.empty());
                }
            }
        }

        // Canonical Maxine blocked messaging copy (Task 25).
        {
            const char* oldHome = std::getenv("HOME");
            const std::string oldHomeStr = oldHome ? std::string(oldHome) : std::string();

            // Force a deterministic HOME so we can validate "~/.local/share/..." rendering.
            ::setenv("HOME", "/home/studiocast_selftest_home", 1);

            const auto mkDiag = [&] {
                studiocast::maxine::MaxineDiagnostics d;
                d.gpu.ok = true;
                d.driver.ok = true;
                return d;
            };

            // 1) No GPU.
            {
                auto d = mkDiag();
                d.gpu.ok = false;
                const auto c = studiocast::maxine::BuildCanonicalMaxineBlockedCopy(d);
                expectEq("maxine_copy no_gpu summary", c.summary,
                         "Maxine unavailable: no supported NVIDIA GPU detected.");
            }

            // 2) Driver too old.
            {
                auto d = mkDiag();
                d.driver.ok = false;
                d.driver.version = "560.0";
                const auto c = studiocast::maxine::BuildCanonicalMaxineBlockedCopy(d);
                expectEq("maxine_copy driver_old summary", c.summary,
                         "Maxine unavailable: NVIDIA driver too old (need R570+).");
                const std::string s = studiocast::maxine::FormatCanonicalMaxineBlockedCopy(c);
                expectContains("maxine_copy driver_old has probe", s,
                               "Run `studiocast-probe` to verify GPU/driver.");
            }

            // 3) VFX SDK missing: must show exact expected paths.
            {
                auto d = mkDiag();
                d.vfx.root_source = "xdg";
                d.vfx.root_exists = false;
                d.vfx.library_exists = false;
                d.vfx.candidate_roots.push_back(
                    "/home/studiocast_selftest_home/.local/share/studiocast/maxine/VideoFX");

                const auto c = studiocast::maxine::BuildCanonicalMaxineBlockedCopy(
                    d, studiocast::maxine::MaxineNeed::vfx);
                expectEq("maxine_copy vfx_missing summary", c.summary,
                         "Maxine unavailable: VFX SDK not found (expected: ~/.local/share/studiocast/maxine/VideoFX or /usr/local/VideoFX).");
                const std::string s = studiocast::maxine::FormatCanonicalMaxineBlockedCopy(c);
                expectContains("maxine_copy vfx_missing has libnvvfx hint", s, "Ensure `libnvvfx.so` is under `<VFX_ROOT>/lib/`");
            }

            // 4) AR SDK missing.
            {
                auto d = mkDiag();
                d.ar.root_source = "xdg";
                d.ar.root_exists = false;
                d.ar.library_exists = false;
                d.ar.candidate_roots.push_back(
                    "/home/studiocast_selftest_home/.local/share/studiocast/maxine/ARSDK");

                const auto c = studiocast::maxine::BuildCanonicalMaxineBlockedCopy(
                    d, studiocast::maxine::MaxineNeed::ar);
                expectEq("maxine_copy ar_missing summary", c.summary,
                         "Maxine unavailable: AR SDK not found (needed for Eye Contact / Auto Frame).");
                const std::string s = studiocast::maxine::FormatCanonicalMaxineBlockedCopy(c);
                expectContains("maxine_copy ar_missing shows expected root", s,
                               "Expected AR SDK root: ~/.local/share/studiocast/maxine/ARSDK or /usr/local/ARSDK.");
            }

            // 4b) AFX SDK missing.
            {
                auto d = mkDiag();
                d.afx.root_source = "xdg";
                d.afx.root_exists = false;
                d.afx.library_exists = false;
                d.afx.candidate_roots.push_back(
                    "/home/studiocast_selftest_home/.local/share/studiocast/maxine/Audio_Effects_SDK");

                const auto c = studiocast::maxine::BuildCanonicalMaxineBlockedCopy(
                    d, studiocast::maxine::MaxineNeed::afx);
                expectEq("maxine_copy afx_missing summary", c.summary,
                         "Maxine unavailable: Audio Effects SDK not found (needed for audio effects).");
                const std::string s = studiocast::maxine::FormatCanonicalMaxineBlockedCopy(c);
                expectContains("maxine_copy afx_missing shows expected root", s,
                               "Expected AFX SDK root: ~/.local/share/studiocast/maxine/Audio_Effects_SDK or /usr/local/Audio_Effects_SDK.");
            }

            // 5) Features missing.
            {
                auto d = mkDiag();
                d.vfx.ok = true;
                d.vfx.library_loadable = true;
                d.ar.ok = true;
                d.ar.library_loadable = true;
                const auto c = studiocast::maxine::BuildCanonicalMaxineBlockedCopy(d);
                expectEq("maxine_copy features summary", c.summary,
                         "Maxine unavailable: feature libraries not installed (run install_feature.sh).");
            }

            // 5b) AFX features missing.
            {
                auto d = mkDiag();
                d.afx.root_exists = true;
                d.afx.library_exists = true;
                d.afx.ok = true;
                d.afx.library_loadable = true;
                const auto c = studiocast::maxine::BuildCanonicalMaxineBlockedCopy(
                    d, studiocast::maxine::MaxineNeed::afx);
                expectEq("maxine_copy afx_features summary", c.summary,
                         "Maxine unavailable: Audio Effects features not installed (run download_features.sh).");
            }

            // Maxine per-effect availability oracle JSON contract.
            // GUI enable/disable decisions rely on `available_effects` + `missing_effects`.
            {
                studiocast::maxine::MaxineDiagnostics d;
                d.ok = true;
                d.supported = true;
                d.available_effects = {"auto_frame"};
                d.available_audio_effects = {"denoiser"};
                d.missing_effects["eye_contact"] = {studiocast::maxine::reasons::MissingArFeature("gaze_redirection")};
                d.missing_effects["noise_removal"] = {studiocast::maxine::reasons::MissingAfxFeature("denoiser")};

                const std::string js = d.ToJson();
                expectContains("maxine_tojson has available_effects", js,
                               "\"available_effects\":[\"auto_frame\"]");
                expectContains("maxine_tojson has available_audio_effects", js,
                               "\"available_audio_effects\":[\"denoiser\"]");
                expectContains("maxine_tojson has missing_effects", js,
                               "\"missing_effects\":{");
                expectContains("maxine_tojson has eye_contact reasons", js,
                               "\"eye_contact\":[\"missing_ar_feature:gaze_redirection\"]");
                expectContains("maxine_tojson has noise_removal reasons", js,
                               "\"noise_removal\":[\"missing_afx_feature:denoiser\"]");
                expectContains("maxine_tojson has components.afx", js,
                               "\"components\":{");
            }

            // Maxine gating decision helper: if a Maxine-backed effect is enabled but not
            // available, the pipeline must be blocked before starting.
            {
                studiocast::video::effects::BroadcastCameraEffects fx;
                fx.virtual_background.mode = studiocast::video::effects::VirtualBackgroundMode::blur;

                auto d = mkDiag();
                d.vfx.root_source = "xdg";
                d.vfx.root_exists = false;
                d.vfx.library_exists = false;
                d.vfx.candidate_roots.push_back(
                    "/home/studiocast_selftest_home/.local/share/studiocast/maxine/VideoFX");

                const auto gate = studiocast::video::effects::EvaluateMaxineGate(fx, d);
                expectTrue("maxine_gate blur blocked", !gate.ok);
                expectContains("maxine_gate blur blocked message", gate.message,
                               "Maxine unavailable: VFX SDK not found");

                d.vfx.root_exists = true;
                d.vfx.library_exists = true;
                d.vfx.ok = true;
                d.vfx.library_loadable = true;
                d.available_effects = {"virtual_background.blur"};
                const auto gate2 = studiocast::video::effects::EvaluateMaxineGate(fx, d);
                expectTrue("maxine_gate blur allowed when available", gate2.ok);
            }

            // AFX: Broadcast-equivalent microphone planning rules.
            {
                using studiocast::maxine::afx::PlanBroadcastMicrophoneEffect;

                const auto p0 = PlanBroadcastMicrophoneEffect(/*studio_voice_enabled=*/false,
                                                              /*noise_removal_enabled=*/false,
                                                              /*room_echo_removal_enabled=*/false,
                                                              /*strength=*/50);
                expectTrue("afx_plan none disabled", !p0.enabled);

                const auto p1 = PlanBroadcastMicrophoneEffect(/*studio_voice_enabled=*/true,
                                                              /*noise_removal_enabled=*/true,
                                                              /*room_echo_removal_enabled=*/true,
                                                              /*strength=*/50);
                expectTrue("afx_plan studio_voice enabled", p1.enabled);
                expectEq("afx_plan studio_voice selector", p1.effect_selector, "studio_voice_low_latency");
                expectEq("afx_plan studio_voice feature_id", p1.feature_id, "studio_voice");

                const auto p2 = PlanBroadcastMicrophoneEffect(/*studio_voice_enabled=*/false,
                                                              /*noise_removal_enabled=*/true,
                                                              /*room_echo_removal_enabled=*/true,
                                                              /*strength=*/50);
                expectEq("afx_plan noise+echo selector", p2.effect_selector, "dereverb_denoiser");
                expectEq("afx_plan noise+echo feature_id", p2.feature_id, "dereverb_denoiser");

                const auto p3 = PlanBroadcastMicrophoneEffect(/*studio_voice_enabled=*/false,
                                                              /*noise_removal_enabled=*/true,
                                                              /*room_echo_removal_enabled=*/false,
                                                              /*strength=*/50);
                expectEq("afx_plan noise selector", p3.effect_selector, "denoiser");
                expectEq("afx_plan noise feature_id", p3.feature_id, "denoiser");

                const auto p4 = PlanBroadcastMicrophoneEffect(/*studio_voice_enabled=*/false,
                                                              /*noise_removal_enabled=*/false,
                                                              /*room_echo_removal_enabled=*/true,
                                                              /*strength=*/50);
                expectEq("afx_plan echo selector", p4.effect_selector, "dereverb");
                expectEq("afx_plan echo feature_id", p4.feature_id, "dereverb");
            }

            // Canonical audio effects planner: AEC + Superres validation / reason strings.
            {
                using studiocast::audio::effects::BroadcastAudioEffects;
                using studiocast::audio::effects::BroadcastAudioEffectsPlanInputs;
                using studiocast::audio::effects::PlanBroadcastAudioEffects;
                using studiocast::audio::effects::SuperresMode;

                BroadcastAudioEffects fx;
                BroadcastAudioEffectsPlanInputs in;
                in.available_pulse_sources = {"monitor0"};
                in.float32_pcm = true;

                fx.microphone.aec.enabled = true;
                fx.microphone.aec.reference_source.clear();
                const auto p0 = PlanBroadcastAudioEffects(fx, in);
                expectTrue("audio_plan aec missing reference_source disabled", !p0.microphone_aec.enabled);
                expectContains("audio_plan aec missing reference_source reason", p0.microphone_aec.reason, "reference_source is empty");

                fx.microphone.aec.reference_source = "monitor0";
                const auto p1 = PlanBroadcastAudioEffects(fx, in);
                expectTrue("audio_plan aec enabled when reference_source available", p1.microphone_aec.enabled);
                expectEq("audio_plan aec planned reference_source", p1.microphone_aec.reference_source, "monitor0");

                fx.microphone.aec.reference_source = "monitor_missing";
                const auto p2 = PlanBroadcastAudioEffects(fx, in);
                expectTrue("audio_plan aec disabled when reference_source unavailable", !p2.microphone_aec.enabled);
                expectContains("audio_plan aec unavailable reason", p2.microphone_aec.reason, "not available");

                fx.microphone.superres.enabled = true;
                fx.microphone.superres.mode = SuperresMode::k16kTo48k;
                in.float32_pcm = false;
                const auto p3 = PlanBroadcastAudioEffects(fx, in);
                expectTrue("audio_plan mic superres disabled when not float32", !p3.microphone_superres.enabled);
                expectContains("audio_plan mic superres not float32 reason", p3.microphone_superres.reason, "not float32");

                in.float32_pcm = true;
                const auto p4 = PlanBroadcastAudioEffects(fx, in);
                expectTrue("audio_plan mic superres enabled when float32", p4.microphone_superres.enabled);
                expectEq("audio_plan mic superres planned mode", std::string(studiocast::audio::effects::ToString(p4.microphone_superres.mode)),
                         "16k_to_48k");
            }

            // AFX: wrapper error messaging should be clean + actionable even without AFX installed.
            {
                studiocast::maxine::afx::AfxEffect e(nullptr);
                studiocast::maxine::afx::AfxEffectConfig cfg;
                cfg.effect_selector = "denoiser";
                cfg.feature_id = "denoiser";
                cfg.features_dir = "/nonexistent/afx/features";
                cfg.model_path = "/this/does/not/exist.trtpkg";

                std::string err;
                expectTrue("afx_effect missing model fails", !e.Configure(cfg, &err));
                expectContains("afx_effect missing model message", err, "AFX model file not found");
            }
            {
                studiocast::maxine::afx::AfxEffect e(nullptr);
                studiocast::maxine::afx::AfxEffectConfig cfg;
                cfg.effect_selector = "denoiser";
                cfg.feature_id = "denoiser";
                cfg.features_dir = "/nonexistent/afx/features";
                cfg.model_path = "/dev/null";

                std::string err;
                expectTrue("afx_effect missing feature lib fails", !e.Configure(cfg, &err));
                expectContains("afx_effect missing feature lib message", err, "AFX feature library not found");
                expectContains("afx_effect missing feature lib mentions .so", err, "libnv_audiofx_denoiser.so");
            }

            // Optional AFX runtime smoke test: if AFX + GPU are present, ensure we can create+load+run.
            {
                const auto paths = studiocast::maxine::ResolveMaxinePaths();
                if (paths.afx.ok) {
                    studiocast::maxine::afx::AfxApi api;
                    std::string apiErr;
                    if (api.InitializeFromLibraryPath(paths.afx.library, &apiErr)) {
                        const auto sel = studiocast::maxine::SelectGpu(studiocast::config::GpuSelection{});
                        if (sel.selected && sel.selected->compute_capability) {
                            studiocast::maxine::afx::AfxEffect fx(&api);
                            studiocast::maxine::afx::AfxEffectConfig cfg;
                            cfg.effect_selector = "denoiser";
                            cfg.feature_id = "denoiser";
                            cfg.features_dir = paths.afx.features_dir;
                            cfg.compute_capability = sel.selected->compute_capability;
                            cfg.sample_rate = 48000;
                            cfg.frame_samples = 480;
                            cfg.channels = 1;
                            cfg.intensity = 0.5f;

                            std::string err;
                            if (!fx.Configure(cfg, &err)) {
                                ++failures;
                                std::printf("[FAIL] afx_effect Configure (runtime)\n  error: %s\n", err.c_str());
                            } else if (!fx.Load(&err)) {
                                ++failures;
                                std::printf("[FAIL] afx_effect Load (runtime)\n  error: %s\n", err.c_str());
                            } else {
                                std::vector<float> in(cfg.frame_samples * cfg.channels);
                                std::vector<float> out(in.size());
                                if (!in.empty()) {
                                    in[0] = 1.0f;
                                }
                                if (!fx.Run(in.data(), out.data(), static_cast<std::uint32_t>(in.size()), &err)) {
                                    ++failures;
                                    std::printf("[FAIL] afx_effect Run (runtime)\n  error: %s\n", err.c_str());
                                }
                            }
                        }
                    }
                }
            }

            // Restore env.
            if (oldHome) {
                ::setenv("HOME", oldHomeStr.c_str(), 1);
            } else {
                ::unsetenv("HOME");
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
