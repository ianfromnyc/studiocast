#pragma once

#include <string>
#include <vector>

namespace studiocast::util {

std::string TrimCopy(const std::string &s);

std::vector<std::string> Split(const std::string &s, char delim);
std::vector<std::string> SplitLines(const std::string &s);

std::string FirstNonEmptyLine(const std::string &s);

} // namespace studiocast::util
