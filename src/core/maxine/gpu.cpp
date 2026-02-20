#include "gpu.h"

#include <cstdlib>

#include "core/util/strings.h"

namespace studiocast::maxine {
namespace {

std::optional<std::pair<int, int>> ParseComputeCap(const std::string &s) {
  const auto t = studiocast::util::TrimCopy(s);
  const auto parts = studiocast::util::Split(t, '.');
  if (parts.size() < 2)
    return std::nullopt;

  const int major = std::atoi(parts[0].c_str());
  const int minor = std::atoi(parts[1].c_str());
  return std::make_pair(major, minor);
}

} // namespace

std::optional<std::string>
MaxineGpuArgFromComputeCap(const std::string &compute_cap) {
  auto cc = ParseComputeCap(compute_cap);
  if (!cc)
    return std::nullopt;

  const auto [maj, min] = *cc;

  // Based on the VFX/AR install docs’ CC mapping table.
  if (maj == 7 && min == 5)
    return "t4";
  if (maj == 8 && min == 0)
    return "a100";
  if (maj == 8 && min == 6)
    return "a10";
  if (maj == 8 && min == 9)
    return "l4";
  if (maj == 9 && min == 0)
    return "h100";
  if (maj == 10 && min == 0)
    return "b100";
  if (maj == 12 && min == 0)
    return "b40";

  return std::nullopt;
}

} // namespace studiocast::maxine
