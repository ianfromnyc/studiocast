#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "core/video/effects/broadcast_effects.h"
#include "core/video/pipewire/pipewire_camera_node.h"
#include "core/video/v4l2_capture.h"
#include "core/video/v4l2_writer.h"

namespace studiocast::video {

enum class CaptureMode {
  // Use the requested `width`/`height` (must be > 0).
  requested,

  // Auto-select a good capture mode. `width`/`height` may be <= 0 (sentinel).
  auto_best,
};

enum class ScalingBackendPreference {
  auto_select,
  cpu,
  gpu,
};

struct CameraPipelineConfig {
  std::string input_device;  // e.g. /dev/video0
  std::string output_device; // e.g. /dev/video10 (v4l2loopback)

  CaptureMode capture_mode = CaptureMode::requested;

  int width = 1280;
  int height = 720;
  int fps = 30;

  PixelFormat output_format = PixelFormat::rgb24;

  bool prefer_mjpeg = true;

  // Output scaling backend selection.
  // - auto_select: use GPU scaling when available, otherwise CPU.
  // - cpu: force CPU scaling.
  // - gpu: prefer GPU scaling.
  //
  // Note: CPU resize can be hard-disabled via `allow_cpu_resize` to prevent
  // silent high-latency scaling paths.
  ScalingBackendPreference scaling_backend =
      ScalingBackendPreference::auto_select;

  // When false, the pipeline will NOT perform CPU resizing when output
  // dimensions differ from the source frame.
  //
  // If an output resize is required and GPU resize is unavailable, the
  // pipeline reports an explicit error and stops instead of silently
  // spending tens of milliseconds per frame.
  bool allow_cpu_resize = true;

  // Mirror every processed frame onto a native PipeWire Video/Source node
  // named "studiocast_camera", beside the v4l2loopback output.
  bool pipewire_output = false;

  studiocast::video::effects::BroadcastCameraEffects effects{};
};

struct CameraPipelineStatus {
  bool running = false;
  bool starting = false;

  std::string input_device;
  std::string output_device;

  CaptureFormat capture{};
  ActualFormat output{};

  // Runtime capture fallback state. Common values: "none" or
  // "raw_after_mjpeg_decode_failure".
  std::string capture_fallback_state = "none";
  std::string capture_fallback_reason;

  // Native PipeWire camera node: "off", "starting", "running", or the text
  // of the last failure. A node that runs but cannot take frames reports
  // that text, not "running".
  std::string pipewire_output_state = "off";
  std::uint32_t pipewire_node_id = 0;
  int pipewire_consumer_count = 0;

  // Active output-scaling backend.
  // Common values: "cpu", "gpu:maxine", "gpu:open_cuda" (empty when idle)
  std::string scaling_backend_active;
  CaptureFormat scaling_from{};
  ActualFormat scaling_to{};

  int frame_index = 0;

  struct MsPerFrame {
    double capture = 0.0;
    double scale = 0.0;
    double effects = 0.0;
    double write = 0.0;
  };

  // Rolling averages of per-frame stage times (milliseconds).
  MsPerFrame ms_per_frame{};
  double fps_actual = 0.0;
  int perf_sample_frames = 0;

  // Optional debug stats (not emitted in status JSON unless explicitly
  // enabled).
  struct Debug {
    double latency_ms = 0.0;
    std::uint64_t capture_sequence = 0;
    int dropped_capture_frames = 0;

    // Best-effort counters for diagnosing v4l2loopback consumer negotiation.
    int output_format_changes = 0;
    int output_refresh_failures = 0;
    int output_write_recoveries = 0;

    // Output pacing/jitter diagnostics (useful for browser/WebRTC capture).
    double pace_sleep_ms = 0.0;
    double pace_late_ms = 0.0;
    std::uint64_t pace_sleeps = 0;
    std::uint64_t pace_late_frames = 0;
    std::uint64_t pace_resyncs = 0;
  } debug{};

  // Optional Open CUDA transfer counters (emitted in status JSON only when
  // STUDIOCAST_DEBUG_OPEN_CUDA_TRANSFERS=1 (or legacy
  // STUDIOCAST_DEBUG_CUDA_UPLOADS=1) is set for the daemon).
  struct OpenCudaTransfers {
    std::uint64_t active_frames = 0;
    std::uint64_t upload_calls = 0;
    std::uint64_t download_calls = 0;
    std::uint64_t final_download_calls = 0;
    std::uint64_t cpu_continuation_download_calls = 0;
    std::uint64_t alpha_download_calls = 0;
    std::uint64_t matte_frame_upload_calls = 0;
    std::uint64_t standalone_scaler_upload_calls = 0;
    std::uint64_t standalone_scaler_download_calls = 0;
    std::uint64_t denoise_tensor_upload_calls = 0;
    std::uint64_t denoise_tensor_download_calls = 0;
    std::uint64_t forced_sync_calls = 0;
    std::uint64_t cpu_tail_stage_calls = 0;
    std::uint64_t cpu_tail_key_light_calls = 0;
    std::uint64_t cpu_tail_auto_frame_calls = 0;
    std::uint64_t cpu_tail_auto_frame_face_tracking_calls = 0;
    std::uint64_t cpu_tail_auto_frame_matte_tracking_calls = 0;
    std::uint64_t cpu_tail_auto_frame_cpu_crop_calls = 0;
    std::uint64_t cpu_tail_denoise_calls = 0;
  } open_cuda_transfers{};

  // Optional Maxine transfer counters (emitted in status JSON only when
  // STUDIOCAST_DEBUG_MAXINE_TRANSFERS=1 is set for the daemon).
  struct MaxineTransfers {
    std::uint64_t active_frames = 0;
    std::uint64_t rgb_to_bgr_calls = 0;
    std::uint64_t upload_calls = 0;
    std::uint64_t green_screen_calls = 0;
    std::uint64_t duplicate_green_screen_calls = 0;
    std::uint64_t shared_green_screen_matte_reuse_calls = 0;
    std::uint64_t shared_green_screen_matte_incompatible_calls = 0;
    std::uint64_t shared_green_screen_input_incompatible_calls = 0;
    std::uint64_t download_calls = 0;
    std::uint64_t final_download_calls = 0;
    std::uint64_t cpu_continuation_download_calls = 0;
    std::uint64_t bgr_to_rgb_calls = 0;
    std::uint64_t deferred_readbacks = 0;
    std::uint64_t forced_sync_calls = 0;
    std::uint64_t standalone_scaler_upload_calls = 0;
    std::uint64_t standalone_scaler_download_calls = 0;
  } maxine_transfers{};

  // Debug/status for effects.
  std::string
      effects_backends; // e.g. "mirror:builtin,virtual_background.blur:maxine"
  std::string
      effects_note; // e.g. "Maxine requested but unavailable; effects disabled"

  struct DegradedEffect {
    bool active = false;
    std::string effect_id;
    std::string backend;
    std::string reason;
    std::string state;
    int failure_count = 0;
    int cooldown_frames = 0;
  };

  // Cached on effect state transitions/config changes only. When active, the
  // effect has been bypassed and the frame loop is continuing pass-through.
  DegradedEffect degraded_effect{};

  std::string last_error;
};

class OptionalEffectBreaker {
public:
  static constexpr int kInitialCooldownFrames = 30;
  static constexpr int kMaxCooldownFrames = 300;

  bool active() const { return active_; }
  bool AllowsAttempt(std::uint64_t frame_index) const;
  void Reset();
  void OnFailure(std::string_view effect, std::string_view effect_backend,
                 std::string failure_reason, std::uint64_t frame_index,
                 int order);
  bool OnSuccess();
  bool MarkRetryReadyIfDue(std::uint64_t frame_index);
  CameraPipelineStatus::DegradedEffect
  ToStatus(std::uint64_t frame_index) const;

  const std::string &effect_id() const { return effect_id_; }
  const std::string &backend() const { return backend_; }
  const std::string &reason() const { return reason_; }
  int failure_count() const { return failure_count_; }
  int trip_order() const { return trip_order_; }

private:
  static int CooldownFramesForFailureCount(int failure_count);

  bool active_ = false;
  std::string effect_id_;
  std::string backend_;
  std::string reason_;
  int failure_count_ = 0;
  int cooldown_frames_ = 0;
  std::uint64_t retry_frame_index_ = 0;
  int trip_order_ = 0;
  bool retry_ready_published_ = false;
};

namespace internal {

// The state of the PipeWire camera node that runs now, as far as the plan
// below needs it.
struct PipeWireNodeState {
  bool running = false;
  int width = 0;
  int height = 0;
  int fps = 0;
  PixelFormat format = PixelFormat::rgb24;
};

// What the camera node needs next.
struct PipeWireNodePlan {
  enum class Action {
    // The node already matches the output, or there is no output format yet.
    keep,
    // No node is wanted; take down whatever runs.
    stop,
    // Start a node for `node`, in place of whatever runs.
    restart,
  };

  Action action = Action::keep;
  studiocast::video::pw_backend::CameraNodeConfig node;
};

// The rule the pipeline follows for its camera node. It reads state only, so
// the pipeline can decide under its mutex and do the work without it.
//
// `wanted` is the configured output preference, `output` the format the
// loopback negotiated, and `current` the node that runs now. An output with no
// size yet gives `keep`, because there is nothing to offer a consumer.
PipeWireNodePlan PlanPipeWireNode(bool wanted, const ActualFormat &output,
                                  const PipeWireNodeState &current);

// The `pipewire_output_state` text of the pipeline status.
//
// `wanted` is the configured output preference, `has_node` says whether a node
// runs now, and `error` is the last failure of that node, empty when there was
// none.
std::string PipeWireOutputStateText(bool wanted, bool has_node,
                                    const std::string &error);

} // namespace internal

class CameraPipelineRunner {
public:
  virtual ~CameraPipelineRunner() = default;

  virtual bool Start(const CameraPipelineConfig &cfg, std::string *error) = 0;
  virtual void Stop() = 0;
  virtual bool EnsureOutputOpen(const CameraPipelineConfig &cfg,
                                std::string *error) = 0;
  virtual void CloseOutput() = 0;
  virtual CameraPipelineStatus Status() const = 0;
  virtual void SetEffects(
      const studiocast::video::effects::BroadcastCameraEffects &effects) = 0;
  virtual void SetMirrorEnabled(bool enabled) = 0;
};

class CameraPipeline final : public CameraPipelineRunner {
public:
  CameraPipeline() = default;
  ~CameraPipeline();

  CameraPipeline(const CameraPipeline &) = delete;
  CameraPipeline &operator=(const CameraPipeline &) = delete;

  bool Start(const CameraPipelineConfig &cfg, std::string *error) override;
  void Stop() override;

  // Opens (and keeps open) the v4l2loopback output device without starting
  // camera capture / processing.
  //
  // This is important when v4l2loopback is loaded with exclusive_caps=1:
  // many applications will not list the device as a capture source unless a
  // producer has it open.
  bool EnsureOutputOpen(const CameraPipelineConfig &cfg,
                        std::string *error) override;
  void CloseOutput() override;

  // Decides what the native PipeWire camera node needs. The caller must hold
  // `mu_`, because the answer comes from the negotiated output format and the
  // node that runs now.
  internal::PipeWireNodePlan PlanPipeWireOutputLocked() const;

  // Carries out a plan. The caller must NOT hold `mu_`: starting and stopping
  // a node talks to the PipeWire server, which can block. Only the swap of the
  // node pointer and its state takes the mutex, and only for that swap.
  void ApplyPipeWireOutputPlan(const internal::PipeWireNodePlan &plan);

  // Hands one processed frame to the native camera node. It never blocks and
  // never fails the pipeline.
  //
  // The caller must NOT hold `mu_`. This takes the mutex only to copy the
  // node reference out and, if the write fails, to report the error.
  void PublishToPipeWire(const std::uint8_t *data, std::size_t bytes);

  CameraPipelineStatus Status() const override;

  // Live update of effects while running.
  void SetEffects(const studiocast::video::effects::BroadcastCameraEffects
                      &effects) override;

  // Convenience for legacy callers.
  void SetMirrorEnabled(bool enabled) override;

private:
  // Opens (or reuses) the loopback writer.
  //
  // If `out_opened_or_renegotiated` is non-null, it will be set to true when we
  // actually performed an open/renegotiation (i.e. the output may have been
  // reset), and false when the existing writer was reused without changes.
  bool OpenOutputLocked(const std::string &outDev, int width, int height,
                        int fps, PixelFormat output_format, bool strict_fps,
                        bool *out_opened_or_renegotiated, std::string *error);

  void ThreadMain(CameraPipelineConfig cfg);

  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::thread th_;
  std::atomic_bool stop_{false};

  bool running_ = false;
  bool starting_ = false;
  bool start_notified_ = false;

  std::string input_device_;
  std::string output_device_;
  CaptureFormat capture_{};
  ActualFormat output_{};
  std::string capture_fallback_state_ = "none";
  std::string capture_fallback_reason_;
  std::string scaling_backend_active_;
  CaptureFormat scaling_from_{};
  ActualFormat scaling_to_{};
  int frame_index_ = 0;

  CameraPipelineStatus::MsPerFrame ms_per_frame_{};
  double fps_actual_ = 0.0;
  int perf_sample_frames_ = 0;

  CameraPipelineStatus::Debug debug_{};

  CameraPipelineStatus::OpenCudaTransfers open_cuda_transfers_{};
  CameraPipelineStatus::MaxineTransfers maxine_transfers_{};

  // Effects: updated live by SetEffects.
  mutable std::mutex effects_mu_;
  studiocast::video::effects::BroadcastCameraEffects effects_{};

  // Effect runtime info (written by pipeline thread when effects chain
  // changes).
  std::string effects_backends_;
  std::string effects_note_;
  CameraPipelineStatus::DegradedEffect degraded_effect_{};

  std::string last_error_;

  // Keep the output open across starts/stops to avoid v4l2loopback edge
  // cases (especially with exclusive_caps=1) and to make the virtual
  // camera visible to apps even when we're idle.
  V4l2Writer writer_;
  std::string writer_device_;

  // Optional second output. It carries the same buffer that goes to
  // v4l2loopback, so it costs one memory copy a frame.
  //
  // The pointer is shared because the frame thread publishes with `mu_`
  // released. It holds a reference for the length of one write, so a node that
  // the supervisor thread swaps out lives until that write is done.
  //
  // `mu_` guards both the pointer and the error.
  std::shared_ptr<studiocast::video::pw_backend::PipeWireCameraNode> pw_node_;
  std::string pw_node_error_;
  bool pw_output_wanted_ = false;

  // Format the running node negotiated, so a renegotiated v4l2loopback format
  // restarts the node instead of sending frames of the wrong size.
  int pw_node_width_ = 0;
  int pw_node_height_ = 0;
  int pw_node_fps_ = 0;
  PixelFormat pw_node_format_ = PixelFormat::rgb24;

  // Idle keepalive frames: while the heavy pipeline is stopped, periodically
  // write a black frame to keep consumers from closing/re-opening due to a
  // lack of frames (which can trigger consumer-driven thrash).
  std::chrono::steady_clock::time_point last_keepalive_frame_at_{};
  ActualFormat keepalive_format_{};
  std::vector<std::uint8_t> keepalive_frame_;
};

} // namespace studiocast::video
