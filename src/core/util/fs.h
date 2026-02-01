#pragma once

#include <optional>
#include <string>

namespace studiocast::util {

std::optional<std::string> ReadTextFile(const std::string &path);

} // namespace studiocast::util
