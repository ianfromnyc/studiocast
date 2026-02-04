#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

#include "core/maxine/ar_api.h"
#include "core/maxine/nvcv_api.h"
#include "core/maxine/vfx_api.h"
#include "core/probe/probe.h"
#include "core/util/strings.h"
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
