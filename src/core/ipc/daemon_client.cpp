#include "daemon_client.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <sstream>

#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "core/ipc/daemon_socket.h"

namespace studiocast::ipc {
namespace {

using Clock = std::chrono::steady_clock;

int PositiveTimeoutOrFallback(int value, int fallback) {
  return value > 0 ? value : fallback;
}

int RemainingTimeoutMs(Clock::time_point deadline) {
  const auto now = Clock::now();
  if (now >= deadline)
    return 0;

  const auto remaining =
      std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
  return std::max(1, static_cast<int>(remaining.count()));
}

bool SetNonBlocking(int fd, std::string *error) {
  const int flags = ::fcntl(fd, F_GETFL, 0);
  if (flags < 0) {
    if (error)
      *error = std::string("fcntl(F_GETFL) failed: ") + std::strerror(errno);
    return false;
  }
  if (::fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) {
    if (error)
      *error = std::string("fcntl(F_SETFL O_NONBLOCK) failed: ") +
               std::strerror(errno);
    return false;
  }
  return true;
}

bool WaitFd(int fd, short events, int timeout_ms, const char *what,
            std::string *error) {
  pollfd pfd{};
  pfd.fd = fd;
  pfd.events = events;

  for (;;) {
    const int r = ::poll(&pfd, 1, timeout_ms);
    if (r > 0)
      return true;
    if (r == 0) {
      if (error) {
        std::ostringstream oss;
        oss << what << " timed out after " << timeout_ms << "ms";
        *error = oss.str();
      }
      return false;
    }
    if (errno == EINTR)
      continue;
    if (error)
      *error = std::string(what) + " poll failed: " + std::strerror(errno);
    return false;
  }
}

bool WaitFdUntil(int fd, short events, Clock::time_point deadline,
                 const char *what, std::string *error) {
  pollfd pfd{};
  pfd.fd = fd;
  pfd.events = events;

  for (;;) {
    const int timeoutMs = RemainingTimeoutMs(deadline);
    if (timeoutMs <= 0) {
      if (error)
        *error = std::string(what) + " timed out after total I/O deadline";
      return false;
    }

    const int r = ::poll(&pfd, 1, timeoutMs);
    if (r > 0)
      return true;
    if (r == 0) {
      if (error)
        *error = std::string(what) + " timed out after total I/O deadline";
      return false;
    }
    if (errno == EINTR)
      continue;
    if (error)
      *error = std::string(what) + " poll failed: " + std::strerror(errno);
    return false;
  }
}

bool FinishConnect(int fd, int timeout_ms, const std::string &path,
                   std::string *error) {
  std::string werr;
  if (!WaitFd(fd, POLLOUT, timeout_ms, "connect", &werr)) {
    if (error)
      *error = werr + " (" + path + ")";
    return false;
  }

  int soErr = 0;
  socklen_t soErrLen = sizeof(soErr);
  if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soErr, &soErrLen) != 0) {
    if (error)
      *error = std::string("getsockopt(SO_ERROR) failed: ") +
               std::strerror(errno) + " (" + path + ")";
    return false;
  }
  if (soErr != 0) {
    if (error)
      *error = std::string("connect failed: ") + std::strerror(soErr) + " (" +
               path + ")";
    return false;
  }
  return true;
}

bool WriteAll(int fd, const void *data, std::size_t bytes,
              Clock::time_point deadline, std::string *error) {
  const char *p = static_cast<const char *>(data);
  std::size_t n = 0;
  while (n < bytes) {
    std::string perr;
    if (!WaitFdUntil(fd, POLLOUT, deadline, "write", &perr)) {
      if (error)
        *error = perr;
      return false;
    }

    const ssize_t w = ::send(fd, p + n, bytes - n, MSG_NOSIGNAL);
    if (w < 0) {
      if (errno == EINTR)
        continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        continue;
      if (errno == EPIPE || errno == ECONNRESET) {
        if (error)
          *error = "connection closed during write";
        return false;
      }
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

bool ReadLine(int fd, std::string *line, Clock::time_point deadline,
              std::string *error) {
  if (!line)
    return false;
  line->clear();

  constexpr std::size_t kMax = 1024 * 1024; // 1MB safety cap
  char buf[4096];

  while (line->size() < kMax) {
    std::string perr;
    if (!WaitFdUntil(fd, POLLIN, deadline, "read", &perr)) {
      if (error)
        *error = perr;
      return false;
    }

    const ssize_t r = ::read(fd, buf, sizeof(buf));
    if (r < 0) {
      if (errno == EINTR)
        continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        continue;
      if (errno == EPIPE || errno == ECONNRESET) {
        if (error)
          *error = "connection closed during read";
        return false;
      }
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
  return DaemonCall(request_line, out, error, DaemonCallOptions{});
}

bool DaemonCall(const std::string &request_line, DaemonCallResult *out,
                std::string *error, const DaemonCallOptions &options) {
  if (out)
    *out = DaemonCallResult{};

  const int connectTimeoutMs =
      PositiveTimeoutOrFallback(options.connect_timeout_ms, 1000);
  const int ioTimeoutMs = PositiveTimeoutOrFallback(options.io_timeout_ms, 10000);

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

  std::string nberr;
  if (!SetNonBlocking(fd, &nberr)) {
    ::close(fd);
    if (error)
      *error = nberr;
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
    if (e == EINPROGRESS || e == EAGAIN || e == EALREADY) {
      std::string cerr;
      if (!FinishConnect(fd, connectTimeoutMs, pathStr, &cerr)) {
        ::close(fd);
        if (error)
          *error = cerr;
        return false;
      }
    } else {
      ::close(fd);
      std::ostringstream oss;
      oss << "connect failed: " << std::strerror(e) << " (" << pathStr << ")";
      if (error)
        *error = oss.str();
      return false;
    }
  }

  std::string wire = request_line;
  wire.push_back('\n');

  const auto ioDeadline =
      Clock::now() + std::chrono::milliseconds(ioTimeoutMs);

  std::string werr;
  if (!WriteAll(fd, wire.data(), wire.size(), ioDeadline, &werr)) {
    ::close(fd);
    if (error)
      *error = werr;
    return false;
  }

  std::string line;
  std::string rerr;
  if (!ReadLine(fd, &line, ioDeadline, &rerr)) {
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
