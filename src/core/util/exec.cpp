#include "exec.h"

#include <cstdio>
#include <sys/wait.h>

namespace studiocast::util {

    ExecResult ExecCapture(const std::string& command) {
        ExecResult r;

        FILE* pipe = popen(command.c_str(), "r");
        if (!pipe) {
            r.exit_code = -1;
            return r;
        }

        char buffer[4096];
        while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            r.stdout_str += buffer;
        }

        const int status = pclose(pipe);
        if (status == -1) {
            r.exit_code = -1;
        } else if (WIFEXITED(status)) {
            r.exit_code = WEXITSTATUS(status);
        } else {
            r.exit_code = -1;
        }

        return r;
    }

}  // namespace studiocast::util
