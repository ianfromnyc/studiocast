// Tests for the microphone monitor: the processed StudioCast microphone feed
// played back on a user-selected output sink.

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "core/audio/mic_monitor.h"
#include "core/audio/pulse/pactl.h"

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
