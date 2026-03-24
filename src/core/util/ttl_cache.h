#pragma once

#include <chrono>
#include <mutex>
#include <optional>
#include <utility>

namespace studiocast::util {

// Simple thread-safe TTL cache.
//
// - Uses steady_clock time points.
// - Computes under the mutex (single-flight) to avoid duplicate work.
// - Accepts an explicit `now` for deterministic testing.
template <typename T> class TtlCache {
public:
  using Clock = std::chrono::steady_clock;
  using TimePoint = Clock::time_point;
  using Duration = Clock::duration;

  template <typename Fn>
  T GetOrCompute(TimePoint now, Duration ttl, Fn &&compute) {
    std::lock_guard<std::mutex> lock(mu_);
    const bool expired = !value_.has_value() || at_ == TimePoint{} ||
                         ttl <= Duration::zero() || (now - at_) >= ttl;
    if (expired) {
      value_ = std::forward<Fn>(compute)();
      at_ = now;
    }
    return *value_;
  }

  void Invalidate() {
    std::lock_guard<std::mutex> lock(mu_);
    value_.reset();
    at_ = TimePoint{};
  }

private:
  std::mutex mu_;
  std::optional<T> value_;
  TimePoint at_{};
};

} // namespace studiocast::util
