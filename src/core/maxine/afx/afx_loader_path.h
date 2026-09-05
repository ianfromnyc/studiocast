#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace studiocast::maxine::afx {

namespace fs = std::filesystem;

// The `<AFX root>/features/<feature>/lib` directories that exist.
//
// The AFX core loads a feature library by its bare name, so these directories
// must be on the loader path of the process that runs the effect.
std::vector<fs::path> AfxFeatureLibDirs(const fs::path &features_dir);

// True when the loader can find `dir` on LD_LIBRARY_PATH.
//
// The loader splits LD_LIBRARY_PATH on ':', so a directory whose name holds
// one can never be found on the path it was added to. Every place that puts a
// directory on the loader path, or that tells the user why a directory is not
// on it, asks this one function, so the answers cannot drift apart.
bool CanGoOnLoaderPath(const fs::path &dir);

// The value that LD_LIBRARY_PATH needs so that the loader finds `dirs`. The
// directories that are missing go in front, and `current` stays after them.
// Returns nothing when `current` already holds every directory.
std::optional<std::string>
LdLibraryPathWithDirs(const std::string &current,
                      const std::vector<fs::path> &dirs);

// The program to start again, given what `/proc/self/exe` points at (an empty
// path when that could not be read).
//
// A restart through `/proc/self/exe` makes the kernel set the process name to
// `exe`, so `pgrep -x studiocastd` and the process views no longer find the
// daemon. The real path keeps the name. The link stays the answer when the
// target is unknown, is not absolute, or names a program file that is gone.
fs::path ExecPathForRestart(const fs::path &self_exe_target);

// Stand-ins the tests put in place of the calls this file makes. A member that
// is null keeps the real call, which is what every program uses.
struct AfxLoaderPathHooks {
  // execv(3). A test uses it to see what happens when the restart fails.
  int (*exec)(const char *path, char *const argv[]) = nullptr;
};

// Puts the AFX feature library directories on the loader path.
//
// glibc reads LD_LIBRARY_PATH once, at start, so a program can only change
// where dlopen looks by starting again. This sets LD_LIBRARY_PATH and re-execs
// the program with the same arguments. It does nothing, and returns, when
// there is no AFX install, when the path already holds the directories, or
// when this process is itself the result of such a re-exec.
//
// A restart that fails leaves the environment as it was, so the children this
// process starts later are not given the feature directories, and a later call
// does not read the guard as a restart that happened.
//
// `note` receives a line worth logging, and stays empty when nothing happened.
void EnsureAfxFeatureLibsOnLoaderPath(char **argv, std::string *note,
                                      const AfxLoaderPathHooks &hooks = {});

} // namespace studiocast::maxine::afx
