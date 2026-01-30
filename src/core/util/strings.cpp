#include "strings.h"

#include <cctype>

namespace studiocast::util {

    std::string TrimCopy(const std::string& s) {
        size_t b = 0;
        while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;

        size_t e = s.size();
        while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;

        return s.substr(b, e - b);
    }

    std::vector<std::string> Split(const std::string& s, char delim) {
        std::vector<std::string> out;
        std::string cur;
        for (char c : s) {
            if (c == delim) {
                out.push_back(cur);
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
        out.push_back(cur);
        return out;
    }

    std::vector<std::string> SplitLines(const std::string& s) {
        std::vector<std::string> out;
        std::string cur;
        for (char c : s) {
            if (c == '\n') {
                out.push_back(cur);
                cur.clear();
            } else if (c != '\r') {
                cur.push_back(c);
            }
        }
        if (!cur.empty()) out.push_back(cur);
        return out;
    }

    std::string FirstNonEmptyLine(const std::string& s) {
        for (const auto& line : SplitLines(s)) {
            auto t = TrimCopy(line);
            if (!t.empty()) return t;
        }
        return "";
    }

}  // namespace studiocast::util
