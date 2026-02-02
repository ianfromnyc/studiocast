#include "daemon_socket.h"

#include <system_error>

#include "core/util/xdg.h"

namespace fs = std::filesystem;

namespace studiocast::ipc {

std::filesystem::path DaemonSocketPath(std::string* error) {
  const auto dir = studiocast::util::StudioCastRuntimeDir();
  if (dir.empty()) {
    if (error) *error = "StudioCastRuntimeDir() is empty (XDG_RUNTIME_DIR not available).";
    return {};
  }

  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) {
    if (error) *error = "Failed to create runtime dir: " + ec.message();
    return {};
  }

  return dir / "studiocastd.sock";
}

}  // namespace studiocast::ipc
