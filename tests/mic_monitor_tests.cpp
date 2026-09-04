// Tests for the microphone monitor: the processed StudioCast microphone feed
// played back on a user-selected output sink.

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/audio/audio_pipeline.h"
#include "core/audio/mic_monitor.h"
#include "core/audio/pulse/pactl.h"
#include "core/audio/virtual_audio_service.h"

namespace {

using studiocast::audio::MicMonitorConfig;

using namespace std::chrono_literals;

class ScopedPactlExecHook final {
public:
  explicit ScopedPactlExecHook(
      studiocast::audio::pulse::PactlExecCaptureHook hook) {
    studiocast::audio::pulse::SetPactlExecCaptureHookForTesting(
        std::move(hook));
  }

  ~ScopedPactlExecHook() {
    studiocast::audio::pulse::SetPactlExecCaptureHookForTesting(nullptr);
  }

  ScopedPactlExecHook(const ScopedPactlExecHook &) = delete;
  ScopedPactlExecHook &operator=(const ScopedPactlExecHook &) = delete;
};

studiocast::util::ExecResult ExecResult(int exit_code,
                                        std::string stdout_str = {}) {
  studiocast::util::ExecResult result;
  result.exit_code = exit_code;
  result.stdout_str = std::move(stdout_str);
  return result;
}

bool ExpectEq(const std::string &label, const std::string &got,
              const std::string &want) {
  if (got == want)
    return true;
  std::cerr << label << ": got '" << got << "' want '" << want << "'\n";
  return false;
}

bool ExpectEqInt(const std::string &label, int got, int want) {
  if (got == want)
    return true;
  std::cerr << label << ": got " << got << " want " << want << "\n";
  return false;
}

bool TestConfigDefaultsAndClamping() {
  const MicMonitorConfig defaults;
  bool ok = true;
  if (defaults.enabled) {
    std::cerr << "monitor must be disabled by default\n";
    ok = false;
  }
  ok &= ExpectEq("default sink", defaults.sink, "auto");
  ok &= ExpectEqInt("default latency", defaults.latency_ms, 20);
  ok &= ExpectEqInt("default volume", defaults.volume, 100);

  MicMonitorConfig wild;
  wild.sink = "   ";
  wild.latency_ms = 0;
  wild.volume = 999;
  const auto low = studiocast::audio::NormalizeMicMonitorConfig(wild);
  ok &= ExpectEq("blank sink normalizes to auto", low.sink, "auto");
  ok &= ExpectEqInt("latency clamps up", low.latency_ms,
                    studiocast::audio::kMicMonitorMinLatencyMs);
  ok &= ExpectEqInt("volume clamps down", low.volume, 100);

  MicMonitorConfig high;
  high.sink = "  analog_out  ";
  high.latency_ms = 100000;
  high.volume = -5;
  const auto capped = studiocast::audio::NormalizeMicMonitorConfig(high);
  ok &= ExpectEq("sink is trimmed", capped.sink, "analog_out");
  ok &= ExpectEqInt("latency clamps down", capped.latency_ms,
                    studiocast::audio::kMicMonitorMaxLatencyMs);
  ok &= ExpectEqInt("volume clamps up", capped.volume, 0);
  return ok;
}

bool TestUnsafeSinksAreRejected() {
  bool ok = true;
  std::string reason;

  struct Case {
    const char *sink;
    const char *mic_source;
    bool unsafe;
  };
  const Case cases[] = {
      {"studiocast_sink", "physical_test_mic", true},
      {"studiocast_speakers", "physical_test_mic", true},
      {"physical_test_sink.monitor", "physical_test_mic", true},
      // Monitoring into the sink whose monitor feeds capture is feedback.
      {"physical_test_sink", "physical_test_sink.monitor", true},
      {"physical_test_sink", "physical_test_mic", false},
      {"physical_test_sink", "", false},
  };

  for (const auto &c : cases) {
    reason.clear();
    const bool unsafe = studiocast::audio::IsUnsafeMicMonitorSinkName(
        c.sink, c.mic_source, &reason);
    if (unsafe != c.unsafe) {
      std::cerr << "sink '" << c.sink << "' with source '" << c.mic_source
                << "': got unsafe=" << unsafe << " want " << c.unsafe << "\n";
      ok = false;
      continue;
    }
    if (unsafe && reason.empty()) {
      std::cerr << "sink '" << c.sink << "' was rejected without a reason\n";
      ok = false;
    }
  }
  return ok;
}

bool TestAutoSinkResolvesToPulseDefault() {
  ScopedPactlExecHook hook([](const std::string &command) {
    if (command == "pactl get-default-sink 2>&1")
      return ExecResult(0, "physical_test_sink\n");
    return ExecResult(99, "unexpected command: " + command);
  });

  std::string error;
  const auto chosen = studiocast::audio::ChooseSafeMicMonitorSinkName(
      "auto", "physical_test_mic", &error);
  if (!chosen) {
    std::cerr << "auto sink was not resolved: " << error << "\n";
    return false;
  }
  return ExpectEq("resolved auto sink", *chosen, "physical_test_sink");
}

bool TestAutoSinkRefusesUnsafePulseDefault() {
  ScopedPactlExecHook hook([](const std::string &command) {
    if (command == "pactl get-default-sink 2>&1")
      return ExecResult(0, "studiocast_sink\n");
    if (command == "pactl list short sinks 2>&1")
      return ExecResult(0, "1\tstudiocast_sink\tmodule-null-sink.c\t"
                           "float32le 2ch 48000Hz\tSUSPENDED\n");
    return ExecResult(99, "unexpected command: " + command);
  });

  std::string error;
  const auto chosen = studiocast::audio::ChooseSafeMicMonitorSinkName(
      "auto", "physical_test_mic", &error);
  if (chosen) {
    std::cerr << "unsafe Pulse default sink was accepted: " << *chosen << "\n";
    return false;
  }
  if (error.empty()) {
    std::cerr << "no reason was reported for the unsafe default sink\n";
    return false;
  }
  return true;
}

bool CommandWasRun(const std::vector<std::string> &commands,
                   const std::string &needle) {
  for (const auto &command : commands) {
    if (command.find(needle) != std::string::npos)
      return true;
  }
  return false;
}

// Recorded pactl output for a session that already has a stale monitor
// loopback (module 77) left over from a previous daemon run.
studiocast::util::ExecResult
FakePactlForMonitorStart(const std::string &command,
                         std::vector<std::string> *log, bool *stale_unloaded) {
  if (log)
    log->push_back(command);

  if (command == "pactl --version 2>&1")
    return ExecResult(0, "pactl 17.0\n");
  if (command == "pactl get-default-sink 2>&1")
    return ExecResult(0, "physical_test_sink\n");
  if (command == "pactl list short modules 2>&1") {
    std::string modules =
        "10\tmodule-null-sink\tsink_name=studiocast_sink\n"
        "11\tmodule-loopback\tsource=other_source sink=physical_test_sink\n";
    if (!stale_unloaded || !*stale_unloaded) {
      modules +=
          "77\tmodule-loopback\tsource=studiocast_mic sink=old_sink "
          "latency_msec=20 "
          "source_output_properties=media.name=StudioCast_Microphone_Monitor\n";
    }
    return ExecResult(0, modules);
  }
  if (command == "pactl unload-module 77 2>&1") {
    if (stale_unloaded)
      *stale_unloaded = true;
    return ExecResult(0, "");
  }
  if (command.rfind("pactl load-module ", 0) == 0)
    return ExecResult(0, "551\n");
  if (command == "pactl list sink-inputs 2>&1") {
    return ExecResult(0, "Sink Input #900\n"
                         "\tDriver: PipeWire\n"
                         "\tOwner Module: 551\n"
                         "\tSink: 42\n"
                         "\tProperties:\n"
                         "\t\tmedia.name = \"StudioCast_Microphone_Monitor\"\n"
                         "\n");
  }
  if (command.rfind("pactl set-sink-input-volume ", 0) == 0)
    return ExecResult(0, "");
  return ExecResult(99, "unexpected command: " + command);
}

bool TestStartLoadsLoopbackAndClearsStaleModules() {
  std::vector<std::string> log;
  bool stale_unloaded = false;
  ScopedPactlExecHook hook([&](const std::string &command) {
    return FakePactlForMonitorStart(command, &log, &stale_unloaded);
  });

  MicMonitorConfig cfg;
  cfg.enabled = true;
  cfg.sink = "auto";
  cfg.latency_ms = 25;
  cfg.volume = 80;

  studiocast::audio::MicMonitorState state;
  std::string error;
  if (!studiocast::audio::StartMicMonitor(cfg, "physical_test_mic", &state,
                                          &error)) {
    std::cerr << "StartMicMonitor failed: " << error << "\n";
    return false;
  }

  bool ok = true;
  if (!stale_unloaded) {
    std::cerr << "the stale monitor loopback was not unloaded\n";
    ok = false;
  }
  if (CommandWasRun(log, "pactl unload-module 11 ")) {
    std::cerr << "an unrelated loopback module was unloaded\n";
    ok = false;
  }

  const std::string want =
      "pactl load-module 'module-loopback' 'source=studiocast_mic' "
      "'sink=physical_test_sink' 'latency_msec=25' "
      "'source_output_properties=media.name=StudioCast_Microphone_Monitor' "
      "'sink_input_properties=media.name=StudioCast_Microphone_Monitor' 2>&1";
  if (!CommandWasRun(log, want)) {
    std::cerr << "monitor load-module command was not issued as expected.\n"
              << "want: " << want << "\ngot:\n";
    for (const auto &c : log)
      std::cerr << "  " << c << "\n";
    ok = false;
  }

  if (!CommandWasRun(log, "pactl set-sink-input-volume 900 80% 2>&1")) {
    std::cerr << "monitor volume was not applied to the monitor sink input\n";
    ok = false;
  }

  ok &= ExpectEqInt("module id", state.module_id, 551);
  ok &= ExpectEq("resolved sink", state.sink, "physical_test_sink");
  ok &= ExpectEqInt("latency", state.latency_ms, 25);
  ok &= ExpectEqInt("volume", state.volume, 80);
  if (!state.active) {
    std::cerr << "monitor state is not active after a successful start\n";
    ok = false;
  }
  return ok;
}

bool TestStartRefusesStudioCastOwnSink() {
  std::vector<std::string> log;
  ScopedPactlExecHook hook([&](const std::string &command) {
    return FakePactlForMonitorStart(command, &log, nullptr);
  });

  MicMonitorConfig cfg;
  cfg.enabled = true;
  cfg.sink = "studiocast_speakers";

  studiocast::audio::MicMonitorState state;
  std::string error;
  if (studiocast::audio::StartMicMonitor(cfg, "physical_test_mic", &state,
                                         &error)) {
    std::cerr << "the monitor started into a StudioCast virtual sink\n";
    return false;
  }
  if (error.empty()) {
    std::cerr << "no reason was reported for the refused sink\n";
    return false;
  }
  if (CommandWasRun(log, "pactl load-module ")) {
    std::cerr << "a loopback was loaded even though the sink was refused\n";
    return false;
  }
  return true;
}

bool TestStopUnloadsOnlyMonitorModules() {
  std::vector<std::string> log;
  bool stale_unloaded = false;
  ScopedPactlExecHook hook([&](const std::string &command) {
    return FakePactlForMonitorStart(command, &log, &stale_unloaded);
  });

  std::string error;
  if (!studiocast::audio::StopMicMonitor(&error)) {
    std::cerr << "StopMicMonitor failed: " << error << "\n";
    return false;
  }
  if (!CommandWasRun(log, "pactl unload-module 77 2>&1")) {
    std::cerr << "the monitor loopback module was not unloaded\n";
    return false;
  }
  if (CommandWasRun(log, "pactl unload-module 11 2>&1") ||
      CommandWasRun(log, "pactl unload-module 10 2>&1")) {
    std::cerr << "an unrelated module was unloaded\n";
    return false;
  }
  return true;
}

// ---------------------------------------------------------------------------
// Service lifecycle
// ---------------------------------------------------------------------------

using studiocast::audio::AudioConsumerSnapshot;
using studiocast::audio::MicMonitorState;
using studiocast::audio::VirtualAudioService;
using studiocast::audio::VirtualAudioServiceConfig;
using studiocast::audio::VirtualAudioServiceHooks;

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

// Records what the service asks the monitor to do.
struct MonitorRecorder {
  std::atomic<int> starts{0};
  std::atomic<int> stops{0};
  std::atomic<bool> running{false};
  std::atomic<bool> fail_start{false};
  std::mutex mu;
  std::string last_sink;
  int last_latency_ms = 0;
  int last_volume = 0;
};

void HookMonitor(VirtualAudioServiceHooks *hooks, MonitorRecorder *rec) {
  hooks->start_mic_monitor = [rec](const MicMonitorConfig &cfg,
                                   const std::string &, MicMonitorState *out,
                                   std::string *error) {
    rec->starts.fetch_add(1, std::memory_order_relaxed);
    if (rec->fail_start.load(std::memory_order_relaxed)) {
      if (error)
        *error = "synthetic monitor start failure";
      rec->running.store(false, std::memory_order_relaxed);
      return false;
    }
    {
      std::lock_guard<std::mutex> lock(rec->mu);
      rec->last_sink = cfg.sink;
      rec->last_latency_ms = cfg.latency_ms;
      rec->last_volume = cfg.volume;
    }
    rec->running.store(true, std::memory_order_relaxed);
    if (out) {
      out->active = true;
      out->module_id = 551;
      out->sink =
          cfg.sink == "auto" ? std::string("physical_test_sink") : cfg.sink;
      out->latency_ms = cfg.latency_ms;
      out->volume = cfg.volume;
    }
    if (error)
      error->clear();
    return true;
  };
  hooks->stop_mic_monitor = [rec](std::string *error) {
    rec->stops.fetch_add(1, std::memory_order_relaxed);
    rec->running.store(false, std::memory_order_relaxed);
    if (error)
      error->clear();
    return true;
  };
  hooks->detect_mic_monitor = [rec](std::string *error) {
    MicMonitorState state;
    state.active = rec->running.load(std::memory_order_relaxed);
    state.module_id = state.active ? 551 : -1;
    if (error)
      error->clear();
    return state;
  };
}

VirtualAudioServiceConfig MonitorServiceConfig() {
  VirtualAudioServiceConfig cfg;
  cfg.enabled = true;
  cfg.create_virtual_mic = true;
  cfg.create_virtual_speakers = false;
  cfg.speakers_enabled = false;
  cfg.source_name = "physical_test_mic";
  cfg.poll_ms = 1;
  cfg.start_retry_ms = 50;
  cfg.effects.engine =
      studiocast::audio::effects::AudioEffectsEnginePreference::kOff;
  cfg.monitor.enabled = true;
  cfg.monitor.sink = "auto";
  cfg.monitor.latency_ms = 20;
  cfg.monitor.volume = 100;
  return cfg;
}

void HookQuietService(VirtualAudioServiceHooks *hooks) {
  hooks->create_virtual_mic = [](std::string *error) {
    if (error)
      error->clear();
    return true;
  };
  hooks->create_pipeline = [](studiocast::audio::AudioProcessor *) {
    return std::unique_ptr<studiocast::audio::AudioPipelineRunner>();
  };
  hooks->sleep_for = [](std::chrono::milliseconds) {
    std::this_thread::sleep_for(1ms);
  };
}

bool TestServiceStartsAndStopsMonitorWithConfig() {
  MonitorRecorder rec;
  VirtualAudioServiceHooks hooks;
  HookQuietService(&hooks);
  HookMonitor(&hooks, &rec);
  hooks.detect_microphone_consumers = [] {
    AudioConsumerSnapshot snap;
    snap.present = false;
    snap.count = 0;
    return snap;
  };

  VirtualAudioService service(std::move(hooks));
  auto cfg = MonitorServiceConfig();

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  const bool started = WaitUntil(
      [&] {
        const auto st = service.Status();
        return st.monitor_active &&
               st.monitor_sink_active == "physical_test_sink";
      },
      500ms);
  if (!started) {
    const auto st = service.Status();
    std::cerr << "monitor did not become active; starts=" << rec.starts.load()
              << " error='" << st.monitor_last_error << "'\n";
    service.Stop();
    return false;
  }

  cfg.monitor.enabled = false;
  service.UpdateConfig(cfg);

  const bool stopped =
      WaitUntil([&] { return !service.Status().monitor_active; }, 500ms);
  if (!stopped) {
    std::cerr << "monitor did not stop after it was disabled\n";
    service.Stop();
    return false;
  }

  service.Stop();
  return rec.stops.load(std::memory_order_relaxed) >= 1;
}

bool TestServiceRestartsMonitorOnSinkChange() {
  MonitorRecorder rec;
  VirtualAudioServiceHooks hooks;
  HookQuietService(&hooks);
  HookMonitor(&hooks, &rec);

  VirtualAudioService service(std::move(hooks));
  auto cfg = MonitorServiceConfig();

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  if (!WaitUntil([&] { return service.Status().monitor_active; }, 500ms)) {
    std::cerr << "monitor did not start\n";
    service.Stop();
    return false;
  }

  cfg.monitor.sink = "other_physical_test_sink";
  service.UpdateConfig(cfg);

  const bool restarted = WaitUntil(
      [&] {
        const auto st = service.Status();
        return st.monitor_active &&
               st.monitor_sink_active == "other_physical_test_sink";
      },
      500ms);
  service.Stop();
  if (!restarted) {
    std::cerr << "monitor did not follow the new sink; starts="
              << rec.starts.load() << "\n";
    return false;
  }
  return rec.starts.load(std::memory_order_relaxed) >= 2;
}

bool TestServiceStopsMonitorWhenServiceStops() {
  MonitorRecorder rec;
  VirtualAudioServiceHooks hooks;
  HookQuietService(&hooks);
  HookMonitor(&hooks, &rec);

  VirtualAudioService service(std::move(hooks));
  const auto cfg = MonitorServiceConfig();

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }
  if (!WaitUntil([&] { return service.Status().monitor_active; }, 500ms)) {
    std::cerr << "monitor did not start\n";
    service.Stop();
    return false;
  }

  service.Stop();

  if (rec.running.load(std::memory_order_relaxed)) {
    std::cerr << "monitor kept running after the service stopped\n";
    return false;
  }
  if (service.Status().monitor_active) {
    std::cerr << "monitor status stayed active after the service stopped\n";
    return false;
  }
  return true;
}

bool TestServiceRetriesMonitorAndReportsError() {
  MonitorRecorder rec;
  rec.fail_start.store(true, std::memory_order_relaxed);

  VirtualAudioServiceHooks hooks;
  HookQuietService(&hooks);
  HookMonitor(&hooks, &rec);

  VirtualAudioService service(std::move(hooks));
  auto cfg = MonitorServiceConfig();

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  const bool reported = WaitUntil(
      [&] {
        const auto st = service.Status();
        return !st.monitor_active &&
               st.monitor_last_error.find("synthetic monitor start failure") !=
                   std::string::npos;
      },
      500ms);
  if (!reported) {
    std::cerr << "the monitor start failure was not reported: '"
              << service.Status().monitor_last_error << "'\n";
    service.Stop();
    return false;
  }

  const int starts_after_first_failure =
      rec.starts.load(std::memory_order_relaxed);
  rec.fail_start.store(false, std::memory_order_relaxed);

  const bool recovered =
      WaitUntil([&] { return service.Status().monitor_active; }, 1000ms);
  service.Stop();
  if (!recovered) {
    std::cerr << "the monitor did not recover after the failure cleared\n";
    return false;
  }
  return rec.starts.load(std::memory_order_relaxed) >
         starts_after_first_failure;
}

bool TestMonitorConsumerIsCountedApartFromApps() {
  MonitorRecorder rec;
  std::atomic<int> consumer_count{0};

  VirtualAudioServiceHooks hooks;
  HookQuietService(&hooks);
  HookMonitor(&hooks, &rec);
  hooks.detect_microphone_consumers = [&consumer_count] {
    AudioConsumerSnapshot snap;
    snap.count = consumer_count.load(std::memory_order_relaxed);
    snap.present = snap.count > 0;
    return snap;
  };

  VirtualAudioService service(std::move(hooks));
  const auto cfg = MonitorServiceConfig();

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  if (!WaitUntil([&] { return service.Status().monitor_active; }, 500ms)) {
    std::cerr << "monitor did not start\n";
    service.Stop();
    return false;
  }

  // Only the monitor is reading studiocast_mic.
  consumer_count.store(1, std::memory_order_relaxed);
  const bool monitor_only = WaitUntil(
      [&] {
        const auto st = service.Status();
        return st.mic_consumer_count == 1 &&
               st.mic_monitor_consumer_count == 1 &&
               st.mic_app_consumer_count == 0;
      },
      500ms);
  if (!monitor_only) {
    const auto st = service.Status();
    std::cerr << "monitor-only consumer split is wrong: total="
              << st.mic_consumer_count << " apps=" << st.mic_app_consumer_count
              << " monitor=" << st.mic_monitor_consumer_count << "\n";
    service.Stop();
    return false;
  }

  // A real app joins the monitor.
  consumer_count.store(2, std::memory_order_relaxed);
  const bool with_app = WaitUntil(
      [&] {
        const auto st = service.Status();
        return st.mic_app_consumer_count == 1 &&
               st.mic_monitor_consumer_count == 1;
      },
      500ms);
  service.Stop();
  if (!with_app) {
    const auto st = service.Status();
    std::cerr << "app consumer split is wrong: total=" << st.mic_consumer_count
              << " apps=" << st.mic_app_consumer_count
              << " monitor=" << st.mic_monitor_consumer_count << "\n";
    return false;
  }
  return true;
}

} // namespace

int main() {
  struct TestCase {
    const char *name;
    std::function<bool()> fn;
  };

  const std::vector<TestCase> tests = {
      {"monitor config defaults and clamping", &TestConfigDefaultsAndClamping},
      {"monitor refuses unsafe sinks", &TestUnsafeSinksAreRejected},
      {"monitor auto sink resolves to the Pulse default",
       &TestAutoSinkResolvesToPulseDefault},
      {"monitor auto sink refuses an unsafe Pulse default",
       &TestAutoSinkRefusesUnsafePulseDefault},
      {"monitor start loads the loopback and clears stale modules",
       &TestStartLoadsLoopbackAndClearsStaleModules},
      {"monitor start refuses a StudioCast sink",
       &TestStartRefusesStudioCastOwnSink},
      {"monitor stop unloads only monitor modules",
       &TestStopUnloadsOnlyMonitorModules},
      {"service starts and stops the monitor with config",
       &TestServiceStartsAndStopsMonitorWithConfig},
      {"service restarts the monitor on a sink change",
       &TestServiceRestartsMonitorOnSinkChange},
      {"service stops the monitor when it stops",
       &TestServiceStopsMonitorWhenServiceStops},
      {"service retries the monitor and reports the error",
       &TestServiceRetriesMonitorAndReportsError},
      {"monitor consumer is counted apart from apps",
       &TestMonitorConsumerIsCountedApartFromApps},
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
