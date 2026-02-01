#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>

#include "core/video/v4l2_writer.h"

namespace studiocast::video {

enum class FeedPixelFormatMode {
  auto_select,
  yuyv,
  rgb24,
};

struct FeedConfig {
  // If empty, auto-select first writable v4l2loopback device from
  // ProbeLoopback().
  std::string device;

  int width = 1280;
  int height = 720;
  int fps = 30;

  FeedPixelFormatMode format = FeedPixelFormatMode::auto_select;
};

struct FeedStatus {
  bool running = false;
  bool starting = false;

  std::string device;
  ActualFormat actual{};
  int frame_index = 0;

  std::string last_error;
};

class VideoFeed final {
public:
  VideoFeed() = default;
  ~VideoFeed();

  VideoFeed(const VideoFeed &) = delete;
  VideoFeed &operator=(const VideoFeed &) = delete;

  bool StartTestPattern(const FeedConfig &cfg, std::string *error);
  void Stop();

  FeedStatus Status() const;

private:
  void ThreadMain(FeedConfig cfg);

  static bool OpenWriterWithMode(V4l2Writer *writer, const std::string &device,
                                 int width, int height, int fps,
                                 FeedPixelFormatMode mode, std::string *error);

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::thread th_;
  std::atomic_bool stop_{false};

  // guarded by mu_
  bool running_ = false;
  bool starting_ = false;
  bool start_notified_ = false;

  std::string device_;
  ActualFormat actual_{};
  int frame_index_ = 0;
  std::string last_error_;
};

} // namespace studiocast::video
