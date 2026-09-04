#include "core/maxine/afx/afx_loader_path.h"

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <unistd.h>

#include "core/maxine/paths.h"

namespace studiocast::maxine::afx {

namespace {

// Set in the process that a re-exec starts, so it happens one time only.
constexpr const char *kReexecGuard = "STUDIOCAST_AFX_LOADER_PATH_REEXEC";

std::vector<std::string> SplitPathList(const std::string &value) {
  std::vector<std::string> out;
  std::string token;
  std::istringstream in(value);
  while (std::getline(in, token, ':')) {
    if (!token.empty())
      out.push_back(token);
  }
  return out;
}

} // namespace

std::vector<fs::path> AfxFeatureLibDirs(const fs::path &features_dir) {
  std::vector<fs::path> out;
  if (features_dir.empty())
    return out;

  std::error_code ec;
  if (!fs::is_directory(features_dir, ec))
    return out;

  fs::directory_iterator it(features_dir, ec);
  if (ec)
    return out;

  // Each test keeps its own error_code: a path that is not there sets one, and
  // a shared code would stop the walk at the first feature without a `lib`.
  for (const auto &entry : it) {
    std::error_code entry_ec;
    if (!entry.is_directory(entry_ec))
      continue;
    const fs::path lib = entry.path() / "lib";
    std::error_code lib_ec;
    if (fs::is_directory(lib, lib_ec))
      out.push_back(lib);
  }

  // The order of a directory listing is not defined; keep it stable.
  std::sort(out.begin(), out.end());
  return out;
}

std::optional<std::string>
LdLibraryPathWithDirs(const std::string &current,
                      const std::vector<fs::path> &dirs) {
  if (dirs.empty())
    return std::nullopt;

  const std::vector<std::string> have = SplitPathList(current);
  auto already_there = [&have](const std::string &dir) {
    return std::find(have.begin(), have.end(), dir) != have.end();
  };

  std::vector<std::string> missing;
  for (const auto &dir : dirs) {
    const std::string s = dir.string();
    if (s.empty() || already_there(s))
      continue;
    if (std::find(missing.begin(), missing.end(), s) == missing.end())
      missing.push_back(s);
  }
  if (missing.empty())
    return std::nullopt;

  std::string out;
  for (const auto &m : missing) {
    if (!out.empty())
      out += ":";
    out += m;
  }
  if (!current.empty()) {
    out += ":";
    out += current;
  }
  return out;
}

void EnsureAfxFeatureLibsOnLoaderPath(char **argv, std::string *note) {
  if (note)
    note->clear();
  if (!argv || !argv[0])
    return;

  const auto paths = studiocast::maxine::ResolveMaxinePaths();
  const auto dirs = AfxFeatureLibDirs(paths.afx.features_dir);
  if (dirs.empty())
    return;

  const char *current = std::getenv("LD_LIBRARY_PATH");
  const auto next = LdLibraryPathWithDirs(
      current ? std::string(current) : std::string(), dirs);
  if (!next)
    return;

  if (std::getenv(kReexecGuard)) {
    if (note) {
      *note = "AFX feature libraries are still not on the loader path after a "
              "restart. Effects that need them cannot load.";
    }
    return;
  }

  if (::setenv("LD_LIBRARY_PATH", next->c_str(), 1) != 0 ||
      ::setenv(kReexecGuard, "1", 1) != 0) {
    if (note) {
      *note = std::string("Failed to set LD_LIBRARY_PATH for the AFX feature "
                          "libraries: ") +
              std::strerror(errno);
    }
    return;
  }

  // glibc reads LD_LIBRARY_PATH at start, so start again with the new value.
  ::execv("/proc/self/exe", argv);

  if (note) {
    *note = std::string("Failed to restart with the AFX feature libraries on "
                        "the loader path: ") +
            std::strerror(errno);
  }
}

} // namespace studiocast::maxine::afx
