#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <functional>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

namespace studiocast::open_video {

struct LatestFrameWinsWorkerStats {
  std::uint64_t submitted = 0;
  std::uint64_t started = 0;
  std::uint64_t completed = 0;
  std::uint64_t failed = 0;
  std::uint64_t overwritten = 0;
  std::uint64_t dropped = 0;
  std::uint64_t stale_results = 0;
  std::uint64_t last_completed_sequence = 0;
  std::string last_error;
};

template <typename TResult> class LatestFrameWinsResult {
public:
  static LatestFrameWinsResult Success(TResult value) {
    LatestFrameWinsResult result;
    result.ok_ = true;
    result.value_.emplace(std::move(value));
    return result;
  }

  static LatestFrameWinsResult Failure(std::string error) {
    LatestFrameWinsResult result;
    result.ok_ = false;
    result.error_ = std::move(error);
    if (result.error_.empty()) {
      result.error_ = "latest-frame worker processor failed.";
    }
    return result;
  }

  bool ok() const { return ok_; }
  const std::string &error() const { return error_; }
  bool has_value() const { return value_.has_value(); }

  std::optional<TResult> TakeValue() { return std::move(value_); }

private:
  bool ok_ = false;
  std::optional<TResult> value_;
  std::string error_;
};

template <typename TFrame, typename TResult> class LatestFrameWinsWorker {
public:
  struct Task {
    std::uint64_t sequence = 0;
    std::uint64_t generation = 0;
    TFrame frame;
  };

  using Result = LatestFrameWinsResult<TResult>;
  using Processor = std::function<Result(const Task &)>;
  using CompletionCallback = std::function<void(const Task &, TResult)>;
  using ErrorCallback = std::function<void(const Task &, const std::string &)>;

  explicit LatestFrameWinsWorker(Processor processor,
                                 CompletionCallback on_completed = {},
                                 ErrorCallback on_error = {})
      : processor_(std::move(processor)),
        on_completed_(std::move(on_completed)), on_error_(std::move(on_error)) {
    if (!processor_) {
      throw std::invalid_argument(
          "LatestFrameWinsWorker requires a processor callback.");
    }
    worker_ = std::thread(&LatestFrameWinsWorker::ThreadMain, this);
  }

  ~LatestFrameWinsWorker() { Stop(); }

  LatestFrameWinsWorker(const LatestFrameWinsWorker &) = delete;
  LatestFrameWinsWorker &operator=(const LatestFrameWinsWorker &) = delete;
  LatestFrameWinsWorker(LatestFrameWinsWorker &&) = delete;
  LatestFrameWinsWorker &operator=(LatestFrameWinsWorker &&) = delete;

  bool Submit(std::uint64_t sequence, TFrame frame) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (stop_requested_) {
        return false;
      }

      ++stats_.submitted;
      Task task{sequence, generation_, std::move(frame)};
      if (pending_.has_value()) {
        ++stats_.overwritten;
        ++stats_.dropped;
        pending_.reset();
      }
      pending_.emplace(std::move(task));
    }

    cv_.notify_one();
    return true;
  }

  std::uint64_t AdvanceGeneration() {
    std::uint64_t generation = 0;
    {
      std::lock_guard<std::mutex> lock(mu_);
      ++generation_;
      generation = generation_;
      if (pending_.has_value()) {
        pending_.reset();
        ++stats_.dropped;
      }
    }
    idle_cv_.notify_all();
    return generation;
  }

  std::uint64_t CurrentGeneration() const {
    std::lock_guard<std::mutex> lock(mu_);
    return generation_;
  }

  LatestFrameWinsWorkerStats GetStats() const {
    std::lock_guard<std::mutex> lock(mu_);
    return stats_;
  }

  bool WaitForIdle(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mu_);
    return idle_cv_.wait_for(
        lock, timeout, [this] { return !busy_ && !pending_.has_value(); });
  }

  bool IsStopped() const {
    std::lock_guard<std::mutex> lock(mu_);
    return stop_requested_;
  }

  void Stop() {
    {
      std::lock_guard<std::mutex> lock(mu_);
      if (!stop_requested_) {
        stop_requested_ = true;
        if (pending_.has_value()) {
          pending_.reset();
          ++stats_.dropped;
        }
      }
    }

    cv_.notify_all();
    idle_cv_.notify_all();

    if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
      worker_.join();
    }
  }

private:
  void ThreadMain() {
    for (;;) {
      std::optional<Task> task;
      {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock,
                 [this] { return stop_requested_ || pending_.has_value(); });

        if (stop_requested_) {
          break;
        }

        task.emplace(std::move(*pending_));
        pending_.reset();
        busy_ = true;
        ++stats_.started;
      }

      Result result = Result::Failure("latest-frame worker processor did not "
                                      "produce a result.");
      try {
        result = processor_(*task);
      } catch (const std::exception &ex) {
        result = Result::Failure(ex.what());
      } catch (...) {
        result = Result::Failure("latest-frame worker processor threw an "
                                 "unknown exception.");
      }

      CompletionCallback completion_callback;
      ErrorCallback error_callback;
      std::optional<TResult> completion_value;
      std::string error;
      {
        std::lock_guard<std::mutex> lock(mu_);
        const bool accepts_result =
            !stop_requested_ && task->generation == generation_;

        if (accepts_result) {
          if (result.ok() && result.has_value()) {
            ++stats_.completed;
            stats_.last_completed_sequence = task->sequence;
            completion_value = result.TakeValue();
            completion_callback = on_completed_;
          } else {
            ++stats_.failed;
            error = result.error();
            if (error.empty()) {
              error =
                  "latest-frame worker processor returned an invalid result.";
            }
            stats_.last_error = error;
            error_callback = on_error_;
          }
        } else {
          ++stats_.stale_results;
        }

        busy_ = false;
      }

      idle_cv_.notify_all();

      if (completion_callback && completion_value.has_value()) {
        completion_callback(*task, std::move(*completion_value));
      }
      if (error_callback) {
        error_callback(*task, error);
      }
    }

    std::lock_guard<std::mutex> lock(mu_);
    busy_ = false;
    pending_.reset();
    idle_cv_.notify_all();
  }

  Processor processor_;
  CompletionCallback on_completed_;
  ErrorCallback on_error_;

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::condition_variable idle_cv_;
  std::thread worker_;

  bool stop_requested_ = false;
  bool busy_ = false;
  std::uint64_t generation_ = 1;
  std::optional<Task> pending_;
  LatestFrameWinsWorkerStats stats_;
};

} // namespace studiocast::open_video
