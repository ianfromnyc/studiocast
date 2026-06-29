#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include "core/video/virtual_camera_service.h"

namespace {

using studiocast::video::CameraPipelineConfig;
using studiocast::video::CameraPipelineRunner;
using studiocast::video::CameraPipelineStatus;
using studiocast::video::VideoConsumerSnapshot;
using studiocast::video::VirtualCameraService;
using studiocast::video::VirtualCameraServiceConfig;
using studiocast::video::VirtualCameraServiceHooks;

using namespace std::chrono_literals;

bool WaitUntil(const std::function<bool()> &pred,
               std::chrono::milliseconds timeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (pred())
      return true;
    std::this_thread::sleep_for(1ms);
  }
  return pred();
}

class FakeCameraPipeline final : public CameraPipelineRunner {
public:
  bool Start(const CameraPipelineConfig &cfg, std::string *error) override {
    start_calls.fetch_add(1, std::memory_order_relaxed);
    if (fail_start.load(std::memory_order_relaxed)) {
      if (error)
        *error = "synthetic camera start failure";
      std::lock_guard<std::mutex> lock(mu_);
      status_.running = false;
      status_.starting = false;
      status_.last_error = "synthetic camera start failure";
      return false;
    }

    if (error)
      error->clear();
    std::lock_guard<std::mutex> lock(mu_);
    status_.running = true;
    status_.starting = false;
    status_.input_device = cfg.input_device.empty()
                               ? std::string("/dev/video-real")
                               : cfg.input_device;
    status_.output_device = cfg.output_device;
    status_.last_error.clear();
    return true;
  }

  void Stop() override {
    stop_calls.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(mu_);
    status_.running = false;
    status_.starting = false;
  }

  bool EnsureOutputOpen(const CameraPipelineConfig &cfg,
                        std::string *error) override {
    ensure_output_calls.fetch_add(1, std::memory_order_relaxed);
    if (fail_ensure_output.load(std::memory_order_relaxed)) {
      if (error)
        *error = "synthetic output open failure";
      return false;
    }

    if (error)
      error->clear();
    std::lock_guard<std::mutex> lock(mu_);
    status_.output_device = cfg.output_device;
    status_.output.width = cfg.width;
    status_.output.height = cfg.height;
    status_.output.fps = cfg.fps;
    status_.output.fps_num = 1;
    status_.output.fps_den = cfg.fps;
    return true;
  }

  void CloseOutput() override {
    close_output_calls.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(mu_);
    status_.output_device.clear();
    status_.output = {};
  }

  CameraPipelineStatus Status() const override {
    std::lock_guard<std::mutex> lock(mu_);
    return status_;
  }

  void SetEffects(
      const studiocast::video::effects::BroadcastCameraEffects &) override {
    set_effects_calls.fetch_add(1, std::memory_order_relaxed);
  }

  void SetMirrorEnabled(bool) override {}

  std::atomic<int> start_calls{0};
  std::atomic<int> stop_calls{0};
  std::atomic<int> ensure_output_calls{0};
  std::atomic<int> close_output_calls{0};
  std::atomic<int> set_effects_calls{0};
  std::atomic<bool> fail_start{false};
  std::atomic<bool> fail_ensure_output{false};

private:
  mutable std::mutex mu_;
  CameraPipelineStatus status_{};
};

struct ServiceHarness {
  FakeCameraPipeline *pipeline = nullptr;
  std::atomic<bool> consumer_present{false};
  std::atomic<bool> device_exists{true};
  std::atomic<bool> detection_error{false};

  VirtualCameraServiceHooks Hooks(std::chrono::milliseconds sleep = 1ms) {
    VirtualCameraServiceHooks hooks;
    hooks.sleep_for = [sleep](std::chrono::milliseconds) {
      std::this_thread::sleep_for(sleep);
    };
    hooks.create_pipeline = [&] {
      auto fake = std::make_unique<FakeCameraPipeline>();
      pipeline = fake.get();
      return fake;
    };
    hooks.choose_output_device = [&](std::string *error) {
      if (!device_exists.load(std::memory_order_relaxed)) {
        if (error)
          *error = "synthetic loopback missing";
        return std::string();
      }
      if (error)
        error->clear();
      return std::string("/tmp/studiocast-test-video10");
    };
    hooks.output_device_exists = [&](const std::string &, std::string *error) {
      if (!device_exists.load(std::memory_order_relaxed)) {
        if (error)
          *error = "synthetic loopback missing";
        return false;
      }
      if (error)
        error->clear();
      return true;
    };
    hooks.detect_consumers = [&](const std::string &, int) {
      VideoConsumerSnapshot out;
      if (detection_error.load(std::memory_order_relaxed)) {
        out.error = "synthetic consumer scan failure";
        return out;
      }
      out.present = consumer_present.load(std::memory_order_relaxed);
      out.count = out.present ? 1 : 0;
      return out;
    };
    return hooks;
  }
};

VirtualCameraServiceConfig TestConfig() {
  VirtualCameraServiceConfig cfg;
  cfg.enabled = true;
  cfg.consumer_poll_ms = 1;
  cfg.start_grace_ms = 0;
  cfg.stop_grace_ms = 0;
  cfg.min_run_ms = 0;
  cfg.pipeline.output_device = "/tmp/studiocast-test-video10";
  cfg.pipeline.width = 640;
  cfg.pipeline.height = 480;
  cfg.pipeline.fps = 30;
  return cfg;
}

bool TestVideoPipelineDoesNotStartWithoutConsumer() {
  ServiceHarness h;
  VirtualCameraService service(h.Hooks());
  auto cfg = TestConfig();

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  const bool idle = WaitUntil(
      [&] {
        const auto status = service.Status();
        return status.pipeline_state == "idle_no_consumer" &&
               status.pipeline_idle_reason == "no_consumer" &&
               !status.pipeline_active_needed && !status.pipeline.running &&
               status.virtual_device_available &&
               h.pipeline->ensure_output_calls.load() > 0;
      },
      250ms);
  const auto status = service.Status();
  const int starts = h.pipeline->start_calls.load();
  service.Stop();

  if (!idle || starts != 0) {
    std::cerr << "video pipeline started or failed to idle without consumer; "
              << "starts=" << starts << " state='" << status.pipeline_state
              << "' idle='" << status.pipeline_idle_reason
              << "' virtual_available=" << status.virtual_device_available
              << "\n";
    return false;
  }
  return true;
}

bool TestVideoPipelineStartsWhenConsumerAppears() {
  ServiceHarness h;
  VirtualCameraService service(h.Hooks());
  auto cfg = TestConfig();

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  if (!WaitUntil(
          [&] { return service.Status().pipeline_state == "idle_no_consumer"; },
          250ms)) {
    std::cerr << "video pipeline did not reach no-consumer idle state\n";
    service.Stop();
    return false;
  }

  h.consumer_present.store(true, std::memory_order_relaxed);
  const bool started = WaitUntil(
      [&] {
        const auto status = service.Status();
        return status.consumer_present && status.pipeline.running &&
               status.pipeline_active_needed &&
               status.pipeline_state == "running" &&
               h.pipeline->start_calls.load() == 1;
      },
      250ms);
  const auto status = service.Status();
  service.Stop();

  if (!started) {
    std::cerr << "video pipeline did not start after consumer appeared; "
              << "starts=" << h.pipeline->start_calls.load() << " state='"
              << status.pipeline_state
              << "' running=" << status.pipeline.running << "\n";
    return false;
  }
  return true;
}

bool TestVideoPipelineStopsWhenConsumerDisappears() {
  ServiceHarness h;
  h.consumer_present.store(true, std::memory_order_relaxed);
  VirtualCameraService service(h.Hooks());
  auto cfg = TestConfig();
  cfg.stop_grace_ms = 20;

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  if (!WaitUntil([&] { return service.Status().pipeline.running; }, 250ms)) {
    std::cerr << "video pipeline did not start for initial consumer\n";
    service.Stop();
    return false;
  }

  h.consumer_present.store(false, std::memory_order_relaxed);
  const bool stopped = WaitUntil(
      [&] {
        const auto status = service.Status();
        return h.pipeline->stop_calls.load() >= 1 && !status.pipeline.running &&
               status.pipeline_state == "idle_no_consumer" &&
               !status.pipeline_active_needed;
      },
      500ms);
  const auto status = service.Status();
  service.Stop();

  if (!stopped) {
    std::cerr << "video pipeline did not stop after consumer disappeared; "
              << "stops=" << h.pipeline->stop_calls.load() << " state='"
              << status.pipeline_state
              << "' running=" << status.pipeline.running << "\n";
    return false;
  }
  return true;
}

bool TestVideoGraceWindowAbsorbsConsumerFlapping() {
  ServiceHarness h;
  h.consumer_present.store(true, std::memory_order_relaxed);
  VirtualCameraService service(h.Hooks());
  auto cfg = TestConfig();
  cfg.stop_grace_ms = 80;

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  if (!WaitUntil([&] { return service.Status().pipeline.running; }, 250ms)) {
    std::cerr << "video pipeline did not start before flap test\n";
    service.Stop();
    return false;
  }

  for (int i = 0; i < 3; ++i) {
    h.consumer_present.store(false, std::memory_order_relaxed);
    std::this_thread::sleep_for(20ms);
    const auto absent = service.Status();
    if (!absent.pipeline.running ||
        h.pipeline->stop_calls.load(std::memory_order_relaxed) != 0) {
      std::cerr << "video pipeline stopped inside grace window; i=" << i
                << " stops=" << h.pipeline->stop_calls.load() << " state='"
                << absent.pipeline_state << "'\n";
      service.Stop();
      return false;
    }

    h.consumer_present.store(true, std::memory_order_relaxed);
    if (!WaitUntil([&] { return service.Status().consumer_present; }, 100ms)) {
      std::cerr << "video consumer did not recover during flap test\n";
      service.Stop();
      return false;
    }
  }

  h.consumer_present.store(false, std::memory_order_relaxed);
  const bool stopped = WaitUntil(
      [&] {
        return h.pipeline->stop_calls.load(std::memory_order_relaxed) == 1 &&
               service.Status().pipeline_state == "idle_no_consumer";
      },
      500ms);
  service.Stop();

  if (!stopped || h.pipeline->start_calls.load() != 1) {
    std::cerr << "video pipeline churned during consumer flapping; starts="
              << h.pipeline->start_calls.load()
              << " stops=" << h.pipeline->stop_calls.load() << "\n";
    return false;
  }
  return true;
}

bool TestVideoConsumerDetectionErrorSurfacesWithoutStarting() {
  ServiceHarness h;
  h.detection_error.store(true, std::memory_order_relaxed);
  VirtualCameraService service(h.Hooks());
  auto cfg = TestConfig();

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  const bool surfaced = WaitUntil(
      [&] {
        const auto status = service.Status();
        return status.pipeline_state == "consumer_detection_error" &&
               status.consumer_error.find("synthetic consumer scan failure") !=
                   std::string::npos &&
               h.pipeline->start_calls.load() == 0;
      },
      250ms);
  const auto status = service.Status();
  service.Stop();

  if (!surfaced) {
    std::cerr << "consumer detection error was not surfaced cleanly; state='"
              << status.pipeline_state << "' consumer_error='"
              << status.consumer_error
              << "' starts=" << h.pipeline->start_calls.load() << "\n";
    return false;
  }
  return true;
}

bool TestVideoStartFailureBacksOff() {
  ServiceHarness h;
  h.consumer_present.store(true, std::memory_order_relaxed);
  VirtualCameraService service(h.Hooks());
  auto cfg = TestConfig();
  h.pipeline->fail_start.store(true, std::memory_order_relaxed);

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  const bool backedOff = WaitUntil(
      [&] {
        const auto status = service.Status();
        return status.pipeline_state == "backing_off" &&
               status.next_start_retry_ms > 0 &&
               status.last_error.find("Pipeline start failed") !=
                   std::string::npos &&
               h.pipeline->start_calls.load() == 1;
      },
      250ms);
  const int startsAfterFirstFailure = h.pipeline->start_calls.load();
  std::this_thread::sleep_for(40ms);
  const int startsAfterWait = h.pipeline->start_calls.load();
  service.Stop();

  if (!backedOff || startsAfterFirstFailure != startsAfterWait) {
    std::cerr << "video start failure did not back off; backedOff=" << backedOff
              << " starts_first=" << startsAfterFirstFailure
              << " starts_later=" << startsAfterWait << "\n";
    return false;
  }
  return true;
}

bool TestVideoStartFailureClearsAfterRecovery() {
  ServiceHarness h;
  h.consumer_present.store(true, std::memory_order_relaxed);
  VirtualCameraService service(h.Hooks());
  auto cfg = TestConfig();
  h.pipeline->fail_start.store(true, std::memory_order_relaxed);

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  if (!WaitUntil(
          [&] { return service.Status().pipeline_state == "backing_off"; },
          250ms)) {
    std::cerr << "video pipeline did not enter backoff before recovery test\n";
    service.Stop();
    return false;
  }

  h.pipeline->fail_start.store(false, std::memory_order_relaxed);
  auto cfg2 = cfg;
  cfg2.pipeline.width = 800;
  service.UpdateConfig(cfg2);

  // The built-in retry backoff is two seconds; wait only until the retry opens.
  const bool recovered = WaitUntil(
      [&] {
        const auto status = service.Status();
        return status.pipeline_state == "running" &&
               status.last_error.empty() && h.pipeline->start_calls.load() >= 2;
      },
      2500ms);
  const auto status = service.Status();
  service.Stop();

  if (!recovered) {
    std::cerr << "video pipeline recovery left stale error; state='"
              << status.pipeline_state << "' last_error='" << status.last_error
              << "' starts=" << h.pipeline->start_calls.load() << "\n";
    return false;
  }
  return true;
}

bool TestVideoConfigRestartTransitionNameIsStable() {
  ServiceHarness h;
  h.consumer_present.store(true, std::memory_order_relaxed);
  VirtualCameraService service(h.Hooks(5ms));
  auto cfg = TestConfig();

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  if (!WaitUntil([&] { return service.Status().pipeline.running; }, 250ms)) {
    std::cerr << "video pipeline did not start before config restart test\n";
    service.Stop();
    return false;
  }

  auto updated = cfg;
  updated.pipeline.width = 800;
  service.UpdateConfig(updated);

  const bool sawStop = WaitUntil(
      [&] { return service.Status().last_transition == "stop_config_restart"; },
      500ms);
  service.Stop();

  if (!sawStop) {
    std::cerr << "config restart transition was not exposed without quotes\n";
    return false;
  }
  return true;
}

bool TestVideoOutputDisappearanceStopsPipelineAndMarksUnavailable() {
  ServiceHarness h;
  h.consumer_present.store(true, std::memory_order_relaxed);
  VirtualCameraService service(h.Hooks());
  auto cfg = TestConfig();

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  if (!WaitUntil([&] { return service.Status().pipeline.running; }, 250ms)) {
    std::cerr << "video pipeline did not start before disappearance test\n";
    service.Stop();
    return false;
  }

  h.device_exists.store(false, std::memory_order_relaxed);
  const bool unavailable = WaitUntil(
      [&] {
        const auto status = service.Status();
        return status.pipeline_state == "device_unavailable" &&
               !status.virtual_device_present &&
               !status.virtual_device_available &&
               status.virtual_device_error.find("synthetic loopback missing") !=
                   std::string::npos &&
               h.pipeline->stop_calls.load() >= 1;
      },
      500ms);
  const auto status = service.Status();
  service.Stop();

  if (!unavailable) {
    std::cerr << "output disappearance did not stop/mark unavailable; state='"
              << status.pipeline_state
              << "' present=" << status.virtual_device_present
              << " available=" << status.virtual_device_available << " error='"
              << status.virtual_device_error
              << "' stops=" << h.pipeline->stop_calls.load() << "\n";
    return false;
  }
  return true;
}

bool TestVideoOutputRecoveryClearsUnavailableError() {
  ServiceHarness h;
  h.device_exists.store(false, std::memory_order_relaxed);
  VirtualCameraService service(h.Hooks());
  auto cfg = TestConfig();
  cfg.pipeline.output_device.clear();

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  if (!WaitUntil(
          [&] {
            const auto status = service.Status();
            return status.pipeline_state == "device_unavailable" &&
                   !status.virtual_device_available &&
                   status.last_error.find("synthetic loopback missing") !=
                       std::string::npos;
          },
          250ms)) {
    const auto status = service.Status();
    std::cerr << "missing output was not surfaced before recovery; state='"
              << status.pipeline_state << "' last_error='" << status.last_error
              << "'\n";
    service.Stop();
    return false;
  }

  h.device_exists.store(true, std::memory_order_relaxed);
  const bool recovered = WaitUntil(
      [&] {
        const auto status = service.Status();
        return status.pipeline_state == "idle_no_consumer" &&
               status.virtual_device_present && status.virtual_device_available &&
               status.virtual_device_error.empty() && status.last_error.empty() &&
               h.pipeline->ensure_output_calls.load() > 0;
      },
      500ms);
  const auto status = service.Status();
  service.Stop();

  if (!recovered) {
    std::cerr << "output recovery left stale unavailable error; state='"
              << status.pipeline_state << "' virtual_error='"
              << status.virtual_device_error << "' last_error='"
              << status.last_error << "'\n";
    return false;
  }
  return true;
}

} // namespace

int main() {
  const struct {
    const char *name;
    bool (*fn)();
  } tests[] = {
      {"video pipeline does not start without consumer",
       &TestVideoPipelineDoesNotStartWithoutConsumer},
      {"video pipeline starts when consumer appears",
       &TestVideoPipelineStartsWhenConsumerAppears},
      {"video pipeline stops when consumer disappears",
       &TestVideoPipelineStopsWhenConsumerDisappears},
      {"video grace window absorbs consumer flapping",
       &TestVideoGraceWindowAbsorbsConsumerFlapping},
      {"video consumer detection error surfaces without starting",
       &TestVideoConsumerDetectionErrorSurfacesWithoutStarting},
      {"video start failure backs off", &TestVideoStartFailureBacksOff},
      {"video start failure clears after recovery",
       &TestVideoStartFailureClearsAfterRecovery},
      {"video config restart transition name is stable",
       &TestVideoConfigRestartTransitionNameIsStable},
      {"video output disappearance marks unavailable",
       &TestVideoOutputDisappearanceStopsPipelineAndMarksUnavailable},
      {"video output recovery clears unavailable error",
       &TestVideoOutputRecoveryClearsUnavailableError},
  };

  int failed = 0;
  for (const auto &test : tests) {
    const bool ok = test.fn();
    std::cout << (ok ? "[PASS] " : "[FAIL] ") << test.name << "\n";
    if (!ok)
      ++failed;
  }

  return failed == 0 ? 0 : 1;
}
