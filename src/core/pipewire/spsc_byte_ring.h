#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace studiocast::pw {

// Single-producer single-consumer byte ring.
//
// One thread pushes and one thread pops, and no lock is needed on the data
// path. One rule makes that safe: only the producer writes the write end, and
// only the consumer writes the read end. Every method below says which end it
// belongs to, and a caller must keep to it.
//
// Overflow policy: drop-newest. A producer that finds no room keeps what the
// ring already holds and throws away the data it is carrying. Drop-oldest
// would need the producer to move the read end, which the consumer moves as
// well, and two writers of the read end corrupt the ring.
//
// Both policies leave the ring full and both leave the same backlog behind, so
// drop-newest costs no latency. It loses the newest bytes instead of the
// oldest ones. The caller counts what it drops.
class SpscByteRing final {
public:
  // Either end, but only before the two threads run.
  void Reset(std::size_t capacity) {
    buf_.assign(capacity + 1, 0);
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_relaxed);
  }

  // Either end.
  std::size_t Capacity() const { return buf_.empty() ? 0 : buf_.size() - 1; }

  // Either end.
  std::size_t Readable() const {
    const std::size_t h = head_.load(std::memory_order_acquire);
    const std::size_t t = tail_.load(std::memory_order_acquire);
    return h >= t ? h - t : buf_.size() - (t - h);
  }

  // Either end.
  std::size_t Writable() const { return Capacity() - Readable(); }

  // Producer end. Returns false and writes nothing when the ring has no room,
  // which is the drop-newest policy above: the caller drops the data it holds
  // and counts the drop.
  bool Push(const void *src, std::size_t bytes) {
    if (bytes > Writable())
      return false;
    const auto *in = static_cast<const std::uint8_t *>(src);
    std::size_t h = head_.load(std::memory_order_relaxed);
    const std::size_t first = std::min(bytes, buf_.size() - h);
    std::memcpy(buf_.data() + h, in, first);
    if (bytes > first)
      std::memcpy(buf_.data(), in + first, bytes - first);
    h = (h + bytes) % buf_.size();
    head_.store(h, std::memory_order_release);
    return true;
  }

  // Consumer end. Returns false and reads nothing when the ring holds too
  // little.
  bool Pop(void *dst, std::size_t bytes) {
    if (bytes > Readable())
      return false;
    auto *out = static_cast<std::uint8_t *>(dst);
    std::size_t t = tail_.load(std::memory_order_relaxed);
    const std::size_t first = std::min(bytes, buf_.size() - t);
    std::memcpy(out, buf_.data() + t, first);
    if (bytes > first)
      std::memcpy(out + first, buf_.data(), bytes - first);
    t = (t + bytes) % buf_.size();
    tail_.store(t, std::memory_order_release);
    return true;
  }

  // Consumer end. Throws away everything the ring holds.
  void Clear() {
    tail_.store(head_.load(std::memory_order_acquire),
                std::memory_order_release);
  }

private:
  std::vector<std::uint8_t> buf_;
  std::atomic<std::size_t> head_{0};
  std::atomic<std::size_t> tail_{0};
};

} // namespace studiocast::pw
