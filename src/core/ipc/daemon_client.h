#pragma once

#include <string>

namespace studiocast::ipc {

struct DaemonCallResult {
  bool ok = false;

  // Raw response line returned by daemon (without trailing newline).
  std::string raw;

  // JSON payload on success (may be "{}" if empty).
  std::string json;

  // JSON payload on error (typically {"error":"..."}).
  std::string error_json;
};

struct DaemonCallOptions {
  // Separate budgets keep stale sockets from blocking UI callers forever while
  // still allowing CLI/status callers to tolerate first-run diagnostics.
  int connect_timeout_ms = 1000;
  int io_timeout_ms = 10000;
};

// Sends a single request line to the studiocastd control socket and reads a
// single-line reply.
//
// Request format: one line WITHOUT a trailing newline (the client will append
// '\n'). Reply format: "OK <json>" or "ERR <json>" on a single line.
//
// Returns false on transport errors (unable to connect/read/write). In that
// case, *error is set.
bool DaemonCall(const std::string &request_line, DaemonCallResult *out,
                std::string *error);

bool DaemonCall(const std::string &request_line, DaemonCallResult *out,
                std::string *error, const DaemonCallOptions &options);

} // namespace studiocast::ipc
