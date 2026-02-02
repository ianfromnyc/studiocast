#pragma once

#include <string>

namespace studiocast::util {

    struct ExecResult {
        int exit_code = -1;
        std::string stdout_str;
    };

    ExecResult ExecCapture(const std::string& command);

}  // namespace studiocast::util
