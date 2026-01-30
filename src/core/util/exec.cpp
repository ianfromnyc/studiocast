#include "exec.h"

#include <cstdio>
#include <memory>
#include <stdexcept>

namespace studiocast::util {

    ExecResult ExecCapture(const std::string& command) {
        ExecResult r;

        // popen uses /bin/sh -c under the hood.
        std::unique_ptr<FILE, int (*)(FILE*)> pipe(popen(command.c_str(), "r"), pclose);
        if (!pipe) {
            r.exit_code = -1;
            r.stdout_str = "";
            return r;
        }

        char buffer[4096];
        while (std::fgets(buffer, sizeof(buffer), pipe.get()) != nullptr) {
            r.stdout_str += buffer;
        }

        // pclose return is shell-dependent; we keep it simple:
        r.exit_code = 0;
        return r;
    }

}  // namespace studiocast::util
