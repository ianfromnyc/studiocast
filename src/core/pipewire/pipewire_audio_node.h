#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "core/pipewire/pipewire_support.h"

namespace studiocast::pw {

// What a node does in the graph.
enum class AudioNodeRole {
  // A virtual capture device. StudioCast writes processed audio into it and
  // other applications record from it. media.class = Audio/Source.
  kVirtualSource,

  // A virtual playback device. Other applications play into it and StudioCast
  // reads from it. media.class = Audio/Sink.
  kVirtualSink,

  // A plain capture stream that reads from a real source node.
  kCapture,

  // A plain playback stream that writes into a real sink node.
  kPlayback,
};

struct AudioNodeConfig {
  AudioNodeRole role = AudioNodeRole::kCapture;

  // node.name and node.description. Leave the description empty to reuse the
  // name.
  std::string node_name;
  std::string node_description;

  // node.name of the peer for kCapture and kPlayback. Empty selects the
  // default device. Ignored for the two virtual roles.
  std::string target_object;

  int sample_rate = 48000;
  std::uint32_t channels = 1;
  std::uint32_t frame_samples = 480;

  // Number of pipeline frames the ring can hold.
  int ring_frames = 8;

  // Milliseconds a blocked Read waits for a full frame before it gives up and
  // reports an error. Write does not use it: a virtual device with no consumer
  // is not driven by the graph, so a write that waited here would stall the
  // pipeline thread. Write waits one quantum at most, then drops the frame it
  // holds and reports success. See OverflowCount.
  int io_timeout_ms = 500;
};

// One PipeWire node with its own thread loop.
//
// The real-time callback moves samples through a single-producer
// single-consumer ring, so the pipeline thread keeps its 10 ms frame format
// whatever quantum the server picks.
//
// Every method is safe to call from the pipeline thread. In a build with
// STUDIOCAST_HAVE_PIPEWIRE=0, Start always fails and says why.
class PipeWireAudioNode final {
public:
  // Holds the PipeWire objects. The definition stays in the source file, so
  // no PipeWire header leaks into the rest of StudioCast. It is public only so
  // the C callbacks can name it.
  struct Impl;

  PipeWireAudioNode();
  ~PipeWireAudioNode();

  PipeWireAudioNode(const PipeWireAudioNode &) = delete;
  PipeWireAudioNode &operator=(const PipeWireAudioNode &) = delete;

  bool Start(const AudioNodeConfig &cfg, std::string *error);
  void Stop();

  // Wakes a blocked Read or Write and makes every later one fail. Safe to
  // call from another thread.
  //
  // The flag stays set until Start or ClearStopRequest, so a caller that wakes
  // a node it does not own must put it back with ClearStopRequest.
  void RequestStop();

  // Takes the stop request back, so the node carries samples again.
  void ClearStopRequest();

  // Transfer of exactly `bytes` bytes of interleaved float32.
  //
  // Read blocks for io_timeout_ms and reports an error when no full frame
  // arrives. Write is as good as non-blocking: it waits one quantum at most
  // for the real-time callback to make room, then drops the frame, counts the
  // drop and reports success, so the pipeline thread keeps its cadence. Both
  // report an error when the node stops or the stream goes down.
  bool Read(void *dst, std::size_t bytes, std::string *error);
  bool Write(const void *src, std::size_t bytes, std::string *error);

  // Number of sample blocks the node had to drop because the ring was full.
  // The ring is single-producer single-consumer, so a full ring loses the new
  // block and keeps what it holds. See core/pipewire/spsc_byte_ring.h.
  std::uint64_t OverflowCount() const;

  // Drops everything the ring holds at the moment of the call.
  //
  // On a node that StudioCast writes into, the real-time callback is the one
  // that reads the ring, so only it may drop anything, and it does so on its
  // next pass. Nothing drives a node with no consumer, so that pass can be a
  // long time later: the call therefore records how much the ring held, and
  // the callback drops that much and keeps every sample written since.
  void Flush();

  // Best-effort graph latency in microseconds.
  bool GetLatencyUs(std::uint64_t *latency_us) const;

  // True while the node is connected to the graph. It turns false for good
  // when the server takes the stream down, and such a node never comes back by
  // itself: the owner must make a new one.
  bool IsRunning() const;

  // The format the node was started with. A caller that moves samples through
  // it must bring the same one. Only meaningful after a Start that succeeded.
  AudioNodeConfig Format() const;

  // PipeWire global id of the node, or 0 before the node reaches the graph.
  std::uint32_t NodeId() const;

  // Number of graph links that other applications hold on this node.
  int ConsumerCount() const;

  std::string LastError() const;

private:
  std::unique_ptr<Impl> impl_;
};

namespace internal {

// How long a write may wait for room on a full ring.
//
// One quantum is all the real-time callback needs to take a block, and the cap
// keeps a node with a long frame from holding the pipeline thread. A node that
// nothing consumes is not driven at all, so no wait would help it: there the
// wait only costs the small delay before the frame goes.
//
// The pipeline thread hands over a frame every 10 ms, so this must stay well
// under that.
std::chrono::microseconds FullRingWait(const AudioNodeConfig &cfg);

} // namespace internal

} // namespace studiocast::pw
