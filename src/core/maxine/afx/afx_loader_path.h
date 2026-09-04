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

// Puts the AFX feature library directories on the loader path.
//
// glibc reads LD_LIBRARY_PATH once, at start, so a program can only change
// where dlopen looks by starting again. This sets LD_LIBRARY_PATH and re-execs
// the program with the same arguments. It does nothing, and returns, when
// there is no AFX install, when the path already holds the directories, or
// when this process is itself the result of such a re-exec.
//
// `note` receives a line worth logging, and stays empty when nothing happened.
void EnsureAfxFeatureLibsOnLoaderPath(char **argv, std::string *note);

} // namespace studiocast::maxine::afx
