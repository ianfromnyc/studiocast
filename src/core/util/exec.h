#pragma once

#include <cstddef>
#include <string>

namespace studiocast::util {

struct ExecResult {
  int exit_code = -1;
  bool timed_out = false;
  std::string stdout_str;
};

struct ExecCaptureOptions {
  int timeout_ms = 5000;
  std::size_t max_output_bytes = 1024 * 1024;
};

ExecResult ExecCapture(const std::string &command);
ExecResult ExecCapture(const std::string &command,
                       const ExecCaptureOptions &options);

} // namespace studiocast::util
