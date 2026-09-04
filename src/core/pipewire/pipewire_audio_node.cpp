#include "core/pipewire/pipewire_audio_node.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <mutex>
#include <vector>

#if STUDIOCAST_HAVE_PIPEWIRE
#include <map>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>
#endif

// The pw_ and spa_ names below stay unqualified. libpipewire 1.0, which Ubuntu
// 24.04 and Linux Mint 22 ship, gives some of them as macros, among them
// pw_registry_add_listener and spa_strerror. A :: in front of a macro name is a
// compile error, so the plain name is the only spelling that works on both
// libpipewire 1.0 and the inline functions of libpipewire 1.6. All these names
// are C symbols in the global namespace, so the plain name finds them.

namespace studiocast::pw {

namespace {

#if STUDIOCAST_HAVE_PIPEWIRE

// pw_init must run once per process. Every node shares that one call.
void EnsurePipeWireInitialized() {
  static std::once_flag once;
  std::call_once(once, [] { pw_init(nullptr, nullptr); });
}

const char *MediaCategoryFor(AudioNodeRole role) {
  switch (role) {
  case AudioNodeRole::kVirtualSource:
  case AudioNodeRole::kPlayback:
    return "Playback";
  case AudioNodeRole::kVirtualSink:
  case AudioNodeRole::kCapture:
    return "Capture";
  }
  return "Capture";
}

const char *MediaClassFor(AudioNodeRole role) {
  switch (role) {
  case AudioNodeRole::kVirtualSource:
    return "Audio/Source";
  case AudioNodeRole::kVirtualSink:
    return "Audio/Sink";
  case AudioNodeRole::kCapture:
    return "Stream/Input/Audio";
  case AudioNodeRole::kPlayback:
    return "Stream/Output/Audio";
  }
  return "Stream/Input/Audio";
}

// True when StudioCast produces the samples on this node.
bool ProducesSamples(AudioNodeRole role) {
  return role == AudioNodeRole::kVirtualSource ||
         role == AudioNodeRole::kPlayback;
}

// True when the node is a device that other applications connect to, rather
// than a stream that StudioCast connects to a device.
bool IsVirtualDevice(AudioNodeRole role) {
  return role == AudioNodeRole::kVirtualSource ||
         role == AudioNodeRole::kVirtualSink;
}

#endif // STUDIOCAST_HAVE_PIPEWIRE

// Single-producer single-consumer byte ring.
//
// The real-time callback owns one end and the pipeline thread owns the other,
// so no lock is needed on the data path.
class SpscByteRing final {
public:
  void Reset(std::size_t capacity) {
    buf_.assign(capacity + 1, 0);
    head_.store(0, std::memory_order_relaxed);
    tail_.store(0, std::memory_order_relaxed);
  }

  std::size_t Capacity() const { return buf_.empty() ? 0 : buf_.size() - 1; }

  std::size_t Readable() const {
    const std::size_t h = head_.load(std::memory_order_acquire);
    const std::size_t t = tail_.load(std::memory_order_acquire);
    return h >= t ? h - t : buf_.size() - (t - h);
  }

  std::size_t Writable() const { return Capacity() - Readable(); }

  // Returns false and writes nothing when the ring has no room.
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

  // Returns false and reads nothing when the ring holds too little.
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

  // Throws away the oldest bytes to make room for a new write.
  void DropOldest(std::size_t bytes) {
    const std::size_t drop = std::min(bytes, Readable());
    if (drop == 0)
      return;
    const std::size_t t = tail_.load(std::memory_order_relaxed);
    tail_.store((t + drop) % buf_.size(), std::memory_order_release);
  }

  void Clear() {
    tail_.store(head_.load(std::memory_order_acquire),
                std::memory_order_release);
  }

private:
  std::vector<std::uint8_t> buf_;
  std::atomic<std::size_t> head_{0};
  std::atomic<std::size_t> tail_{0};
};

} // namespace

struct PipeWireAudioNode::Impl {
  AudioNodeConfig cfg;
  SpscByteRing ring;

  std::atomic<bool> stop_requested{false};
  std::atomic<std::uint32_t> node_id{0};
  std::atomic<int> consumer_count{0};
  std::atomic<std::uint64_t> latency_us{0};
  std::atomic<std::uint64_t> overflow_count{0};
  std::atomic<bool> latency_valid{false};

  mutable std::mutex error_mu;
  std::string last_error;

  // Wakes a blocked Read or Write. The real-time callback only notifies; it
  // never takes the mutex.
  std::mutex wake_mu;
  std::condition_variable wake_cv;

  void SetError(std::string msg) {
    std::lock_guard<std::mutex> lock(error_mu);
    last_error = std::move(msg);
  }

  std::string Error() const {
    std::lock_guard<std::mutex> lock(error_mu);
    return last_error;
  }

  void Wake() { wake_cv.notify_all(); }

  // Waits a short time for the ring to change. The bounded wait makes a lost
  // notification a small hiccup instead of a hang.
  void WaitForRing() {
    std::unique_lock<std::mutex> lock(wake_mu);
    wake_cv.wait_for(lock, std::chrono::microseconds(500));
  }

#if STUDIOCAST_HAVE_PIPEWIRE
  struct pw_thread_loop *loop = nullptr;
  struct pw_context *context = nullptr;
  struct pw_core *core = nullptr;
  struct pw_registry *registry = nullptr;
  struct pw_stream *stream = nullptr;

  struct spa_hook stream_listener{};
  struct spa_hook registry_listener{};

  // Graph link ids that touch this node, so a remove event can undo a add
  // event.
  std::map<std::uint32_t, bool> counted_links;
  std::mutex links_mu;

  std::size_t stride = 0;
#endif
};

#if STUDIOCAST_HAVE_PIPEWIRE

namespace {

void OnStreamStateChanged(void *data, enum pw_stream_state old,
                          enum pw_stream_state state, const char *error) {
  (void)old;
  auto *impl = static_cast<PipeWireAudioNode::Impl *>(data);
  if (state == PW_STREAM_STATE_ERROR) {
    impl->SetError(error ? std::string(error) : std::string("stream error"));
  } else if (state == PW_STREAM_STATE_STREAMING ||
             state == PW_STREAM_STATE_PAUSED) {
    const std::uint32_t id = pw_stream_get_node_id(impl->stream);
    if (id != PW_ID_ANY)
      impl->node_id.store(id, std::memory_order_release);
  }
  impl->Wake();
}

// Publishes a best-effort graph latency for the status fields.
void UpdateLatency(PipeWireAudioNode::Impl *impl) {
  struct pw_time t{};
  if (pw_stream_get_time_n(impl->stream, &t, sizeof(t)) < 0) {
    impl->latency_valid.store(false, std::memory_order_relaxed);
    return;
  }
  if (t.rate.denom == 0) {
    impl->latency_valid.store(false, std::memory_order_relaxed);
    return;
  }
  const double seconds = static_cast<double>(t.delay) *
                         static_cast<double>(t.rate.num) /
                         static_cast<double>(t.rate.denom);
  const double us = seconds * 1e6;
  impl->latency_us.store(us > 0.0 ? static_cast<std::uint64_t>(us) : 0,
                         std::memory_order_relaxed);
  impl->latency_valid.store(true, std::memory_order_relaxed);
}

void OnStreamProcess(void *data) {
  auto *impl = static_cast<PipeWireAudioNode::Impl *>(data);
  struct pw_buffer *b = pw_stream_dequeue_buffer(impl->stream);
  if (!b)
    return;

  struct spa_buffer *buf = b->buffer;
  if (buf->n_datas == 0 || buf->datas[0].data == nullptr) {
    pw_stream_queue_buffer(impl->stream, b);
    return;
  }

  auto *bytes = static_cast<std::uint8_t *>(buf->datas[0].data);
  const std::size_t stride = impl->stride;

  if (ProducesSamples(impl->cfg.role)) {
    // The server asks for samples. Give it what the ring holds and pad the
    // rest with silence, so an underrun is a short gap and not a stall.
    std::size_t want = buf->datas[0].maxsize;
    if (b->requested != 0 && stride != 0) {
      const std::size_t asked = static_cast<std::size_t>(b->requested) * stride;
      want = std::min(want, asked);
    }
    if (stride != 0)
      want -= want % stride;

    std::size_t got = 0;
    if (want > 0) {
      const std::size_t have = impl->ring.Readable();
      got = std::min(want, have - (stride ? have % stride : 0));
      if (got > 0 && !impl->ring.Pop(bytes, got))
        got = 0;
    }
    if (got < want)
      std::memset(bytes + got, 0, want - got);

    buf->datas[0].chunk->offset = 0;
    buf->datas[0].chunk->stride = static_cast<std::int32_t>(stride);
    buf->datas[0].chunk->size = static_cast<std::uint32_t>(want);
  } else {
    // The server delivers samples. Keep the newest ones when the pipeline
    // thread is late, so latency never grows without a bound.
    const std::size_t size = buf->datas[0].chunk->size;
    const std::size_t offset = buf->datas[0].chunk->offset;
    if (size > 0 && offset + size <= buf->datas[0].maxsize) {
      if (size > impl->ring.Writable())
        impl->ring.DropOldest(size - impl->ring.Writable());
      (void)impl->ring.Push(bytes + offset,
                            std::min(size, impl->ring.Capacity()));
    }
  }

  pw_stream_queue_buffer(impl->stream, b);
  UpdateLatency(impl);
  impl->Wake();
}

// Built field by field, because the PipeWire event structures grow between
// releases and a partial aggregate initializer would warn.
const struct pw_stream_events &StreamEvents() {
  static const struct pw_stream_events events = [] {
    struct pw_stream_events e{};
    e.version = PW_VERSION_STREAM_EVENTS;
    e.state_changed = OnStreamStateChanged;
    e.process = OnStreamProcess;
    return e;
  }();
  return events;
}

// Reads a numeric node id out of a link property.
bool LinkEndpointMatches(const struct spa_dict *props, const char *key,
                         std::uint32_t node_id) {
  const char *v = spa_dict_lookup(props, key);
  if (!v)
    return false;
  char *end = nullptr;
  const unsigned long parsed = std::strtoul(v, &end, 10);
  if (!end || *end != '\0')
    return false;
  return static_cast<std::uint32_t>(parsed) == node_id;
}

void OnRegistryGlobal(void *data, std::uint32_t id, std::uint32_t permissions,
                      const char *type, std::uint32_t version,
                      const struct spa_dict *props) {
  (void)permissions;
  (void)version;
  auto *impl = static_cast<PipeWireAudioNode::Impl *>(data);
  if (!type || std::strcmp(type, PW_TYPE_INTERFACE_Link) != 0 || !props)
    return;

  const std::uint32_t self = impl->node_id.load(std::memory_order_acquire);
  if (self == 0)
    return;

  // A virtual source is the output end of every consumer link. A virtual sink
  // is the input end.
  const char *key = ProducesSamples(impl->cfg.role) ? PW_KEY_LINK_OUTPUT_NODE
                                                    : PW_KEY_LINK_INPUT_NODE;
  if (!LinkEndpointMatches(props, key, self))
    return;

  std::lock_guard<std::mutex> lock(impl->links_mu);
  if (impl->counted_links.emplace(id, true).second)
    impl->consumer_count.fetch_add(1, std::memory_order_relaxed);
}

void OnRegistryGlobalRemove(void *data, std::uint32_t id) {
  auto *impl = static_cast<PipeWireAudioNode::Impl *>(data);
  std::lock_guard<std::mutex> lock(impl->links_mu);
  if (impl->counted_links.erase(id) > 0)
    impl->consumer_count.fetch_sub(1, std::memory_order_relaxed);
}

const struct pw_registry_events &RegistryEvents() {
  static const struct pw_registry_events events = [] {
    struct pw_registry_events e{};
    e.version = PW_VERSION_REGISTRY_EVENTS;
    e.global = OnRegistryGlobal;
    e.global_remove = OnRegistryGlobalRemove;
    return e;
  }();
  return events;
}

} // namespace

#endif // STUDIOCAST_HAVE_PIPEWIRE

PipeWireAudioNode::PipeWireAudioNode() : impl_(std::make_unique<Impl>()) {}

PipeWireAudioNode::~PipeWireAudioNode() { Stop(); }

#if !STUDIOCAST_HAVE_PIPEWIRE

bool PipeWireAudioNode::Start(const AudioNodeConfig &cfg, std::string *error) {
  (void)cfg;
  const std::string msg = "StudioCast was built without PipeWire support "
                          "(STUDIOCAST_ENABLE_PIPEWIRE=OFF).";
  impl_->SetError(msg);
  if (error)
    *error = msg;
  return false;
}

void PipeWireAudioNode::Stop() {}

bool PipeWireAudioNode::Read(void *dst, std::size_t bytes, std::string *error) {
  (void)dst;
  (void)bytes;
  if (error)
    *error = impl_->Error();
  return false;
}

bool PipeWireAudioNode::Write(const void *src, std::size_t bytes,
                              std::string *error) {
  (void)src;
  (void)bytes;
  if (error)
    *error = impl_->Error();
  return false;
}

#else

bool PipeWireAudioNode::Start(const AudioNodeConfig &cfg, std::string *error) {
  if (error)
    error->clear();
  Stop();

  if (cfg.channels == 0 || cfg.channels > 2 || cfg.sample_rate <= 0 ||
      cfg.frame_samples == 0) {
    const std::string msg = "Unsupported PipeWire node format.";
    impl_->SetError(msg);
    if (error)
      *error = msg;
    return false;
  }

  EnsurePipeWireInitialized();

  impl_->cfg = cfg;
  impl_->stop_requested.store(false, std::memory_order_release);
  impl_->node_id.store(0, std::memory_order_release);
  impl_->consumer_count.store(0, std::memory_order_relaxed);
  impl_->latency_valid.store(false, std::memory_order_relaxed);
  impl_->overflow_count.store(0, std::memory_order_relaxed);
  impl_->stride = AudioFrameBytes(1, cfg.channels);
  impl_->ring.Reset(
      AudioRingCapacityBytes(cfg.frame_samples, cfg.channels, cfg.ring_frames));
  impl_->SetError({});

  const std::string name =
      cfg.node_name.empty() ? std::string("studiocast") : cfg.node_name;
  const std::string description =
      cfg.node_description.empty() ? name : cfg.node_description;

  impl_->loop = pw_thread_loop_new(name.c_str(), nullptr);
  if (!impl_->loop) {
    const std::string msg = "Could not create the PipeWire thread loop.";
    impl_->SetError(msg);
    if (error)
      *error = msg;
    return false;
  }

  impl_->context =
      pw_context_new(pw_thread_loop_get_loop(impl_->loop), nullptr, 0);
  if (!impl_->context) {
    Stop();
    const std::string msg = "Could not create the PipeWire context.";
    impl_->SetError(msg);
    if (error)
      *error = msg;
    return false;
  }

  if (pw_thread_loop_start(impl_->loop) < 0) {
    Stop();
    const std::string msg = "Could not start the PipeWire thread loop.";
    impl_->SetError(msg);
    if (error)
      *error = msg;
    return false;
  }

  pw_thread_loop_lock(impl_->loop);

  impl_->core = pw_context_connect(impl_->context, nullptr, 0);
  if (!impl_->core) {
    pw_thread_loop_unlock(impl_->loop);
    Stop();
    const std::string msg = "Could not connect to the PipeWire server.";
    impl_->SetError(msg);
    if (error)
      *error = msg;
    return false;
  }

  impl_->registry = pw_core_get_registry(impl_->core, PW_VERSION_REGISTRY, 0);
  if (impl_->registry) {
    pw_registry_add_listener(impl_->registry, &impl_->registry_listener,
                               &RegistryEvents(), impl_.get());
  }

  const std::string latency =
      NodeLatencyProperty(cfg.frame_samples, cfg.sample_rate);
  const std::string rate = NodeRateProperty(cfg.sample_rate);

  struct pw_properties *props = pw_properties_new(
      PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY,
      MediaCategoryFor(cfg.role), PW_KEY_MEDIA_ROLE, "Production",
      PW_KEY_MEDIA_CLASS, MediaClassFor(cfg.role), PW_KEY_NODE_NAME,
      name.c_str(), PW_KEY_NODE_DESCRIPTION, description.c_str(),
      PW_KEY_NODE_LATENCY, latency.c_str(), PW_KEY_NODE_RATE, rate.c_str(),
      PW_KEY_APP_NAME, "StudioCast", nullptr);

  if (IsVirtualDevice(cfg.role)) {
    pw_properties_set(props, PW_KEY_NODE_VIRTUAL, "true");
  } else if (!cfg.target_object.empty()) {
#ifdef PW_KEY_TARGET_OBJECT
    pw_properties_set(props, PW_KEY_TARGET_OBJECT, cfg.target_object.c_str());
#else
    pw_properties_set(props, PW_KEY_NODE_TARGET, cfg.target_object.c_str());
#endif
  }

  impl_->stream = pw_stream_new(impl_->core, name.c_str(), props);
  if (!impl_->stream) {
    pw_thread_loop_unlock(impl_->loop);
    Stop();
    const std::string msg = "Could not create the PipeWire stream.";
    impl_->SetError(msg);
    if (error)
      *error = msg;
    return false;
  }

  pw_stream_add_listener(impl_->stream, &impl_->stream_listener,
                           &StreamEvents(), impl_.get());

  std::uint8_t pod_buffer[1024];
  struct spa_pod_builder builder =
      SPA_POD_BUILDER_INIT(pod_buffer, sizeof(pod_buffer));
  struct spa_audio_info_raw info{};
  info.format = SPA_AUDIO_FORMAT_F32;
  info.rate = static_cast<std::uint32_t>(cfg.sample_rate);
  info.channels = cfg.channels;
  if (cfg.channels == 1) {
    info.position[0] = SPA_AUDIO_CHANNEL_MONO;
  } else {
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
    info.position[1] = SPA_AUDIO_CHANNEL_FR;
  }

  const struct spa_pod *params[1];
  params[0] =
      spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &info);

  // A virtual device waits for other applications to connect to it, so it must
  // not connect itself.
  auto flags = static_cast<enum pw_stream_flags>(PW_STREAM_FLAG_MAP_BUFFERS |
                                                 PW_STREAM_FLAG_RT_PROCESS);
  if (!IsVirtualDevice(cfg.role)) {
    flags = static_cast<enum pw_stream_flags>(static_cast<int>(flags) |
                                              PW_STREAM_FLAG_AUTOCONNECT);
  }

  const enum spa_direction direction =
      ProducesSamples(cfg.role) ? SPA_DIRECTION_OUTPUT : SPA_DIRECTION_INPUT;

  const int rc = pw_stream_connect(impl_->stream, direction, PW_ID_ANY, flags,
                                     params, 1);
  pw_thread_loop_unlock(impl_->loop);

  if (rc < 0) {
    const std::string msg =
        std::string("Could not connect the PipeWire node: ") +
        spa_strerror(rc);
    Stop();
    impl_->SetError(msg);
    if (error)
      *error = msg;
    return false;
  }

  return true;
}

void PipeWireAudioNode::Stop() {
  RequestStop();

  if (impl_->loop)
    pw_thread_loop_stop(impl_->loop);

  if (impl_->stream) {
    pw_stream_destroy(impl_->stream);
    impl_->stream = nullptr;
  }
  if (impl_->registry) {
    pw_proxy_destroy(reinterpret_cast<struct pw_proxy *>(impl_->registry));
    impl_->registry = nullptr;
  }
  if (impl_->core) {
    pw_core_disconnect(impl_->core);
    impl_->core = nullptr;
  }
  if (impl_->context) {
    pw_context_destroy(impl_->context);
    impl_->context = nullptr;
  }
  if (impl_->loop) {
    pw_thread_loop_destroy(impl_->loop);
    impl_->loop = nullptr;
  }

  {
    std::lock_guard<std::mutex> lock(impl_->links_mu);
    impl_->counted_links.clear();
  }
  impl_->consumer_count.store(0, std::memory_order_relaxed);
  impl_->node_id.store(0, std::memory_order_release);
}

bool PipeWireAudioNode::Read(void *dst, std::size_t bytes, std::string *error) {
  if (error)
    error->clear();
  if (!impl_->stream) {
    if (error)
      *error = "The PipeWire node is not running.";
    return false;
  }

  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(impl_->cfg.io_timeout_ms);
  while (!impl_->stop_requested.load(std::memory_order_acquire)) {
    if (impl_->ring.Pop(dst, bytes))
      return true;
    if (std::chrono::steady_clock::now() >= deadline) {
      if (error)
        *error = "Timed out while reading from the PipeWire node.";
      return false;
    }
    impl_->WaitForRing();
  }

  if (error)
    *error = impl_->Error();
  return false;
}

bool PipeWireAudioNode::Write(const void *src, std::size_t bytes,
                              std::string *error) {
  if (error)
    error->clear();
  if (!impl_->stream) {
    if (error)
      *error = "The PipeWire node is not running.";
    return false;
  }
  if (bytes > impl_->ring.Capacity()) {
    if (error)
      *error = "The write is larger than the PipeWire ring.";
    return false;
  }

  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(impl_->cfg.io_timeout_ms);
  while (!impl_->stop_requested.load(std::memory_order_acquire)) {
    if (impl_->ring.Push(src, bytes))
      return true;
    if (std::chrono::steady_clock::now() >= deadline) {
      // Nothing consumes the node, so the graph does not run it. Drop the
      // oldest samples and keep the pipeline thread moving.
      impl_->ring.DropOldest(bytes);
      impl_->overflow_count.fetch_add(1, std::memory_order_relaxed);
      return impl_->ring.Push(src, bytes);
    }
    impl_->WaitForRing();
  }

  if (error)
    *error = impl_->Error();
  return false;
}

#endif // STUDIOCAST_HAVE_PIPEWIRE

void PipeWireAudioNode::RequestStop() {
  impl_->stop_requested.store(true, std::memory_order_release);
  impl_->Wake();
}

void PipeWireAudioNode::Flush() { impl_->ring.Clear(); }

bool PipeWireAudioNode::GetLatencyUs(std::uint64_t *latency_us) const {
  if (!impl_->latency_valid.load(std::memory_order_relaxed))
    return false;
  if (latency_us)
    *latency_us = impl_->latency_us.load(std::memory_order_relaxed);
  return true;
}

std::uint64_t PipeWireAudioNode::OverflowCount() const {
  return impl_->overflow_count.load(std::memory_order_relaxed);
}

std::uint32_t PipeWireAudioNode::NodeId() const {
  return impl_->node_id.load(std::memory_order_acquire);
}

int PipeWireAudioNode::ConsumerCount() const {
  return impl_->consumer_count.load(std::memory_order_relaxed);
}

std::string PipeWireAudioNode::LastError() const { return impl_->Error(); }

} // namespace studiocast::pw
