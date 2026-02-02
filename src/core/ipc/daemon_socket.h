#pragma once

#include <filesystem>
#include <string>

namespace studiocast::ipc {

// Returns the default Unix socket path for studiocastd.
//
// Path: $XDG_RUNTIME_DIR/studiocast/studiocastd.sock (preferred)
// Fallback: /tmp/studiocast-runtime-$UID/studiocast/studiocastd.sock
//
// The parent directory is created if necessary.
std::filesystem::path DaemonSocketPath(std::string* error);

}  // namespace studiocast::ipc
