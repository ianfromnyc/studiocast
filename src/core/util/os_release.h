#pragma once

#include <string>

namespace studiocast::util {

    // Best-effort: returns PRETTY_NAME from /etc/os-release, or empty string.
    std::string ReadOsPrettyName();

}  // namespace studiocast::util
