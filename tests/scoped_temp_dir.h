#pragma once

// A temporary directory that goes away at the end of the scope. The CMake
// configure tests each need a throwaway tree, so the helper lives here rather
// than once per test file. mkdtemp makes the name, so two helpers with one
// prefix never name the same directory and nobody else can create it first.

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <cstdlib>

namespace studiocast::tests {

class ScopedTempDir {
public:
  explicit ScopedTempDir(const std::string &prefix) {
    std::error_code ec;
    const std::filesystem::path base = std::filesystem::temp_directory_path(ec);
    if (ec) {
      error_ = "temp_directory_path failed: " + ec.message();
      return;
    }

    // mkdtemp replaces the six X characters and creates the directory with
    // mode 0700, so the name is unique and the directory is ours.
    const std::string pattern = (base / (prefix + "-XXXXXX")).string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    if (::mkdtemp(buffer.data()) == nullptr) {
      error_ = "mkdtemp failed for " + pattern;
      return;
    }
    path_ = std::filesystem::path(buffer.data());
  }

  ~ScopedTempDir() {
    if (path_.empty())
      return;
    std::error_code ec;
    std::filesystem::remove_all(path_, ec);
  }

  ScopedTempDir(const ScopedTempDir &) = delete;
  ScopedTempDir &operator=(const ScopedTempDir &) = delete;

  bool ok() const { return error_.empty(); }
  const std::string &error() const { return error_; }
  const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
  std::string error_;
};

} // namespace studiocast::tests
