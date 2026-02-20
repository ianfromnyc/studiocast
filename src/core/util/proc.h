#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace studiocast::util {

struct OpenFileScanOptions {
  // PID to exclude from results (e.g., the daemon's own PID).
  int exclude_pid = -1;

  // If true, return as soon as a match is found.
  bool stop_at_first = false;
};

// Returns the PIDs (best-effort) that currently have an open fd referring to
// the same underlying file/device node as `target`.
//
// This scans /proc/*/fd and may be limited by permissions (we ignore processes
// we can't inspect).
std::vector<int> PidsWithOpenFile(const std::filesystem::path &target,
                                  const OpenFileScanOptions &opt,
                                  std::string *error);

// Convenience helper: true if any process other than `exclude_pid` has `target`
// open.
bool AnyOtherProcessHasFileOpen(const std::filesystem::path &target,
                                int exclude_pid, std::string *error);

} // namespace studiocast::util
