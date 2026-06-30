#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <thread>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "core/ipc/daemon_client.h"
#include "core/ipc/daemon_server.h"
#include "core/ipc/daemon_socket.h"

namespace {

namespace fs = std::filesystem;
using namespace std::chrono_literals;

class ScopedRuntimeDir {
public:
  explicit ScopedRuntimeDir(const std::string &name) {
    had_old_ = std::getenv("XDG_RUNTIME_DIR") != nullptr;
    if (had_old_)
      old_ = std::getenv("XDG_RUNTIME_DIR");

    std::error_code ec;
    const auto tmp = fs::temp_directory_path(ec);
    if (ec) {
      error_ = "failed to resolve temp directory: " + ec.message();
      return;
    }

    dir_ = tmp /
           (name + "-" + std::to_string(static_cast<long long>(::getpid())));
    fs::remove_all(dir_, ec);
    fs::create_directories(dir_, ec);
    if (ec) {
      error_ = "failed to create temp runtime dir: " + ec.message();
      return;
    }

    if (::setenv("XDG_RUNTIME_DIR", dir_.string().c_str(), 1) != 0)
      error_ = std::string("setenv failed: ") + std::strerror(errno);
  }

  ~ScopedRuntimeDir() {
    if (had_old_) {
      (void)::setenv("XDG_RUNTIME_DIR", old_.c_str(), 1);
    } else {
      (void)::unsetenv("XDG_RUNTIME_DIR");
    }

    std::error_code ec;
    fs::remove_all(dir_, ec);
  }

  bool ok() const { return error_.empty(); }
  const std::string &error() const { return error_; }

private:
  fs::path dir_;
  bool had_old_ = false;
  std::string old_;
  std::string error_;
};

bool BindListeningSocket(const fs::path &path, int *listen_fd,
                         std::string *error) {
  if (!listen_fd)
    return false;
  *listen_fd = -1;

  const std::string pathStr = path.string();
  if (pathStr.size() >= sizeof(sockaddr_un::sun_path)) {
    if (error)
      *error = "socket path too long";
    return false;
  }

  (void)::unlink(pathStr.c_str());

  const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) {
    if (error)
      *error = std::string("socket failed: ") + std::strerror(errno);
    return false;
  }

  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  std::strncpy(addr.sun_path, pathStr.c_str(), sizeof(addr.sun_path) - 1);

  if (::bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    const int e = errno;
    ::close(fd);
    if (error)
      *error = std::string("bind failed: ") + std::strerror(e);
    return false;
  }

  if (::listen(fd, 1) != 0) {
    const int e = errno;
    ::close(fd);
    if (error)
      *error = std::string("listen failed: ") + std::strerror(e);
    return false;
  }

  *listen_fd = fd;
  return true;
}

studiocast::ipc::DaemonCallOptions FastOptions() {
  studiocast::ipc::DaemonCallOptions options;
  options.connect_timeout_ms = 100;
  options.io_timeout_ms = 100;
  return options;
}

bool TestDaemonCallSuccess() {
  ScopedRuntimeDir runtime("studiocast-ipc-success");
  if (!runtime.ok()) {
    std::cerr << runtime.error() << "\n";
    return false;
  }

  std::string err;
  const auto socketPath = studiocast::ipc::DaemonSocketPath(&err);
  if (socketPath.empty()) {
    std::cerr << "DaemonSocketPath failed: " << err << "\n";
    return false;
  }

  studiocast::ipc::DaemonServer server;
  if (!server.Start(
          socketPath,
          [](const std::string &line) {
            if (line == "PING")
              return std::string("OK {\"pong\":true}");
            return std::string("ERR {\"error\":\"unexpected\"}");
          },
          &err)) {
    std::cerr << "server.Start failed: " << err << "\n";
    return false;
  }

  studiocast::ipc::DaemonCallResult res;
  err.clear();
  const bool ok = studiocast::ipc::DaemonCall("PING", &res, &err,
                                              FastOptions());
  server.Stop();

  if (!ok || !res.ok || res.json != "{\"pong\":true}") {
    std::cerr << "DaemonCall success path failed; ok=" << ok
              << " res.ok=" << res.ok << " json='" << res.json
              << "' err='" << err << "'\n";
    return false;
  }

  return true;
}

bool TestMissingSocketFailsQuickly() {
  ScopedRuntimeDir runtime("studiocast-ipc-missing");
  if (!runtime.ok()) {
    std::cerr << runtime.error() << "\n";
    return false;
  }

  studiocast::ipc::DaemonCallResult res;
  std::string err;
  const auto start = std::chrono::steady_clock::now();
  const bool ok =
      studiocast::ipc::DaemonCall("GET_STATUS", &res, &err, FastOptions());
  const auto elapsed = std::chrono::steady_clock::now() - start;

  if (ok) {
    std::cerr << "DaemonCall unexpectedly succeeded for missing socket\n";
    return false;
  }

  if (elapsed > 500ms) {
    std::cerr << "missing socket path was not bounded; elapsed="
              << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)
                     .count()
              << "ms err='" << err << "'\n";
    return false;
  }

  return true;
}

bool TestReadTimeoutIsBounded() {
  ScopedRuntimeDir runtime("studiocast-ipc-timeout");
  if (!runtime.ok()) {
    std::cerr << runtime.error() << "\n";
    return false;
  }

  std::string err;
  const auto socketPath = studiocast::ipc::DaemonSocketPath(&err);
  if (socketPath.empty()) {
    std::cerr << "DaemonSocketPath failed: " << err << "\n";
    return false;
  }

  int listenFd = -1;
  if (!BindListeningSocket(socketPath, &listenFd, &err)) {
    std::cerr << "BindListeningSocket failed: " << err << "\n";
    return false;
  }

  std::thread acceptThread([listenFd] {
    const int fd = ::accept4(listenFd, nullptr, nullptr, SOCK_CLOEXEC);
    if (fd >= 0) {
      std::this_thread::sleep_for(200ms);
      ::close(fd);
    }
  });

  studiocast::ipc::DaemonCallOptions options;
  options.connect_timeout_ms = 100;
  options.io_timeout_ms = 50;

  studiocast::ipc::DaemonCallResult res;
  err.clear();
  const auto start = std::chrono::steady_clock::now();
  const bool ok =
      studiocast::ipc::DaemonCall("GET_STATUS", &res, &err, options);
  const auto elapsed = std::chrono::steady_clock::now() - start;

  ::shutdown(listenFd, SHUT_RDWR);
  ::close(listenFd);
  if (acceptThread.joinable())
    acceptThread.join();
  (void)::unlink(socketPath.string().c_str());

  if (ok) {
    std::cerr << "DaemonCall unexpectedly succeeded when server never replied\n";
    return false;
  }

  if (elapsed > 500ms) {
    std::cerr << "read timeout was not bounded; elapsed="
              << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed)
                     .count()
              << "ms err='" << err << "'\n";
    return false;
  }

  if (err.find("read timed out") == std::string::npos) {
    std::cerr << "expected read timeout error, got '" << err << "'\n";
    return false;
  }

  return true;
}

} // namespace

int main() {
  struct Test {
    const char *name;
    bool (*fn)();
  };

  const Test tests[] = {
      {"DaemonCall success", TestDaemonCallSuccess},
      {"DaemonCall missing socket bounded", TestMissingSocketFailsQuickly},
      {"DaemonCall read timeout bounded", TestReadTimeoutIsBounded},
  };

  bool ok = true;
  for (const auto &test : tests) {
    if (!test.fn()) {
      std::cerr << "[FAIL] " << test.name << "\n";
      ok = false;
    } else {
      std::cout << "[PASS] " << test.name << "\n";
    }
  }

  return ok ? 0 : 1;
}
