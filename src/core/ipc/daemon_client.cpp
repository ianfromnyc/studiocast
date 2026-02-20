#include "daemon_client.h"

#include <cerrno>
#include <cstring>
#include <sstream>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "core/ipc/daemon_socket.h"

namespace studiocast::ipc {
namespace {

bool WriteAll(int fd, const void *data, std::size_t bytes, std::string *error) {
  const char *p = static_cast<const char *>(data);
  std::size_t n = 0;
  while (n < bytes) {
    const ssize_t w = ::write(fd, p + n, bytes - n);
    if (w < 0) {
      if (errno == EINTR)
        continue;
      if (error)
        *error = std::string("write failed: ") + std::strerror(errno);
      return false;
    }
    if (w == 0) {
      if (error)
        *error = "write returned 0";
      return false;
    }
    n += static_cast<std::size_t>(w);
  }
  return true;
}

bool ReadLine(int fd, std::string *line, std::string *error) {
  if (!line)
    return false;
  line->clear();

  constexpr std::size_t kMax = 1024 * 1024; // 1MB safety cap
  char buf[4096];

  while (line->size() < kMax) {
    const ssize_t r = ::read(fd, buf, sizeof(buf));
    if (r < 0) {
      if (errno == EINTR)
        continue;
      if (error)
        *error = std::string("read failed: ") + std::strerror(errno);
      return false;
    }
    if (r == 0) {
      if (error)
        *error = "connection closed";
      return false;
    }

    line->append(buf, buf + r);
    const auto pos = line->find('\n');
    if (pos != std::string::npos) {
      line->resize(
          pos); // strip newline and anything after (daemon sends 1 line)
      return true;
    }
  }

  if (error)
    *error = "response too large";
  return false;
}

} // namespace

bool DaemonCall(const std::string &request_line, DaemonCallResult *out,
                std::string *error) {
  if (out)
    *out = DaemonCallResult{};

  std::string pathErr;
  const auto sockPath = DaemonSocketPath(&pathErr);
  if (sockPath.empty()) {
    if (error)
      *error = pathErr.empty() ? "DaemonSocketPath() returned empty" : pathErr;
    return false;
  }

  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    if (error)
      *error = std::string("socket() failed: ") + std::strerror(errno);
    return false;
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  const std::string pathStr = sockPath.string();
  if (pathStr.size() >= sizeof(addr.sun_path)) {
    ::close(fd);
    if (error)
      *error = "Socket path too long: " + pathStr;
    return false;
  }
  std::strncpy(addr.sun_path, pathStr.c_str(), sizeof(addr.sun_path) - 1);

  if (::connect(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    const int e = errno;
    ::close(fd);
    std::ostringstream oss;
    oss << "connect failed: " << std::strerror(e) << " (" << pathStr << ")";
    if (error)
      *error = oss.str();
    return false;
  }

  std::string wire = request_line;
  wire.push_back('\n');

  std::string werr;
  if (!WriteAll(fd, wire.data(), wire.size(), &werr)) {
    ::close(fd);
    if (error)
      *error = werr;
    return false;
  }

  std::string line;
  std::string rerr;
  if (!ReadLine(fd, &line, &rerr)) {
    ::close(fd);
    if (error)
      *error = rerr;
    return false;
  }

  ::close(fd);

  if (!out)
    return true;

  out->raw = line;
  if (line.rfind("OK", 0) == 0) {
    out->ok = true;
    if (line.size() > 2 && line[2] == ' ') {
      out->json = line.substr(3);
    } else {
      out->json = "{}";
    }
    return true;
  }

  if (line.rfind("ERR", 0) == 0) {
    out->ok = false;
    if (line.size() > 3 && line[3] == ' ') {
      out->error_json = line.substr(4);
    } else {
      out->error_json = "{\"error\":\"unknown\"}";
    }
    return true;
  }

  // Unexpected reply
  out->ok = false;
  out->error_json = "{\"error\":\"bad_reply\"}";
  if (error)
    *error = "Unexpected reply from daemon: " + line;
  return false;
}

} // namespace studiocast::ipc
