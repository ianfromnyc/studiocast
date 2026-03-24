#include "proc.h"

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace fs = std::filesystem;

namespace studiocast::util {
namespace {

struct FileId {
  bool is_char_device = false;
  dev_t rdev = 0;
  dev_t dev = 0;
  ino_t ino = 0;
};

bool GetFileId(const fs::path &p, FileId *out, std::string *error) {
  if (!out)
    return false;

  struct stat st {};
  if (::stat(p.c_str(), &st) != 0) {
    if (error) {
      *error = "stat(" + p.string() +
               ") failed: " + std::string(std::strerror(errno));
    }
    return false;
  }

  out->is_char_device = S_ISCHR(st.st_mode);
  out->rdev = st.st_rdev;
  out->dev = st.st_dev;
  out->ino = st.st_ino;
  return true;
}

bool Matches(const FileId &id, const struct stat &st) {
  if (id.is_char_device) {
    return S_ISCHR(st.st_mode) && st.st_rdev == id.rdev;
  }
  return st.st_dev == id.dev && st.st_ino == id.ino;
}

bool ParsePid(const std::string &name, int *pid) {
  if (!pid)
    return false;
  if (name.empty())
    return false;
  for (char c : name) {
    if (c < '0' || c > '9')
      return false;
  }
  *pid = std::atoi(name.c_str());
  return *pid > 0;
}

} // namespace

std::string ProcessNameFromPid(int pid) {
  if (pid <= 0)
    return {};

  std::ostringstream path;
  path << "/proc/" << pid << "/comm";

  std::ifstream f(path.str());
  if (!f.is_open())
    return {};

  std::string name;
  std::getline(f, name);
  // /proc/<pid>/comm includes a trailing newline; getline strips it.
  return name;
}

std::vector<int> PidsWithOpenFile(const fs::path &target,
                                  const OpenFileScanOptions &opt,
                                  std::string *error) {
  if (error)
    error->clear();

  FileId targetId{};
  std::string idErr;
  if (!GetFileId(target, &targetId, &idErr)) {
    if (error)
      *error = idErr;
    return {};
  }

  std::vector<int> results;

  std::error_code procEc;
  for (fs::directory_iterator it("/proc", procEc);
       it != fs::directory_iterator(); it.increment(procEc)) {
    if (procEc) {
      // /proc iteration failures shouldn't be fatal; just stop.
      if (error && error->empty())
        *error = "Failed to iterate /proc: " + procEc.message();
      break;
    }

    const fs::directory_entry &ent = *it;
    if (!ent.is_directory(procEc))
      continue;

    const std::string base = ent.path().filename().string();
    int pid = -1;
    if (!ParsePid(base, &pid))
      continue;
    if (opt.exclude_pid > 0 && pid == opt.exclude_pid)
      continue;

    const fs::path fdDir = ent.path() / "fd";

    std::error_code fdEc;
    for (fs::directory_iterator fdit(fdDir, fdEc);
         fdit != fs::directory_iterator(); fdit.increment(fdEc)) {
      if (fdEc) {
        // common if we lack permissions; ignore
        break;
      }

      const fs::path fdPath = fdit->path();

      struct stat st {};
      if (::stat(fdPath.c_str(), &st) != 0) {
        continue;
      }

      if (Matches(targetId, st)) {
        results.push_back(pid);
        break;
      }
    }

    if (opt.stop_at_first && !results.empty()) {
      return results;
    }
  }

  return results;
}

bool AnyOtherProcessHasFileOpen(const fs::path &target, int exclude_pid,
                                std::string *error) {
  OpenFileScanOptions opt;
  opt.exclude_pid = exclude_pid;
  opt.stop_at_first = true;

  const auto pids = PidsWithOpenFile(target, opt, error);
  return !pids.empty();
}

} // namespace studiocast::util
