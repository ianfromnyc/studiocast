#pragma once

#include <optional>
#include <string>

namespace studiocast::maxine {

    // Returns the recommended value for VFX/AR install_feature.sh's `--gpu` flag.
    // Example: "7.5" -> "t4", "8.6" -> "a10".
    std::optional<std::string> MaxineGpuArgFromComputeCap(const std::string& compute_cap);

}  // namespace studiocast::maxine
