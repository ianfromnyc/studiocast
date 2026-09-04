#include "core/maxine/afx/afx_loader_path.h"

namespace studiocast::maxine::afx {

std::vector<fs::path> AfxFeatureLibDirs(const fs::path &) { return {}; }

std::optional<std::string>
LdLibraryPathWithDirs(const std::string &, const std::vector<fs::path> &) {
  return std::nullopt;
}

void EnsureAfxFeatureLibsOnLoaderPath(char **, std::string *) {}

} // namespace studiocast::maxine::afx
