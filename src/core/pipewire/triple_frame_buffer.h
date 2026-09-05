#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace studiocast::pw {

// Single-producer single-consumer frame hand-off that never blocks.
//
// Three slots hold one frame each. The producer owns one of them, the consumer
// owns another, and the third is the hand-over point that one atomic exchange
// moves between the two. Neither side ever waits for the other, so the consumer
// may be a real-time callback: a video frame is megabytes, and a callback that
// waited for a mutex held across a copy of that size would invert the priority
// of the whole graph.
//
// Overflow policy: the newest frame wins. A producer that publishes twice
// before the consumer takes anything throws the older frame away, which is the
// latest-frame-wins rule the rest of the video path follows. Publish says when
// that happened, and the caller counts it as a dropped frame.
//
// A consumer that finds no new frame keeps the last one, so a cycle without a
// fresh frame repeats the previous one instead of sending black.
//
// Every method below says which end it belongs to, and a caller must keep to
// it.
class TripleFrameBuffer final {
public:
  // Either end, but only before the two threads run.
  void Reset(std::size_t frame_bytes) {
    frame_bytes_ = frame_bytes;
    for (auto &slot : slots_)
      slot.assign(frame_bytes, 0);
    back_ = 0;
    front_ = 1;
    handover_.store(kSlot2, std::memory_order_relaxed);
    have_frame_ = false;
  }

  // Either end, but only before the two threads run. Forgets the frame the
  // consumer would repeat.
  void Clear() {
    handover_.store(handover_.load(std::memory_order_relaxed) & kIndexMask,
                    std::memory_order_relaxed);
    have_frame_ = false;
  }

  // Either end.
  std::size_t FrameBytes() const { return frame_bytes_; }

  // Producer end. Copies one frame in and offers it to the consumer.
  //
  // Returns true when the frame it replaced had not been taken yet, which is a
  // dropped frame.
  bool Publish(const std::uint8_t *data, std::size_t bytes) {
    if (!data || frame_bytes_ == 0 || bytes < frame_bytes_)
      return false;
    std::memcpy(slots_[back_].data(), data, frame_bytes_);
    return Offer();
  }

  // Producer end. Copies one frame in row by row, so a source whose rows sit
  // `source_stride` bytes apart lands packed at `row_bytes`.
  //
  // A camera pipeline writes into a buffer the loopback driver sized, and that
  // buffer can hold padding after every row. A copy that took the bytes in one
  // run would read every row after the first from the wrong offset, which
  // shears the picture a consumer receives.
  //
  // Returns the same "replaced an untaken frame" answer as Publish, or false
  // when the source is too small for the rows it says it holds.
  bool PublishRows(const std::uint8_t *data, std::size_t bytes,
                   std::size_t source_stride, std::size_t row_bytes,
                   std::size_t rows) {
    if (!data || frame_bytes_ == 0 || rows == 0 || row_bytes == 0)
      return false;
    if (source_stride < row_bytes || row_bytes * rows != frame_bytes_)
      return false;
    // The last row needs its own bytes only, not the padding behind it.
    if (bytes < source_stride * (rows - 1) + row_bytes)
      return false;

    std::uint8_t *dst = slots_[back_].data();
    if (source_stride == row_bytes) {
      // A packed source needs no row walk, which is the usual case: a size
      // the driver does not pad.
      std::memcpy(dst, data, frame_bytes_);
    } else {
      for (std::size_t y = 0; y < rows; ++y)
        std::memcpy(dst + y * row_bytes, data + y * source_stride, row_bytes);
    }
    return Offer();
  }

  // Consumer end. Returns the newest frame, or the last one when no new frame
  // arrived, or null before the first frame.
  const std::uint8_t *Acquire() {
    if ((handover_.load(std::memory_order_acquire) & kFresh) != 0) {
      const std::uint32_t taken = handover_.exchange(
          static_cast<std::uint32_t>(front_), std::memory_order_acq_rel);
      front_ = taken & kIndexMask;
      have_frame_ = true;
    }
    return have_frame_ ? slots_[front_].data() : nullptr;
  }

private:
  // Producer end. Offers the slot the producer just filled and takes the slot
  // the consumer left. Returns true when the frame it replaced had not been
  // taken yet.
  bool Offer() {
    const std::uint32_t replaced = handover_.exchange(
        static_cast<std::uint32_t>(back_) | kFresh, std::memory_order_acq_rel);
    back_ = replaced & kIndexMask;
    return (replaced & kFresh) != 0;
  }

  // The hand-over word holds a slot index and one bit that says the slot holds
  // a frame the consumer has not taken.
  static constexpr std::uint32_t kIndexMask = 0x3;
  static constexpr std::uint32_t kFresh = 0x4;
  static constexpr std::uint32_t kSlot2 = 2;

  std::vector<std::uint8_t> slots_[3];
  std::size_t frame_bytes_ = 0;

  // Producer only.
  std::size_t back_ = 0;

  // Consumer only.
  std::size_t front_ = 1;
  bool have_frame_ = false;

  std::atomic<std::uint32_t> handover_{kSlot2};
};

} // namespace studiocast::pw
