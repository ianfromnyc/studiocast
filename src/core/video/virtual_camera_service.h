#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "core/video/camera_pipeline.h"

namespace studiocast::video {

struct VirtualCameraServiceConfig {
  // Underlying processing pipeline config.
  CameraPipelineConfig pipeline;

  // If false, processing will never start even if a consumer opens the
  // virtual camera. The v4l2loopback output is still kept open so the
  // device stays discoverable.
  bool enabled = true;

  // How often we scan for consumers of the virtual camera.
  int consumer_poll_ms = 250;

  // Debounce consumer detection on start. A consumer must be continuously
  // present for at least this long before we start the heavy camera pipeline.
  // This reduces start/stop thrashing when apps probe cameras via open/close.
  int start_grace_ms = 300;

  // After the last consumer disconnects, we wait this long before stopping the
  // pipeline. This avoids rapid start/stop flapping when apps probe cameras.
  int stop_grace_ms = 1000;

  // Once the pipeline starts running, keep it running for at least this long
  // (while enabled) even if consumer detection briefly drops out. This helps
  // absorb transient /proc scan gaps and consumer renegotiation windows.
  int min_run_ms = 1500;

  // If true, the pipeline runs even when there are no consumers of the virtual
  // camera. Useful for debugging or when you want the virtual camera to always
  // be "hot".
  bool always_on = false;
};

struct VirtualCameraServiceStatus {
  bool service_running = false;

  // True when the configured/selected loopback node exists. `available` means
  // the daemon has no current output-open/keepalive error for that node.
  bool virtual_device_present = false;
  bool virtual_device_available = false;
  std::string virtual_device_error;

  // True if at least one external process has the virtual camera open.
  bool consumer_present = false;
  int consumer_count = 0;
  std::string consumer_error;

  // Supervisor state for daemon/GUI consumers. Normal idle-no-consumer is not a
  // failure.
  bool pipeline_active_needed = false;
  std::string pipeline_state;
  std::string pipeline_idle_reason;

  // Supervisor diagnostics (counts reset on Start()).
  std::uint64_t pipeline_start_attempts = 0;
  std::uint64_t pipeline_starts = 0;
  std::uint64_t pipeline_start_failures = 0;
  std::uint64_t pipeline_stops = 0;
  std::uint64_t pipeline_config_restarts = 0;

  // Best-effort thrash / stabilization visibility (helps diagnose
  // consumer-driven open/close probing behavior in apps like Discord).
  bool stabilizing = false;
  int thrash_events_10s = 0;

  // Last pipeline transition and how long ago it happened.
  std::string last_transition;
  std::int64_t last_transition_ms_ago = -1;

  // If >= 0, time until the next pipeline start retry (backoff).
  std::int64_t next_start_retry_ms = -1;

  CameraPipelineStatus pipeline;
  std::string last_error;
};

struct VideoConsumerSnapshot {
  bool present = false;
  int count = 0;
  std::string error;
};

struct VirtualCameraServiceHooks {
  std::function<void(std::chrono::milliseconds)> sleep_for;
  std::function<std::unique_ptr<CameraPipelineRunner>()> create_pipeline;
  std::function<std::string(std::string *)> choose_output_device;
  std::function<bool(const std::string &, std::string *)> output_device_exists;
  std::function<VideoConsumerSnapshot(const std::string &, int)>
      detect_consumers;
};

// Supervises the camera pipeline and starts/stops it based on whether any
// process is consuming the v4l2loopback device (consumer-driven start/stop).
//
// This lives in studiocast_core (no Qt deps) so it can be reused by the GUI or
// a daemon.
class VirtualCameraService final {
public:
  VirtualCameraService();
  explicit VirtualCameraService(VirtualCameraServiceHooks hooks);
  ~VirtualCameraService();

  VirtualCameraService(const VirtualCameraService &) = delete;
  VirtualCameraService &operator=(const VirtualCameraService &) = delete;

  // Starts the supervisor thread (does not necessarily start the pipeline
  // immediately).
  bool Start(const VirtualCameraServiceConfig &cfg, std::string *error);
  void Stop();

  // Thread-safe update of config. Effect changes apply live; device/size/fps
  // changes cause a restart on the next poll cycle.
  void UpdateConfig(const VirtualCameraServiceConfig &cfg);

  // Returns the most recently applied configuration (thread-safe snapshot).
  VirtualCameraServiceConfig Config() const;

  VirtualCameraServiceStatus Status() const;

private:
  void ThreadMain();
  void SleepFor(std::chrono::milliseconds d) const;
  std::string ChooseOutputDevice(std::string *error) const;
  bool OutputDeviceExists(const std::string &path, std::string *error) const;
  VideoConsumerSnapshot DetectConsumers(const std::string &dev,
                                        int excludePid) const;

  static bool NeedsPipelineRestart(const CameraPipelineConfig &a,
                                   const CameraPipelineConfig &b);

  mutable std::mutex mu_;
  std::thread th_;
  std::atomic_bool stop_{false};

  bool running_ = false;
  bool virtual_device_present_ = false;
  bool virtual_device_available_ = false;
  std::string virtual_device_error_;
  bool consumer_present_ = false;
  int consumer_count_ = 0;
  std::string consumer_error_;
  bool pipeline_active_needed_ = false;
  std::string pipeline_state_;
  std::string pipeline_idle_reason_;
  std::string last_error_;

  VirtualCameraServiceHooks hooks_{};
  VirtualCameraServiceConfig cfg_;

  std::chrono::steady_clock::time_point last_consumer_seen_{};
  std::chrono::steady_clock::time_point next_start_retry_{};

  // Supervisor counters / diagnostics (protected by mu_).
  std::uint64_t pipeline_start_attempts_ = 0;
  std::uint64_t pipeline_starts_ = 0;
  std::uint64_t pipeline_start_failures_ = 0;
  std::uint64_t pipeline_stops_ = 0;
  std::uint64_t pipeline_config_restarts_ = 0;

  bool stabilizing_ = false;
  int thrash_events_10s_ = 0;

  std::string last_transition_;
  std::chrono::steady_clock::time_point last_transition_at_{};

  std::unique_ptr<CameraPipelineRunner> pipeline_;
};

} // namespace studiocast::video
