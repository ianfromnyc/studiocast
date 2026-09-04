#include "core/video/pipewire/pipewire_camera_node.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#if STUDIOCAST_HAVE_PIPEWIRE
#include <cstdlib>
#include <map>

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>
#endif

// The pw_ and spa_ names below stay unqualified. libpipewire 1.0, which Ubuntu
// 24.04 and Linux Mint 22 ship, gives some of them as macros, among them
// pw_registry_add_listener and spa_strerror. A :: in front of a macro name is a
// compile error, so the plain name is the only spelling that works on both
// libpipewire 1.0 and the inline functions of libpipewire 1.6. All these names
// are C symbols in the global namespace, so the plain name finds them.

namespace studiocast::video::pw_backend {

std::size_t CameraFrameBytes(int width, int height, PixelFormat format) {
  if (width <= 0 || height <= 0)
    return 0;
  const std::size_t pixels =
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
  switch (format) {
  case PixelFormat::rgb24:
    return pixels * 3;
  case PixelFormat::yuyv:
    return pixels * 2;
  }
  return 0;
}

namespace {

std::size_t CameraStrideBytes(int width, PixelFormat format) {
  if (width <= 0)
    return 0;
  const std::size_t w = static_cast<std::size_t>(width);
  switch (format) {
  case PixelFormat::rgb24:
    return w * 3;
  case PixelFormat::yuyv:
    return w * 2;
  }
  return 0;
}

#if STUDIOCAST_HAVE_PIPEWIRE

void EnsurePipeWireInitialized() {
  static std::once_flag once;
  std::call_once(once, [] { pw_init(nullptr, nullptr); });
}

std::uint32_t SpaFormatFor(PixelFormat format) {
  switch (format) {
  case PixelFormat::rgb24:
    return SPA_VIDEO_FORMAT_RGB;
  case PixelFormat::yuyv:
    return SPA_VIDEO_FORMAT_YUY2;
  }
  return SPA_VIDEO_FORMAT_RGB;
}

#endif // STUDIOCAST_HAVE_PIPEWIRE

} // namespace

struct PipeWireCameraNode::Impl {
  CameraNodeConfig cfg;
  std::size_t frame_bytes = 0;
  std::size_t stride_bytes = 0;

  // The newest staged frame. The real-time callback takes it under this
  // mutex, which it holds only for one copy.
  //
  // `staged_valid` says the buffer holds a frame, so a cycle without a new
  // frame repeats the last one instead of sending black. `staged_pending`
  // says the callback has not taken that frame yet, so staging over it loses
  // a frame. Only that case is a drop.
  std::mutex frame_mu;
  std::vector<std::uint8_t> staged;
  bool staged_valid = false;
  bool staged_pending = false;

  std::atomic<bool> running{false};
  std::atomic<std::uint32_t> node_id{0};
  std::atomic<int> consumer_count{0};
  std::atomic<std::uint64_t> frames_sent{0};
  std::atomic<std::uint64_t> frames_dropped{0};

  mutable std::mutex error_mu;
  std::string last_error;

  void SetError(std::string msg) {
    std::lock_guard<std::mutex> lock(error_mu);
    last_error = std::move(msg);
  }

  std::string Error() const {
    std::lock_guard<std::mutex> lock(error_mu);
    return last_error;
  }

#if STUDIOCAST_HAVE_PIPEWIRE
  struct pw_thread_loop *loop = nullptr;
  struct pw_context *context = nullptr;
  struct pw_core *core = nullptr;
  struct pw_registry *registry = nullptr;
  struct pw_stream *stream = nullptr;

  struct spa_hook stream_listener {};
  struct spa_hook registry_listener {};

  std::map<std::uint32_t, bool> counted_links;
  std::mutex links_mu;
#endif
};

#if STUDIOCAST_HAVE_PIPEWIRE

namespace {

void OnStreamStateChanged(void *data, enum pw_stream_state old,
                          enum pw_stream_state state, const char *error) {
  (void)old;
  auto *impl = static_cast<PipeWireCameraNode::Impl *>(data);
  if (state == PW_STREAM_STATE_ERROR) {
    impl->SetError(error ? std::string(error) : std::string("stream error"));
    return;
  }
  if (state == PW_STREAM_STATE_STREAMING ||
      state == PW_STREAM_STATE_PAUSED) {
    const std::uint32_t id = pw_stream_get_node_id(impl->stream);
    if (id != PW_ID_ANY)
      impl->node_id.store(id, std::memory_order_release);
  }
}

// The server picked a format. Answer with the buffer layout StudioCast wants,
// otherwise the stream never gets data ports and a consumer receives nothing.
void OnParamChanged(void *data, std::uint32_t id, const struct spa_pod *param) {
  auto *impl = static_cast<PipeWireCameraNode::Impl *>(data);
  if (param == nullptr || id != SPA_PARAM_Format)
    return;

  std::uint32_t media_type = 0;
  std::uint32_t media_subtype = 0;
  if (spa_format_parse(param, &media_type, &media_subtype) < 0)
    return;
  if (media_type != SPA_MEDIA_TYPE_video ||
      media_subtype != SPA_MEDIA_SUBTYPE_raw)
    return;

  struct spa_video_info_raw raw {};
  if (spa_format_video_raw_parse(param, &raw) < 0)
    return;

  const auto stride = static_cast<std::int32_t>(impl->stride_bytes);
  const auto size = static_cast<std::int32_t>(impl->frame_bytes);

  std::uint8_t pod_buffer[1024];
  struct spa_pod_builder builder =
      SPA_POD_BUILDER_INIT(pod_buffer, sizeof(pod_buffer));
  const struct spa_pod *params[1];
  params[0] = static_cast<const struct spa_pod *>(spa_pod_builder_add_object(
      &builder, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
      SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 8),
      SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1), SPA_PARAM_BUFFERS_size,
      SPA_POD_Int(size), SPA_PARAM_BUFFERS_stride, SPA_POD_Int(stride),
      SPA_PARAM_BUFFERS_dataType,
      SPA_POD_CHOICE_FLAGS_Int(1 << SPA_DATA_MemPtr)));

  pw_stream_update_params(impl->stream, params, 1);
}

void OnStreamProcess(void *data) {
  auto *impl = static_cast<PipeWireCameraNode::Impl *>(data);
  struct pw_buffer *b = pw_stream_dequeue_buffer(impl->stream);
  if (!b)
    return;

  struct spa_buffer *buf = b->buffer;
  if (buf->n_datas == 0 || buf->datas[0].data == nullptr) {
    pw_stream_queue_buffer(impl->stream, b);
    return;
  }

  auto *out = static_cast<std::uint8_t *>(buf->datas[0].data);
  const std::size_t room =
      std::min<std::size_t>(buf->datas[0].maxsize, impl->frame_bytes);

  std::size_t written = 0;
  {
    std::lock_guard<std::mutex> lock(impl->frame_mu);
    if (impl->staged_valid && impl->staged.size() >= room) {
      std::memcpy(out, impl->staged.data(), room);
      impl->staged_pending = false;
      written = room;
    }
  }
  if (written == 0) {
    // No frame is ready yet. A black frame keeps the consumer's timing
    // steady instead of stalling it.
    std::memset(out, 0, room);
    written = room;
  } else {
    impl->frames_sent.fetch_add(1, std::memory_order_relaxed);
  }

  buf->datas[0].chunk->offset = 0;
  buf->datas[0].chunk->stride = static_cast<std::int32_t>(impl->stride_bytes);
  buf->datas[0].chunk->size = static_cast<std::uint32_t>(written);

  pw_stream_queue_buffer(impl->stream, b);
}

const struct pw_stream_events &StreamEvents() {
  static const struct pw_stream_events events = [] {
    struct pw_stream_events e {};
    e.version = PW_VERSION_STREAM_EVENTS;
    e.state_changed = OnStreamStateChanged;
    e.param_changed = OnParamChanged;
    e.process = OnStreamProcess;
    return e;
  }();
  return events;
}

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
  auto *impl = static_cast<PipeWireCameraNode::Impl *>(data);
  if (!type || std::strcmp(type, PW_TYPE_INTERFACE_Link) != 0 || !props)
    return;

  const std::uint32_t self = impl->node_id.load(std::memory_order_acquire);
  if (self == 0)
    return;

  // The camera node produces frames, so it is the output end of a consumer
  // link.
  if (!LinkEndpointMatches(props, PW_KEY_LINK_OUTPUT_NODE, self))
    return;

  std::lock_guard<std::mutex> lock(impl->links_mu);
  if (impl->counted_links.emplace(id, true).second)
    impl->consumer_count.fetch_add(1, std::memory_order_relaxed);
}

void OnRegistryGlobalRemove(void *data, std::uint32_t id) {
  auto *impl = static_cast<PipeWireCameraNode::Impl *>(data);
  std::lock_guard<std::mutex> lock(impl->links_mu);
  if (impl->counted_links.erase(id) > 0)
    impl->consumer_count.fetch_sub(1, std::memory_order_relaxed);
}

const struct pw_registry_events &RegistryEvents() {
  static const struct pw_registry_events events = [] {
    struct pw_registry_events e {};
    e.version = PW_VERSION_REGISTRY_EVENTS;
    e.global = OnRegistryGlobal;
    e.global_remove = OnRegistryGlobalRemove;
    return e;
  }();
  return events;
}

} // namespace

#endif // STUDIOCAST_HAVE_PIPEWIRE

PipeWireCameraNode::PipeWireCameraNode() : impl_(std::make_unique<Impl>()) {}

PipeWireCameraNode::~PipeWireCameraNode() { Stop(); }

#if !STUDIOCAST_HAVE_PIPEWIRE

bool PipeWireCameraNode::Start(const CameraNodeConfig &cfg,
                               std::string *error) {
  (void)cfg;
  const std::string msg = "StudioCast was built without PipeWire support "
                          "(STUDIOCAST_ENABLE_PIPEWIRE=OFF).";
  impl_->SetError(msg);
  if (error)
    *error = msg;
  return false;
}

void PipeWireCameraNode::Stop() {}

#else

bool PipeWireCameraNode::Start(const CameraNodeConfig &cfg,
                               std::string *error) {
  if (error)
    error->clear();
  Stop();

  const std::size_t bytes = CameraFrameBytes(cfg.width, cfg.height, cfg.format);
  if (bytes == 0 || cfg.fps <= 0) {
    const std::string msg = "Unsupported PipeWire camera format.";
    impl_->SetError(msg);
    if (error)
      *error = msg;
    return false;
  }

  EnsurePipeWireInitialized();

  impl_->cfg = cfg;
  impl_->frame_bytes = bytes;
  impl_->stride_bytes = CameraStrideBytes(cfg.width, cfg.format);
  impl_->node_id.store(0, std::memory_order_release);
  impl_->consumer_count.store(0, std::memory_order_relaxed);
  impl_->frames_sent.store(0, std::memory_order_relaxed);
  impl_->frames_dropped.store(0, std::memory_order_relaxed);
  impl_->SetError({});
  {
    std::lock_guard<std::mutex> lock(impl_->frame_mu);
    impl_->staged.assign(bytes, 0);
    impl_->staged_valid = false;
    impl_->staged_pending = false;
  }

  const std::string name =
      cfg.node_name.empty()
          ? std::string(studiocast::pw::kVirtualCameraNodeName)
          : cfg.node_name;
  const std::string description =
      cfg.node_description.empty()
          ? std::string(studiocast::pw::kVirtualCameraDescription)
          : cfg.node_description;

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
  if (!impl_->context || pw_thread_loop_start(impl_->loop) < 0) {
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

  struct pw_properties *props = pw_properties_new(
      PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Playback",
      PW_KEY_MEDIA_ROLE, "Camera", PW_KEY_MEDIA_CLASS, "Video/Source",
      PW_KEY_NODE_NAME, name.c_str(), PW_KEY_NODE_DESCRIPTION,
      description.c_str(), PW_KEY_NODE_VIRTUAL, "true", PW_KEY_NODE_DRIVER,
      "true", PW_KEY_APP_NAME, "StudioCast", nullptr);

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
  struct spa_video_info_raw info {};
  info.format = static_cast<enum spa_video_format>(SpaFormatFor(cfg.format));
  info.size = SPA_RECTANGLE(static_cast<std::uint32_t>(cfg.width),
                            static_cast<std::uint32_t>(cfg.height));
  info.framerate =
      SPA_FRACTION(static_cast<std::uint32_t>(cfg.fps), std::uint32_t{1});

  const struct spa_pod *params[1];
  params[0] =
      spa_format_video_raw_build(&builder, SPA_PARAM_EnumFormat, &info);

  // A Video/Source node waits for consumers, so it must not connect itself.
  //
  // It also drives its part of the graph: a frame arriving from the pipeline
  // is what starts a cycle. Without this the graph has no clock for the link
  // and a consumer receives nothing.
  const auto flags = static_cast<enum pw_stream_flags>(
      PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS |
      PW_STREAM_FLAG_DRIVER);

  const int rc = pw_stream_connect(impl_->stream, SPA_DIRECTION_OUTPUT,
                                     PW_ID_ANY, flags, params, 1);
  pw_thread_loop_unlock(impl_->loop);

  if (rc < 0) {
    const std::string msg =
        std::string("Could not connect the PipeWire camera node: ") +
        spa_strerror(rc);
    Stop();
    impl_->SetError(msg);
    if (error)
      *error = msg;
    return false;
  }

  impl_->running.store(true, std::memory_order_release);
  return true;
}

void PipeWireCameraNode::Stop() {
  impl_->running.store(false, std::memory_order_release);

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
  {
    std::lock_guard<std::mutex> lock(impl_->frame_mu);
    impl_->staged_valid = false;
    impl_->staged_pending = false;
  }
}

#endif // STUDIOCAST_HAVE_PIPEWIRE

bool PipeWireCameraNode::IsRunning() const {
  return impl_->running.load(std::memory_order_acquire);
}

bool PipeWireCameraNode::WriteFrame(const std::uint8_t *data,
                                    std::size_t bytes, std::string *error) {
  if (error)
    error->clear();
  if (!IsRunning()) {
    if (error)
      *error = "The PipeWire camera node is not running.";
    return false;
  }
  if (!data || bytes < impl_->frame_bytes) {
    if (error)
      *error = "The frame is smaller than the negotiated camera format.";
    return false;
  }

  {
    std::lock_guard<std::mutex> lock(impl_->frame_mu);
    if (impl_->staged_pending)
      impl_->frames_dropped.fetch_add(1, std::memory_order_relaxed);
    std::memcpy(impl_->staged.data(), data, impl_->frame_bytes);
    impl_->staged_valid = true;
    impl_->staged_pending = true;
  }

#if STUDIOCAST_HAVE_PIPEWIRE
  // The node drives the graph, so a new frame starts a cycle.
  if (impl_->stream)
    pw_stream_trigger_process(impl_->stream);
#endif
  return true;
}

std::uint32_t PipeWireCameraNode::NodeId() const {
  return impl_->node_id.load(std::memory_order_acquire);
}

int PipeWireCameraNode::ConsumerCount() const {
  return impl_->consumer_count.load(std::memory_order_relaxed);
}

std::uint64_t PipeWireCameraNode::FramesSent() const {
  return impl_->frames_sent.load(std::memory_order_relaxed);
}

std::uint64_t PipeWireCameraNode::FramesDropped() const {
  return impl_->frames_dropped.load(std::memory_order_relaxed);
}

std::string PipeWireCameraNode::LastError() const { return impl_->Error(); }

} // namespace studiocast::video::pw_backend
