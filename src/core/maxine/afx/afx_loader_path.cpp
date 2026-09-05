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

// The link to the running program. It always works, but a program started
// through it takes `exe` as its process name.
constexpr const char *kSelfExeLink = "/proc/self/exe";

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
    // The loader splits LD_LIBRARY_PATH on ':', so a directory whose name
    // holds one can never be found on the path it was just added to. Adding it
    // would make the "already there" test above fail forever, and every start
    // would end with one wasted restart.
    if (s.find(':') != std::string::npos)
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

fs::path ExecPathForRestart(const fs::path &self_exe_target) {
  const std::string target = self_exe_target.string();
  if (target.empty() || target.front() != '/')
    return fs::path(kSelfExeLink);

  // The kernel adds this when the program file is no longer there. The link
  // still opens the running program, so keep it in that case.
  const std::string kDeleted = " (deleted)";
  if (target.size() > kDeleted.size() &&
      target.compare(target.size() - kDeleted.size(), kDeleted.size(),
                     kDeleted) == 0) {
    return fs::path(kSelfExeLink);
  }

  return self_exe_target;
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

  // A directory whose name holds a ':' is left out of the loader path, so say
  // so: the effects it holds cannot load until it is renamed.
  std::string unusable;
  for (const auto &d : dirs) {
    const std::string s = d.string();
    if (s.find(':') == std::string::npos)
      continue;
    if (!unusable.empty())
      unusable += ", ";
    unusable += s;
  }
  auto add_unusable_note = [&unusable](std::string *out) {
    if (!out || unusable.empty())
      return;
    if (!out->empty())
      *out += " ";
    *out += "These AFX feature directories cannot go on the loader path, "
            "because the loader splits it on ':' and their names hold one: " +
            unusable + ". Rename them.";
  };

  const char *current = std::getenv("LD_LIBRARY_PATH");
  const auto next = LdLibraryPathWithDirs(
      current ? std::string(current) : std::string(), dirs);
  if (!next) {
    add_unusable_note(note);
    return;
  }

  if (std::getenv(kReexecGuard)) {
    if (note) {
      *note = "AFX feature libraries are still not on the loader path after a "
              "restart. Effects that need them cannot load.";
    }
    add_unusable_note(note);
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
  // Start the real program file, not the `/proc/self/exe` link, so that the
  // process keeps its name for `pgrep`, `pkill` and the process views.
  std::error_code ec;
  const fs::path self = fs::read_symlink(kSelfExeLink, ec);
  const fs::path program = ExecPathForRestart(ec ? fs::path() : self);
  ::execv(program.c_str(), argv);

  if (note) {
    *note = std::string("Failed to restart with the AFX feature libraries on "
                        "the loader path: ") +
            std::strerror(errno);
  }
}

} // namespace studiocast::maxine::afx
