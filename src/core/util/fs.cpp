#include "fs.h"

#include <fstream>
#include <sstream>

namespace studiocast::util {

    std::optional<std::string> ReadTextFile(const std::string& path) {
        std::ifstream f(path);
        if (!f.is_open()) return std::nullopt;

        std::ostringstream oss;
        oss << f.rdbuf();
        return oss.str();
    }

}  // namespace studiocast::util
