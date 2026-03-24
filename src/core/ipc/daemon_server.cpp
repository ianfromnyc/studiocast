#include "daemon_server.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <sstream>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

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

} // namespace

DaemonServer::~DaemonServer() { Stop(); }

bool DaemonServer::Start(const std::filesystem::path &socket_path,
                         Handler handler, std::string *error) {
  Stop();

  if (!handler) {
    if (error)
      *error = "DaemonServer handler is null";
    return false;
  }

  const std::string pathStr = socket_path.string();
  if (pathStr.empty()) {
    if (error)
      *error = "socket_path is empty";
    return false;
  }

  if (pathStr.size() >= sizeof(sockaddr_un::sun_path)) {
    if (error)
      *error = "Socket path too long: " + pathStr;
    return false;
  }

  // Remove stale socket file.
  (void)::unlink(pathStr.c_str());

  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    if (error)
      *error = std::string("socket() failed: ") + std::strerror(errno);
    return false;
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, pathStr.c_str(), sizeof(addr.sun_path) - 1);

  if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    const int e = errno;
    ::close(fd);
    if (error) {
      std::ostringstream oss;
      oss << "bind failed: " << std::strerror(e) << " (" << pathStr << ")";
      *error = oss.str();
    }
    return false;
  }

  // Best-effort: restrict permissions.
  (void)::chmod(pathStr.c_str(), 0600);

  if (::listen(fd, 16) != 0) {
    const int e = errno;
    ::close(fd);
    if (error) {
      std::ostringstream oss;
      oss << "listen failed: " << std::strerror(e);
      *error = oss.str();
    }
    return false;
  }

  listen_fd_ = fd;
  socket_path_ = socket_path;
  handler_ = std::move(handler);
  stop_.store(false);
  running_.store(true);

  accept_th_ = std::thread(&DaemonServer::AcceptThread, this);
  return true;
}

void DaemonServer::Stop() {
  stop_.store(true);

  int listenFd = -1;
  {
    listenFd = listen_fd_;
    listen_fd_ = -1;
  }
  if (listenFd >= 0) {
    ::shutdown(listenFd, SHUT_RDWR);
    ::close(listenFd);
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    for (int fd : client_fds_) {
      if (fd >= 0) {
        ::shutdown(fd, SHUT_RDWR);
        ::close(fd);
      }
    }
    client_fds_.clear();
  }

  if (accept_th_.joinable())
    accept_th_.join();

  {
    std::lock_guard<std::mutex> lock(mu_);
    for (auto &t : client_threads_) {
      if (t.joinable())
        t.join();
    }
    client_threads_.clear();
  }

  if (!socket_path_.empty()) {
    (void)::unlink(socket_path_.string().c_str());
  }

  socket_path_.clear();
  handler_ = nullptr;
  running_.store(false);
  stop_.store(false);
}

void DaemonServer::AcceptThread() {
  while (!stop_.load()) {
    const int fd = ::accept4(listen_fd_, nullptr, nullptr, SOCK_CLOEXEC);
    if (fd < 0) {
      if (errno == EINTR)
        continue;
      if (stop_.load())
        break;
      // If listen fd was closed, accept will fail.
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      continue;
    }

    {
      std::lock_guard<std::mutex> lock(mu_);
      client_fds_.push_back(fd);
      client_threads_.emplace_back(&DaemonServer::ClientThread, this, fd);
    }
  }
}

void DaemonServer::ClientThread(int fd) {
  auto removeFd = [&]() {
    std::lock_guard<std::mutex> lock(mu_);
    auto it = std::find(client_fds_.begin(), client_fds_.end(), fd);
    if (it != client_fds_.end())
      client_fds_.erase(it);
  };

  std::string buffer;
  buffer.reserve(4096);

  char tmp[4096];

  while (!stop_.load()) {
    const ssize_t r = ::read(fd, tmp, sizeof(tmp));
    if (r < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    if (r == 0)
      break;

    buffer.append(tmp, tmp + r);

    for (;;) {
      const auto pos = buffer.find('\n');
      if (pos == std::string::npos)
        break;

      std::string line = buffer.substr(0, pos);
      buffer.erase(0, pos + 1);

      // Ignore empty lines.
      if (line.empty())
        continue;

      std::string reply;
      try {
        reply = handler_ ? handler_(line)
                         : std::string("ERR {\"error\":\"no_handler\"}");
      } catch (...) {
        reply = "ERR {\"error\":\"exception\"}";
      }

      reply.push_back('\n');

      std::string werr;
      if (!WriteAll(fd, reply.data(), reply.size(), &werr)) {
        break;
      }
    }
  }

  ::close(fd);
  removeFd();
}

} // namespace studiocast::ipc
