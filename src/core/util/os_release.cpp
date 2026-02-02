#include "os_release.h"

#include <map>
#include <string>

#include "core/util/fs.h"
#include "core/util/strings.h"

namespace studiocast::util {
    namespace {

        std::map<std::string, std::string> ParseOsRelease(const std::string& content) {
            std::map<std::string, std::string> kv;

            for (const auto& line : SplitLines(content)) {
                const auto t = TrimCopy(line);
                if (t.empty() || t[0] == '#') continue;

                const auto pos = t.find('=');
                if (pos == std::string::npos) continue;

                auto key = TrimCopy(t.substr(0, pos));
                auto val = TrimCopy(t.substr(pos + 1));

                // Strip optional quotes
                if (val.size() >= 2 && ((val.front() == '"' && val.back() == '"') || (val.front() == '\'' && val.back() == '\''))) {
                    val = val.substr(1, val.size() - 2);
                }

                kv[key] = val;
            }

            return kv;
        }

    }  // namespace

    std::string ReadOsPrettyName() {
        auto content = ReadTextFile("/etc/os-release");
        if (!content) return "";

        auto kv = ParseOsRelease(*content);
        auto it = kv.find("PRETTY_NAME");
        if (it == kv.end()) return "";
        return it->second;
    }

}  // namespace studiocast::util
