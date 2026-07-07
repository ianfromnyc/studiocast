#pragma once

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace studiocast::ipc {

// Minimal line-based Unix domain socket server.
//
// Protocol:
//   - Each request is a single line terminated by '\n'
//   - Each response is a single line terminated by '\n'
//   - The handler is called once per request line and must return the response
//   line
//     WITHOUT a trailing newline.
class DaemonServer final {
public:
  using Handler = std::function<std::string(const std::string &request_line)>;

  DaemonServer() = default;
  ~DaemonServer();

  DaemonServer(const DaemonServer &) = delete;
  DaemonServer &operator=(const DaemonServer &) = delete;

  bool Start(const std::filesystem::path &socket_path, Handler handler,
             std::string *error);
  void Stop();

  bool Running() const { return running_.load(); }

private:
  void AcceptThread();
  void ClientThread(int fd);

  std::atomic_bool stop_{false};
  std::atomic_bool running_{false};

  int listen_fd_ = -1;
  std::filesystem::path socket_path_;
  Handler handler_;

  std::thread accept_th_;

  mutable std::mutex mu_;
  std::condition_variable client_cv_;
  std::vector<int> client_fds_; // active client fds for shutdown
  std::size_t active_clients_ = 0;
};

} // namespace studiocast::ipc
