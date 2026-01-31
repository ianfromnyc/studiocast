#pragma once

#include <optional>
#include <string>
#include <vector>

namespace studiocast::probe {

    struct Version {
        int major = 0;
        int minor = 0;
        int patch = 0;
        bool has_patch = false;
        std::string original;
    };

    struct GpuInfo {
        std::string name;
        std::optional<std::string> compute_cap;  // e.g. "8.6"
    };

    struct CheckResult {
        std::string name;
        bool ok = false;
        bool skipped = false;
        std::string details;
    };

    struct Report {
        std::string app_version;
        std::string app_git_sha;

        std::string os_pretty_name;
        std::string kernel;

        std::optional<Version> nvidia_driver;
        std::vector<GpuInfo> gpus;

        std::vector<CheckResult> checks;
        std::vector<std::string> notes;

        bool AllChecksPassed() const;

        std::string ToText() const;
        std::string ToJson() const;
    };

    Report Run(bool verbose);

}  // namespace studiocast::probe
