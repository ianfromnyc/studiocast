#pragma once

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

  // Milliseconds a blocked Read or Write waits before it gives up. A Read
  // reports an error. A Write drops the frame it holds and continues, because
  // a virtual device with no consumer is not driven by the graph and must
  // never stall the pipeline thread.
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

  // Wakes a blocked Read or Write. Safe to call from another thread.
  void RequestStop();

  // Blocking transfer of exactly `bytes` bytes of interleaved float32.
  bool Read(void *dst, std::size_t bytes, std::string *error);
  bool Write(const void *src, std::size_t bytes, std::string *error);

  // Number of sample blocks the node had to drop because the ring was full.
  // The ring is single-producer single-consumer, so a full ring loses the new
  // block and keeps what it holds. See core/pipewire/spsc_byte_ring.h.
  std::uint64_t OverflowCount() const;

  // Drops everything the ring holds. On a node that StudioCast writes into,
  // the real-time callback reads the ring, so it empties the ring on its next
  // pass instead of emptying it here.
  void Flush();

  // Best-effort graph latency in microseconds.
  bool GetLatencyUs(std::uint64_t *latency_us) const;

  // PipeWire global id of the node, or 0 before the node reaches the graph.
  std::uint32_t NodeId() const;

  // Number of graph links that other applications hold on this node.
  int ConsumerCount() const;

  std::string LastError() const;

private:
  std::unique_ptr<Impl> impl_;
};

} // namespace studiocast::pw
