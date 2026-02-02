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

// Sends a single request line to the studiocastd control socket and reads a single-line reply.
//
// Request format: one line WITHOUT a trailing newline (the client will append '\n').
// Reply format: "OK <json>" or "ERR <json>" on a single line.
//
// Returns false on transport errors (unable to connect/read/write). In that case, *error is set.
bool DaemonCall(const std::string& request_line, DaemonCallResult* out, std::string* error);

}  // namespace studiocast::ipc
