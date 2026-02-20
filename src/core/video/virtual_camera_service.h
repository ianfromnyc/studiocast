#pragma once

#include <atomic>
#include <chrono>
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

  // True if at least one external process has the virtual camera open.
  bool consumer_present = false;
  int consumer_count = 0;

  CameraPipelineStatus pipeline;
  std::string last_error;
};

// Supervises the camera pipeline and starts/stops it based on whether any
// process is consuming the v4l2loopback device (consumer-driven start/stop).
//
// This lives in studiocast_core (no Qt deps) so it can be reused by the GUI or
// a daemon.
class VirtualCameraService final {
public:
  VirtualCameraService() = default;
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

  static bool NeedsPipelineRestart(const CameraPipelineConfig &a,
                                   const CameraPipelineConfig &b);

  mutable std::mutex mu_;
  std::thread th_;
  std::atomic_bool stop_{false};

  bool running_ = false;
  bool consumer_present_ = false;
  int consumer_count_ = 0;
  std::string last_error_;

  VirtualCameraServiceConfig cfg_;

  std::chrono::steady_clock::time_point last_consumer_seen_{};
  std::chrono::steady_clock::time_point next_start_retry_{};

  CameraPipeline pipeline_;
};

} // namespace studiocast::video
