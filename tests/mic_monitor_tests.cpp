// Tests for the microphone monitor: the processed StudioCast microphone feed
// played back on a user-selected output sink.

#include <atomic>
#include <chrono>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
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

// No pactl means no Pulse, and no Pulse means there is no loopback to remove.
// The stop must report that as nothing to clean. Reporting a failure instead
// tells a user who never turned the monitor on that it needs attention, and
// keeps the stop retry running for ever.
bool TestStopReportsNothingToCleanWithoutPactl() {
  std::vector<std::string> log;
  ScopedPactlExecHook hook([&](const std::string &command) {
    log.push_back(command);
    if (command == "pactl --version 2>&1")
      return ExecResult(127, "sh: pactl: command not found\n");
    return ExecResult(99, "unexpected command: " + command);
  });

  std::string error = "not touched";
  if (!studiocast::audio::StopMicMonitor(&error)) {
    std::cerr << "the stop reported a failure without pactl: '" << error
              << "'\n";
    return false;
  }
  if (!error.empty()) {
    std::cerr << "the stop left an error behind: '" << error << "'\n";
    return false;
  }
  if (CommandWasRun(log, "unload-module")) {
    std::cerr << "the stop ran pactl after the availability check failed\n";
    return false;
  }
  return true;
}

// A sound server that does not answer is not a sound server that is absent.
// "Nothing to clean" is only true when pactl cannot be run at all: a `pactl
// --version` that runs out of time says nothing about the loopback, which may
// still play the microphone into the speakers. The stop must fail, so the
// service keeps coming back to the route and cleans it at shutdown.
bool TestStopReportsAFailureWhenPactlTimesOut() {
  std::vector<std::string> log;
  ScopedPactlExecHook hook([&](const std::string &command) {
    log.push_back(command);
    if (command == "pactl --version 2>&1") {
      auto result = ExecResult(-1);
      result.timed_out = true;
      return result;
    }
    return ExecResult(99, "unexpected command: " + command);
  });

  std::string error;
  if (studiocast::audio::StopMicMonitor(&error)) {
    std::cerr << "the stop reported the monitor removed after pactl timed "
                 "out\n";
    return false;
  }
  if (error.empty()) {
    std::cerr << "the failed stop said nothing about why\n";
    return false;
  }
  // The GUI reads this text and has an arm of its own for it, because a stop
  // that got no answer can leave the loopback playing. A sentence that only
  // means the same thing reads as a monitor that starts again on its own.
  if (error.find(studiocast::audio::kSoundServerNoAnswerOnStopMessage) ==
      std::string::npos) {
    std::cerr << "the failed stop did not use the shared text: '" << error
              << "'\n";
    return false;
  }
  return true;
}

// A `pactl --version` that only ran out of time is not a pactl that is
// missing. A killed pactl prints nothing, so "pactl not available: " ended
// with the colon, and the GUI made it "The monitor needs attention. Open
// Support for technical details." for a sound server that is only busy.
bool TestStartReportsATimedOutPactlAsABusySoundServer() {
  ScopedPactlExecHook hook([](const std::string &command) {
    if (command == "pactl --version 2>&1") {
      auto result = ExecResult(-1);
      result.timed_out = true;
      return result;
    }
    return ExecResult(99, "unexpected command: " + command);
  });

  MicMonitorConfig cfg;
  cfg.enabled = true;
  cfg.sink = "headset_test_sink";

  studiocast::audio::MicMonitorState state;
  std::string error;
  if (studiocast::audio::StartMicMonitor(cfg, "physical_test_mic", &state,
                                         &error)) {
    std::cerr << "the start reported success while pactl timed out\n";
    return false;
  }
  // The GUI reads this text, so the message must carry it exactly. A sentence
  // that only means the same thing becomes "Open Support" on the page.
  if (error.find(studiocast::audio::kSoundServerNoAnswerMessage) ==
      std::string::npos) {
    std::cerr << "the failed start blamed a missing pactl: '" << error << "'\n";
    return false;
  }
  return true;
}

// The sink question blocks the audio supervisor, and a failed pinned start
// asks it every time. Each pactl process carries its own deadline, so two of
// them are twice the wait of a wedged sound server, and a stop the user asked
// for waits behind them. The sink list answers the question on its own.
bool TestSinkPresentAsksTheSoundServerOnce() {
  std::vector<std::string> log;
  ScopedPactlExecHook hook([&](const std::string &command) {
    log.push_back(command);
    if (command == "pactl list short sinks 2>&1") {
      return ExecResult(0, "3\theadset_test_sink\tmodule-alsa-card.c\t"
                           "s16le 2ch 48000Hz\tSUSPENDED\n");
    }
    return ExecResult(99, "unexpected command: " + command);
  });

  std::string error;
  const auto present =
      studiocast::audio::MicMonitorSinkPresent("headset_test_sink", &error);
  if (!present.has_value() || !*present) {
    std::cerr << "the sink in the list was not reported as present: error='"
              << error << "'\n";
    return false;
  }
  if (log.size() != 1) {
    std::cerr << "the sink question ran " << log.size()
              << " pactl commands, want 1\n";
    return false;
  }
  return ExpectEq("the sink question command", log.front(),
                  "pactl list short sinks 2>&1");
}

// One question is enough only while a sound server that cannot be run at all
// still gives no answer. A pactl that is not installed must not read as a sink
// that is gone.
bool TestSinkPresentGivesNoAnswerWithoutPactl() {
  ScopedPactlExecHook hook([](const std::string &) {
    return ExecResult(127, "sh: pactl: command not found\n");
  });

  std::string error;
  const auto present =
      studiocast::audio::MicMonitorSinkPresent("headset_test_sink", &error);
  if (present.has_value()) {
    std::cerr << "a missing pactl answered '" << (*present ? "present" : "gone")
              << "'\n";
    return false;
  }
  if (error.empty()) {
    std::cerr << "the unanswered sink question said nothing about why\n";
    return false;
  }
  return true;
}

// The sink question has three answers, and the pin rule depends on all three.
// A `pactl list short sinks` that runs out of time prints nothing at all, so
// an empty list must not read as "the sink is gone": the monitor would stop
// for good and blame an output that never moved.
bool TestSinkPresentGivesNoAnswerWhenTheSinkListTimesOut() {
  ScopedPactlExecHook hook([](const std::string &command) {
    if (command == "pactl --version 2>&1")
      return ExecResult(0, "pactl 17.0\n");
    if (command == "pactl list short sinks 2>&1") {
      auto result = ExecResult(-1);
      result.timed_out = true;
      return result;
    }
    return ExecResult(99, "unexpected command: " + command);
  });

  std::string error;
  const auto present =
      studiocast::audio::MicMonitorSinkPresent("headset_test_sink", &error);
  if (present.has_value()) {
    std::cerr << "a timed-out sink list answered '"
              << (*present ? "present" : "gone") << "'\n";
    return false;
  }
  if (error.empty()) {
    std::cerr << "the unanswered sink question said nothing about why\n";
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
  std::atomic<int> volume_calls{0};
  std::atomic<bool> running{false};
  std::atomic<bool> fail_start{false};
  std::atomic<bool> fail_stop{false};
  std::atomic<bool> fail_volume{false};
  std::atomic<bool> fail_detect{false};
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
    if (rec->fail_stop.load(std::memory_order_relaxed)) {
      // A failed stop says nothing about the loopback, so it may still play.
      if (error)
        *error = "synthetic monitor stop failure";
      return false;
    }
    rec->running.store(false, std::memory_order_relaxed);
    if (error)
      error->clear();
    return true;
  };
  hooks->set_mic_monitor_volume = [rec](int, int volume, std::string *error) {
    rec->volume_calls.fetch_add(1, std::memory_order_relaxed);
    if (rec->fail_volume.load(std::memory_order_relaxed)) {
      if (error)
        *error = "synthetic monitor volume failure";
      return false;
    }
    {
      std::lock_guard<std::mutex> lock(rec->mu);
      rec->last_volume = volume;
    }
    if (error)
      error->clear();
    return true;
  };
  // Every output the tests ask about is there unless the test says otherwise.
  // Without this the service would ask the real sound server of the person who
  // runs the tests.
  hooks->mic_monitor_sink_present = [](const std::string &, std::string *error) {
    if (error)
      error->clear();
    return std::optional<bool>(true);
  };
  hooks->detect_mic_monitor = [rec](std::string *error) {
    MicMonitorState state;
    if (rec->fail_detect.load(std::memory_order_relaxed)) {
      // A failed check reports nothing about the route, so the state stays
      // empty. Pulse behaves this way when pactl is gone.
      if (error)
        *error = "synthetic monitor check failure";
      return state;
    }
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

// A user who reacts to a failed start by turning the monitor off must not be
// told for ever that the monitor needs attention. A monitor that is not wanted
// and has no route left to remove reports nothing.
bool TestServiceClearsAStartFailureWhenTheMonitorIsTurnedOff() {
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
        return service.Status().monitor_last_error.find(
                   "synthetic monitor start failure") != std::string::npos;
      },
      1000ms);
  if (!reported) {
    std::cerr << "the monitor start failure was not reported\n";
    service.Stop();
    return false;
  }

  cfg.monitor.enabled = false;
  service.UpdateConfig(cfg);

  const bool cleared = WaitUntil(
      [&] { return service.Status().monitor_last_error.empty(); }, 1000ms);
  if (!cleared) {
    std::cerr << "a start failure survived turning the monitor off: '"
              << service.Status().monitor_last_error << "'\n";
    service.Stop();
    return false;
  }

  service.Stop();
  return true;
}

// A volume step that fails and a later step that works are one control the
// user moves. The error from the failed step must go when a step succeeds,
// because the monitor plays and nothing needs attention.
bool TestServiceClearsAVolumeErrorAfterALaterStepWorks() {
  MonitorRecorder rec;
  rec.fail_volume.store(true, std::memory_order_relaxed);

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

  if (!WaitUntil([&] { return service.Status().monitor_active; }, 1000ms)) {
    std::cerr << "the monitor did not start\n";
    service.Stop();
    return false;
  }

  cfg.monitor.volume = 60;
  service.UpdateConfig(cfg);

  const bool reported = WaitUntil(
      [&] {
        return service.Status().monitor_last_error.find(
                   "synthetic monitor volume failure") != std::string::npos;
      },
      1000ms);
  if (!reported) {
    std::cerr << "the monitor volume failure was not reported\n";
    service.Stop();
    return false;
  }

  rec.fail_volume.store(false, std::memory_order_relaxed);

  const bool cleared = WaitUntil(
      [&] { return service.Status().monitor_last_error.empty(); }, 2000ms);
  if (!cleared) {
    std::cerr << "a volume error survived a later volume step that worked: '"
              << service.Status().monitor_last_error << "'\n";
    service.Stop();
    return false;
  }

  service.Stop();
  return true;
}

// A stopped service reports no monitor state at all, so the error that the
// last failed start left behind must go with the rest of the monitor fields.
bool TestServiceClearsMonitorErrorWhenItStops() {
  MonitorRecorder rec;
  rec.fail_start.store(true, std::memory_order_relaxed);

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

  const bool reported = WaitUntil(
      [&] { return !service.Status().monitor_last_error.empty(); }, 500ms);
  if (!reported) {
    std::cerr << "the monitor start failure was not reported\n";
    service.Stop();
    return false;
  }

  service.Stop();

  const auto st = service.Status();
  if (!st.monitor_last_error.empty()) {
    std::cerr << "a stale monitor error survived the stop: '"
              << st.monitor_last_error << "'\n";
    return false;
  }
  return true;
}

// A failed check of the monitor route says nothing about the loopback, so the
// status must not go on claiming an active monitor. The service reports the
// failure and hands the route to the restart path.
bool TestServiceTreatsAFailedMonitorCheckAsStopped() {
  MonitorRecorder rec;
  VirtualAudioService *service_ptr = nullptr;
  std::mutex seen_mu;
  std::string error_at_restart;

  VirtualAudioServiceHooks hooks;
  HookQuietService(&hooks);
  HookMonitor(&hooks, &rec);

  // Read the reported error at the start of the restart, which is the one
  // moment the failed check is the newest thing that happened.
  auto start_monitor = hooks.start_mic_monitor;
  hooks.start_mic_monitor = [&](const MicMonitorConfig &cfg,
                                const std::string &source, MicMonitorState *out,
                                std::string *error) {
    if (service_ptr != nullptr &&
        rec.fail_detect.load(std::memory_order_relaxed)) {
      const std::string reported = service_ptr->Status().monitor_last_error;
      std::lock_guard<std::mutex> lock(seen_mu);
      if (error_at_restart.empty())
        error_at_restart = reported;
    }
    return start_monitor(cfg, source, out, error);
  };

  VirtualAudioService service(std::move(hooks));
  service_ptr = &service;
  const auto cfg = MonitorServiceConfig();

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  if (!WaitUntil([&] { return service.Status().monitor_active; }, 1000ms)) {
    std::cerr << "the monitor did not start\n";
    service.Stop();
    return false;
  }

  // The restart fails as well, so that the state after a failed check stays
  // readable instead of being repaired in the same pass.
  const int starts_before = rec.starts.load(std::memory_order_relaxed);
  rec.fail_start.store(true, std::memory_order_relaxed);
  rec.fail_detect.store(true, std::memory_order_relaxed);

  // The service checks the route every two seconds.
  const bool reported = WaitUntil(
      [&] {
        const auto st = service.Status();
        return !st.monitor_active && !st.monitor_last_error.empty() &&
               rec.starts.load(std::memory_order_relaxed) > starts_before;
      },
      5000ms);
  if (!reported) {
    const auto st = service.Status();
    std::cerr << "a failed check left the monitor state alone: active="
              << st.monitor_active << " error='" << st.monitor_last_error
              << "' starts=" << rec.starts.load(std::memory_order_relaxed)
              << " (was " << starts_before << ")\n";
    service.Stop();
    return false;
  }

  bool ok = true;
  {
    std::lock_guard<std::mutex> lock(seen_mu);
    if (error_at_restart.find("synthetic monitor check failure") ==
        std::string::npos) {
      std::cerr << "the failed check was not reported: '" << error_at_restart
                << "'\n";
      ok = false;
    }
  }

  rec.fail_detect.store(false, std::memory_order_relaxed);
  rec.fail_start.store(false, std::memory_order_relaxed);
  if (!WaitUntil([&] { return service.Status().monitor_active; }, 2000ms)) {
    std::cerr << "the monitor did not come back after the check recovered\n";
    ok = false;
  }

  service.Stop();
  return ok;
}

// Pulse unloads the loopback when the chosen output disappears, and the Pulse
// default moves to the built-in speakers on the same unplug. Re-resolving
// "auto" would therefore move the monitor onto the speakers on its own and
// build a feedback loop, so the monitor stops and says why instead.
bool TestServiceStopsTheMonitorWhenTheResolvedOutputDisappears() {
  MonitorRecorder rec;
  std::atomic<bool> headset_present{true};

  VirtualAudioServiceHooks hooks;
  HookQuietService(&hooks);
  HookMonitor(&hooks, &rec);

  // "auto" resolves to the headset while it is plugged in, and to the built-in
  // speakers once it is gone, the way the Pulse default sink moves.
  auto start_monitor = hooks.start_mic_monitor;
  hooks.start_mic_monitor = [&](const MicMonitorConfig &cfg,
                                const std::string &source, MicMonitorState *out,
                                std::string *error) {
    MicMonitorConfig resolved = cfg;
    if (resolved.sink == "auto") {
      resolved.sink = headset_present.load(std::memory_order_relaxed)
                          ? std::string("headset_test_sink")
                          : std::string("speaker_test_sink");
    }
    return start_monitor(resolved, source, out, error);
  };

  VirtualAudioService service(std::move(hooks));
  auto cfg = MonitorServiceConfig();

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  if (!WaitUntil(
          [&] {
            return service.Status().monitor_sink_active == "headset_test_sink";
          },
          1000ms)) {
    std::cerr << "the monitor did not start on the headset\n";
    service.Stop();
    return false;
  }

  // The headset is unplugged: Pulse unloads the loopback and the default sink
  // becomes the built-in speakers.
  headset_present.store(false, std::memory_order_relaxed);
  rec.running.store(false, std::memory_order_relaxed);

  // The service checks the route every two seconds.
  const bool reported = WaitUntil(
      [&] {
        const auto st = service.Status();
        return !st.monitor_active &&
               st.monitor_note.find("headset_test_sink") != std::string::npos;
      },
      5000ms);
  if (!reported) {
    const auto st = service.Status();
    std::cerr << "the lost monitor output was not reported: active="
              << st.monitor_active << " note='" << st.monitor_note << "'\n";
    service.Stop();
    return false;
  }

  // The monitor must stay off rather than follow the default onto the
  // speakers.
  std::this_thread::sleep_for(500ms);
  bool ok = true;
  {
    std::lock_guard<std::mutex> lock(rec.mu);
    if (rec.last_sink != "headset_test_sink") {
      std::cerr << "the monitor moved to '" << rec.last_sink
                << "' on its own\n";
      ok = false;
    }
  }
  if (service.Status().monitor_active) {
    std::cerr << "the monitor restarted itself after the output was lost\n";
    ok = false;
  }

  // Turning the monitor off and on again is an explicit restart, so the
  // service resolves the default afresh.
  cfg.monitor.enabled = false;
  service.UpdateConfig(cfg);
  if (!WaitUntil([&] { return service.Status().monitor_note.empty(); },
                 1000ms)) {
    std::cerr << "the lost-output note survived turning the monitor off\n";
    ok = false;
  }
  cfg.monitor.enabled = true;
  service.UpdateConfig(cfg);
  if (!WaitUntil(
          [&] {
            return service.Status().monitor_sink_active == "speaker_test_sink";
          },
          1000ms)) {
    std::cerr << "the monitor did not resolve the default again after the "
                 "user turned it back on\n";
    ok = false;
  }

  service.Stop();
  return ok;
}

// A lost output is the designed-for failure of the monitor, and the sentence
// the service writes tells the user what to do about it. It goes in the note,
// which the GUI prints as written, and not in the error, which the GUI turns
// into "The monitor needs attention. Open Support for technical details."
bool TestServiceReportsALostOutputAsANote() {
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

  if (!WaitUntil([&] { return service.Status().monitor_active; }, 1000ms)) {
    std::cerr << "the monitor did not start\n";
    service.Stop();
    return false;
  }

  // Pulse unloads the loopback when the output disappears.
  rec.running.store(false, std::memory_order_relaxed);

  // The service checks the route every two seconds.
  const bool reported = WaitUntil(
      [&] {
        const auto st = service.Status();
        return !st.monitor_active &&
               st.monitor_note.find("disappeared") != std::string::npos;
      },
      5000ms);
  bool ok = true;
  if (!reported) {
    const auto st = service.Status();
    std::cerr << "the lost output was not reported as a note: note='"
              << st.monitor_note << "' error='" << st.monitor_last_error
              << "'\n";
    ok = false;
  } else if (!service.Status().monitor_last_error.empty()) {
    std::cerr << "the lost output was also reported as an error: '"
              << service.Status().monitor_last_error << "'\n";
    ok = false;
  }

  service.Stop();
  return ok;
}

// A failed check of the route is not a request from the user. The restart that
// follows it must keep the output the last start really used: re-resolving
// "auto" here would move the monitor onto the new Pulse default and build the
// feedback loop the lost-output stop exists to prevent.
bool TestServiceKeepsTheResolvedOutputAcrossAFailedCheck() {
  MonitorRecorder rec;
  std::atomic<bool> headset_present{true};

  VirtualAudioServiceHooks hooks;
  HookQuietService(&hooks);
  HookMonitor(&hooks, &rec);

  // "auto" resolves to the headset while it is plugged in, and to the built-in
  // speakers once it is gone, the way the Pulse default sink moves.
  auto start_monitor = hooks.start_mic_monitor;
  hooks.start_mic_monitor = [&](const MicMonitorConfig &cfg,
                                const std::string &source, MicMonitorState *out,
                                std::string *error) {
    MicMonitorConfig resolved = cfg;
    if (resolved.sink == "auto") {
      resolved.sink = headset_present.load(std::memory_order_relaxed)
                          ? std::string("headset_test_sink")
                          : std::string("speaker_test_sink");
    }
    return start_monitor(resolved, source, out, error);
  };

  VirtualAudioService service(std::move(hooks));
  const auto cfg = MonitorServiceConfig();

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  if (!WaitUntil(
          [&] {
            return service.Status().monitor_sink_active == "headset_test_sink";
          },
          1000ms)) {
    std::cerr << "the monitor did not start on the headset\n";
    service.Stop();
    return false;
  }

  // The check fails in the same window that the Pulse default moves to the
  // built-in speakers.
  const int starts_before = rec.starts.load(std::memory_order_relaxed);
  headset_present.store(false, std::memory_order_relaxed);
  rec.fail_detect.store(true, std::memory_order_relaxed);

  // The service checks the route every two seconds. `monitor_sink_active` is
  // cleared before the restart and written after it, so it is a safe point to
  // read the sink the restart used.
  const bool restarted = WaitUntil(
      [&] {
        return rec.starts.load(std::memory_order_relaxed) > starts_before &&
               !service.Status().monitor_sink_active.empty();
      },
      5000ms);
  if (!restarted) {
    std::cerr << "the service did not restart the monitor after the failed "
                 "check\n";
    service.Stop();
    return false;
  }

  bool ok = true;
  const auto st = service.Status();
  if (st.monitor_sink_active != "headset_test_sink") {
    std::cerr << "a failed check moved the monitor to '"
              << st.monitor_sink_active << "'\n";
    ok = false;
  }

  service.Stop();
  return ok;
}

// A daemon that is killed while the monitor plays leaves the tagged loopback
// loaded in Pulse. Nothing else removes it, so every service start clears it,
// even one that starts with the monitor turned off.
bool TestServiceClearsAStaleMonitorAtStart() {
  MonitorRecorder rec;
  VirtualAudioServiceHooks hooks;
  HookQuietService(&hooks);
  HookMonitor(&hooks, &rec);

  VirtualAudioService service(std::move(hooks));
  auto cfg = MonitorServiceConfig();
  cfg.monitor.enabled = false;

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  const bool cleared = WaitUntil(
      [&] { return rec.stops.load(std::memory_order_relaxed) >= 1; }, 1000ms);
  service.Stop();
  if (!cleared) {
    std::cerr << "a service start with the monitor off never cleared a stale "
                 "loopback\n";
    return false;
  }
  return true;
}

// A failed check, and a failed stop after it, say nothing about the loopback.
// The service must keep the knowledge that a route may still play and stop it
// when the service stops.
bool TestServiceStopsAMonitorItCanNoLongerSee() {
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
  if (!WaitUntil([&] { return service.Status().monitor_active; }, 1000ms)) {
    std::cerr << "the monitor did not start\n";
    service.Stop();
    return false;
  }

  // pactl breaks: the check, the start and the stop all fail.
  rec.fail_detect.store(true, std::memory_order_relaxed);
  rec.fail_start.store(true, std::memory_order_relaxed);
  rec.fail_stop.store(true, std::memory_order_relaxed);

  // The service checks the route every two seconds.
  if (!WaitUntil([&] { return !service.Status().monitor_active; }, 5000ms)) {
    std::cerr << "a failed check left the monitor state alone\n";
    service.Stop();
    return false;
  }

  // pactl works again, but the service no longer believes a route is running.
  rec.fail_stop.store(false, std::memory_order_relaxed);
  const int stops_before = rec.stops.load(std::memory_order_relaxed);

  service.Stop();

  const int stops_after = rec.stops.load(std::memory_order_relaxed);
  if (stops_after <= stops_before) {
    std::cerr << "the service stopped without unloading the loopback it could "
                 "no longer see\n";
    return false;
  }
  if (rec.running.load(std::memory_order_relaxed)) {
    std::cerr << "a monitor loopback outlived the service\n";
    return false;
  }
  return true;
}

// A lost output is an answer about the sink list, not about the module list,
// so it says nothing about the loopback. A stop that failed in the same window
// can have left one loaded, and it plays the microphone into the speakers
// until something removes it. The service must therefore keep the knowledge
// that a route may still play, and keep trying to remove it, while the output
// is lost.
bool TestServiceCleansUpTheLoopbackAfterALostOutput() {
  MonitorRecorder rec;
  std::atomic<bool> sink_gone{false};
  std::atomic<bool> start_fails{false};

  VirtualAudioServiceHooks hooks;
  HookQuietService(&hooks);
  HookMonitor(&hooks, &rec);

  auto start_monitor = hooks.start_mic_monitor;
  hooks.start_mic_monitor = [&](const MicMonitorConfig &cfg,
                                const std::string &source, MicMonitorState *out,
                                std::string *error) {
    if (start_fails.load(std::memory_order_relaxed)) {
      // A start that fails leaves the loopback of the last start alone: a
      // sound server that cannot load a module cannot unload one either.
      rec.starts.fetch_add(1, std::memory_order_relaxed);
      if (error)
        *error = "synthetic monitor start failure";
      return false;
    }
    return start_monitor(cfg, source, out, error);
  };
  hooks.mic_monitor_sink_present = [&](const std::string &,
                                       std::string *error) {
    if (error)
      error->clear();
    return std::optional<bool>(!sink_gone.load(std::memory_order_relaxed));
  };

  VirtualAudioService service(std::move(hooks));
  const auto cfg = MonitorServiceConfig();

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }
  if (!WaitUntil([&] { return service.Status().monitor_active; }, 1000ms)) {
    std::cerr << "the monitor did not start\n";
    service.Stop();
    return false;
  }

  // The sound server goes wrong: the check, the start and the stop all fail.
  // The loopback of the first start is still loaded.
  rec.fail_detect.store(true, std::memory_order_relaxed);
  start_fails.store(true, std::memory_order_relaxed);
  rec.fail_stop.store(true, std::memory_order_relaxed);
  if (!WaitUntil([&] { return !service.Status().monitor_active; }, 5000ms)) {
    std::cerr << "a failed check left the monitor state alone\n";
    service.Stop();
    return false;
  }

  // The output is gone as well, and only the failed start can find it.
  sink_gone.store(true, std::memory_order_relaxed);
  if (!WaitUntil(
          [&] {
            return service.Status().monitor_note.find("disappeared") !=
                   std::string::npos;
          },
          6000ms)) {
    std::cerr << "the lost monitor output was not reported: note='"
              << service.Status().monitor_note << "'\n";
    service.Stop();
    return false;
  }

  const int stops_before = rec.stops.load(std::memory_order_relaxed);
  if (!WaitUntil(
          [&] {
            return rec.stops.load(std::memory_order_relaxed) >= stops_before + 2;
          },
          4000ms)) {
    std::cerr << "the lost output ended the cleanup of a loopback that may "
                 "still play\n";
    service.Stop();
    return false;
  }

  // A stop that works removes it, without anything the user must do.
  rec.fail_stop.store(false, std::memory_order_relaxed);
  bool ok = true;
  if (!WaitUntil([&] { return !rec.running.load(std::memory_order_relaxed); },
                 4000ms)) {
    std::cerr << "the loopback stayed loaded after the stop worked again\n";
    ok = false;
  }

  service.Stop();
  if (rec.running.load(std::memory_order_relaxed)) {
    std::cerr << "a monitor loopback outlived the service\n";
    ok = false;
  }
  return ok;
}

// The lost-output note says the monitor stopped. While the cleanup keeps
// failing that is not true: a loopback may still play the microphone into the
// speakers, which is the failure the lost-output stop exists to prevent. The
// note must say so while it lasts, and go back to the plain sentence when the
// stop works.
bool TestServiceSaysTheLostOutputMayStillPlay() {
  MonitorRecorder rec;
  std::atomic<bool> sink_gone{false};
  std::atomic<bool> start_fails{false};

  VirtualAudioServiceHooks hooks;
  HookQuietService(&hooks);
  HookMonitor(&hooks, &rec);

  auto start_monitor = hooks.start_mic_monitor;
  hooks.start_mic_monitor = [&](const MicMonitorConfig &cfg,
                                const std::string &source, MicMonitorState *out,
                                std::string *error) {
    if (start_fails.load(std::memory_order_relaxed)) {
      rec.starts.fetch_add(1, std::memory_order_relaxed);
      if (error)
        *error = "synthetic monitor start failure";
      return false;
    }
    return start_monitor(cfg, source, out, error);
  };
  hooks.mic_monitor_sink_present = [&](const std::string &,
                                       std::string *error) {
    if (error)
      error->clear();
    return std::optional<bool>(!sink_gone.load(std::memory_order_relaxed));
  };

  VirtualAudioService service(std::move(hooks));
  const auto cfg = MonitorServiceConfig();

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }
  if (!WaitUntil([&] { return service.Status().monitor_active; }, 1000ms)) {
    std::cerr << "the monitor did not start\n";
    service.Stop();
    return false;
  }

  // The sound server goes wrong and the output is gone: the loopback of the
  // first start stays loaded, because no stop works.
  rec.fail_detect.store(true, std::memory_order_relaxed);
  start_fails.store(true, std::memory_order_relaxed);
  rec.fail_stop.store(true, std::memory_order_relaxed);
  sink_gone.store(true, std::memory_order_relaxed);
  if (!WaitUntil(
          [&] {
            return service.Status().monitor_note.find("still hear") !=
                   std::string::npos;
          },
          10000ms)) {
    std::cerr << "the note said the monitor stopped while a loopback may "
                 "still play: note='"
              << service.Status().monitor_note << "'\n";
    service.Stop();
    return false;
  }

  bool ok = true;
  if (service.Status().monitor_note.find("disappeared") == std::string::npos) {
    std::cerr << "the note lost the sentence that says what to do: '"
              << service.Status().monitor_note << "'\n";
    ok = false;
  }

  // A stop that works ends it, and the note goes back to the plain sentence.
  rec.fail_stop.store(false, std::memory_order_relaxed);
  if (!WaitUntil(
          [&] {
            return service.Status().monitor_note.find("still hear") ==
                   std::string::npos;
          },
          6000ms)) {
    std::cerr << "the note still warned about the microphone after the stop "
                 "worked: '"
              << service.Status().monitor_note << "'\n";
    ok = false;
  }

  service.Stop();
  if (rec.running.load(std::memory_order_relaxed)) {
    std::cerr << "a monitor loopback outlived the service\n";
    ok = false;
  }
  return ok;
}

// The lost-output cleanup runs a stop on the audio supervisor thread, and in
// production a stop is a pactl process with a deadline. The state that drives
// the cleanup is terminal: only the user ends a lost output. A stop that keeps
// failing must therefore give up, or one deadline per backoff step blocks the
// supervisor for the rest of the run, and everything the user asks for waits
// behind it. Giving up keeps the knowledge that a loopback may still play, so
// the stop at shutdown still removes it.
bool TestServiceStopsRetryingTheLostOutputCleanup() {
  MonitorRecorder rec;
  std::atomic<bool> sink_gone{false};
  std::atomic<bool> start_fails{false};

  VirtualAudioServiceHooks hooks;
  HookQuietService(&hooks);
  HookMonitor(&hooks, &rec);

  auto start_monitor = hooks.start_mic_monitor;
  hooks.start_mic_monitor = [&](const MicMonitorConfig &cfg,
                                const std::string &source, MicMonitorState *out,
                                std::string *error) {
    if (start_fails.load(std::memory_order_relaxed)) {
      rec.starts.fetch_add(1, std::memory_order_relaxed);
      if (error)
        *error = "synthetic monitor start failure";
      return false;
    }
    return start_monitor(cfg, source, out, error);
  };
  hooks.mic_monitor_sink_present = [&](const std::string &,
                                       std::string *error) {
    if (error)
      error->clear();
    return std::optional<bool>(!sink_gone.load(std::memory_order_relaxed));
  };

  VirtualAudioService service(std::move(hooks));
  const auto cfg = MonitorServiceConfig();

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }
  if (!WaitUntil([&] { return service.Status().monitor_active; }, 1000ms)) {
    std::cerr << "the monitor did not start\n";
    service.Stop();
    return false;
  }

  // The sound server goes wrong and never comes back: the check, the start and
  // the stop all fail, and the output is gone as well.
  rec.fail_detect.store(true, std::memory_order_relaxed);
  start_fails.store(true, std::memory_order_relaxed);
  rec.fail_stop.store(true, std::memory_order_relaxed);
  sink_gone.store(true, std::memory_order_relaxed);
  if (!WaitUntil(
          [&] {
            return service.Status().monitor_note.find("disappeared") !=
                   std::string::npos;
          },
          8000ms)) {
    std::cerr << "the lost monitor output was not reported: note='"
              << service.Status().monitor_note << "'\n";
    service.Stop();
    return false;
  }

  // The cleanup must go quiet on its own. The backoff caps at eight steps of
  // the retry delay, so a window longer than one capped step with no new stop
  // says the retry ended.
  const int before = rec.stops.load(std::memory_order_relaxed);
  int seen = before;
  auto lastChange = std::chrono::steady_clock::now();
  const auto deadline = lastChange + 14000ms;
  bool settled = false;
  while (std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(20ms);
    const int now = rec.stops.load(std::memory_order_relaxed);
    if (now != seen) {
      seen = now;
      lastChange = std::chrono::steady_clock::now();
      continue;
    }
    if (std::chrono::steady_clock::now() - lastChange >= 2500ms) {
      settled = true;
      break;
    }
  }

  bool ok = true;
  if (!settled) {
    std::cerr << "the lost-output cleanup kept blocking the supervisor: "
              << (seen - before) << " stops and still going\n";
    ok = false;
  }
  if (seen - before > 8) {
    std::cerr << "the lost-output cleanup ran " << (seen - before)
              << " stops before it gave up\n";
    ok = false;
  }

  // Giving up keeps the knowledge, so the service still stops the route it
  // could not remove.
  const int before_shutdown = rec.stops.load(std::memory_order_relaxed);
  service.Stop();
  if (rec.stops.load(std::memory_order_relaxed) <= before_shutdown) {
    std::cerr << "the service stopped without trying to remove the loopback "
                 "the cleanup gave up on\n";
    ok = false;
  }
  return ok;
}

// The sink question is the one place that knows why the sound server could not
// answer, and "no answer" is the arm a monitor sits in while the sound server
// is unwell. A monitor stuck on a pinned start must leave that reason where a
// support case can read it.
bool TestServiceReportsWhyTheSinkQuestionHadNoAnswer() {
  MonitorRecorder rec;
  VirtualAudioServiceHooks hooks;
  HookQuietService(&hooks);
  HookMonitor(&hooks, &rec);
  hooks.mic_monitor_sink_present = [](const std::string &,
                                      std::string *error) {
    if (error)
      *error = "pactl list short sinks did not answer in time";
    return std::optional<bool>();
  };

  VirtualAudioService service(std::move(hooks));
  const auto cfg = MonitorServiceConfig();

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }
  if (!WaitUntil([&] { return service.Status().monitor_active; }, 1000ms)) {
    std::cerr << "the monitor did not start\n";
    service.Stop();
    return false;
  }

  // The sound server goes wrong, so the pinned start fails and asks about the
  // output it names.
  rec.fail_detect.store(true, std::memory_order_relaxed);
  rec.fail_start.store(true, std::memory_order_relaxed);
  const bool reported = WaitUntil(
      [&] {
        return service.Status().monitor_last_error.find("did not answer in "
                                                        "time") !=
               std::string::npos;
      },
      6000ms);
  bool ok = true;
  if (!reported) {
    std::cerr << "the unanswered sink question left no trace: '"
              << service.Status().monitor_last_error << "'\n";
    ok = false;
  }
  // The GUI reads this text, so a sound server that gave no answer must not
  // become "Open Support" on the page.
  if (service.Status().monitor_last_error.find(
          studiocast::audio::kSoundServerNoAnswerMessage) ==
      std::string::npos) {
    std::cerr << "the unanswered sink question did not use the shared text: '"
              << service.Status().monitor_last_error << "'\n";
    ok = false;
  }
  if (!service.Status().monitor_note.empty()) {
    std::cerr << "a sound server that gave no answer reported a lost output: '"
              << service.Status().monitor_note << "'\n";
    ok = false;
  }

  service.Stop();
  return ok;
}

// The third arm of the pin rule: no answer about the output keeps the pin and
// the retry, so a monitor that only waits for the sound server comes back on
// its own, on the output it played on before.
bool TestServiceKeepsThePinWhenTheSoundServerGivesNoAnswer() {
  MonitorRecorder rec;
  std::atomic<bool> answers{false};
  VirtualAudioServiceHooks hooks;
  HookQuietService(&hooks);
  HookMonitor(&hooks, &rec);
  hooks.mic_monitor_sink_present = [&](const std::string &,
                                       std::string *error) {
    if (answers.load(std::memory_order_relaxed)) {
      if (error)
        error->clear();
      return std::optional<bool>(true);
    }
    if (error)
      *error = "pactl list short sinks did not answer in time";
    return std::optional<bool>();
  };

  VirtualAudioService service(std::move(hooks));
  const auto cfg = MonitorServiceConfig();

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }
  if (!WaitUntil(
          [&] {
            return service.Status().monitor_sink_active == "physical_test_sink";
          },
          1000ms)) {
    std::cerr << "the monitor did not start\n";
    service.Stop();
    return false;
  }

  // The sound server is unreachable: the check and the start fail, and the
  // sink question has no answer.
  rec.fail_detect.store(true, std::memory_order_relaxed);
  rec.fail_start.store(true, std::memory_order_relaxed);
  const int starts_before = rec.starts.load(std::memory_order_relaxed);
  bool ok = true;
  if (!WaitUntil(
          [&] {
            return rec.starts.load(std::memory_order_relaxed) >=
                   starts_before + 2;
          },
          6000ms)) {
    std::cerr << "a sound server that gave no answer ended the retries\n";
    service.Stop();
    return false;
  }
  if (!service.Status().monitor_note.empty()) {
    std::cerr << "a sound server that gave no answer reported a lost output: '"
              << service.Status().monitor_note << "'\n";
    ok = false;
  }

  // The sound server comes back, and nothing the user does is needed.
  answers.store(true, std::memory_order_relaxed);
  rec.fail_detect.store(false, std::memory_order_relaxed);
  rec.fail_start.store(false, std::memory_order_relaxed);
  if (!WaitUntil(
          [&] {
            const auto st = service.Status();
            return st.monitor_active && st.monitor_last_error.empty();
          },
          6000ms)) {
    const auto st = service.Status();
    std::cerr << "the monitor did not come back on its own: active="
              << st.monitor_active << " error='" << st.monitor_last_error
              << "'\n";
    ok = false;
  }
  {
    std::lock_guard<std::mutex> lock(rec.mu);
    if (rec.last_sink != "physical_test_sink") {
      std::cerr << "the monitor came back on '" << rec.last_sink
                << "' instead of the output it played on\n";
      ok = false;
    }
  }

  service.Stop();
  return ok;
}

// A monitor that is on while microphone processing is off is a state the user
// made one click ago, not a failure. It belongs in the note, so nothing sends
// the user to Support for it.
bool TestServiceReportsAnIdleMonitorAsANote() {
  MonitorRecorder rec;
  VirtualAudioServiceHooks hooks;
  HookQuietService(&hooks);
  HookMonitor(&hooks, &rec);

  VirtualAudioService service(std::move(hooks));
  auto cfg = MonitorServiceConfig();
  cfg.enabled = false;

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  const bool noted = WaitUntil(
      [&] {
        const auto st = service.Status();
        return st.monitor_note.find("Microphone processing is off") !=
               std::string::npos;
      },
      1000ms);
  bool ok = true;
  if (!noted) {
    std::cerr << "the idle monitor was not reported as a note: '"
              << service.Status().monitor_note << "'\n";
    ok = false;
  }
  if (!service.Status().monitor_last_error.empty()) {
    std::cerr << "an ordinary idle monitor was reported as an error: '"
              << service.Status().monitor_last_error << "'\n";
    ok = false;
  }

  // Turning microphone processing on clears the note.
  cfg.enabled = true;
  service.UpdateConfig(cfg);
  if (!WaitUntil([&] { return service.Status().monitor_note.empty(); },
                 1000ms)) {
    std::cerr << "the idle note survived turning processing on\n";
    ok = false;
  }

  service.Stop();
  return ok;
}

// The volume belongs to the loopback sink input, not to the module, so a
// volume change applies in place. Reloading the module would break the sound
// once per step of the spin box.
bool TestServiceAppliesAVolumeChangeWithoutReloading() {
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
  if (!WaitUntil([&] { return service.Status().monitor_active; }, 1000ms)) {
    std::cerr << "the monitor did not start\n";
    service.Stop();
    return false;
  }

  const int starts_before = rec.starts.load(std::memory_order_relaxed);

  // Ten steps of the volume spin box.
  for (int volume = 90; volume > 80; --volume) {
    cfg.monitor.volume = volume;
    service.UpdateConfig(cfg);
    if (!WaitUntil(
            [&] {
              std::lock_guard<std::mutex> lock(rec.mu);
              return rec.last_volume == volume;
            },
            1000ms)) {
      std::cerr << "the monitor volume did not follow " << volume << "\n";
      service.Stop();
      return false;
    }
  }

  const int reloads =
      rec.starts.load(std::memory_order_relaxed) - starts_before;
  const int applied = rec.volume_calls.load(std::memory_order_relaxed);
  service.Stop();

  bool ok = true;
  if (reloads != 0) {
    std::cerr << "volume-only changes reloaded the loopback " << reloads
              << " times\n";
    ok = false;
  }
  if (applied < 10) {
    std::cerr << "the volume was applied in place only " << applied
              << " times\n";
    ok = false;
  }
  return ok;
}

// The monitor loops from `studiocast_mic`. Without the virtual microphone
// there is nothing to loop from, so the retry could never succeed. The service
// waits instead of running pactl forever.
bool TestServiceWaitsForTheVirtualMicrophoneBeforeMonitoring() {
  MonitorRecorder rec;
  VirtualAudioServiceHooks hooks;
  HookQuietService(&hooks);
  HookMonitor(&hooks, &rec);

  VirtualAudioService service(std::move(hooks));
  auto cfg = MonitorServiceConfig();
  cfg.create_virtual_mic = false;

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  const bool explained = WaitUntil(
      [&] {
        return service.Status().monitor_note.find("StudioCast Microphone") !=
               std::string::npos;
      },
      1000ms);
  const int starts = rec.starts.load(std::memory_order_relaxed);

  // The virtual microphone appears, so the monitor starts.
  cfg.create_virtual_mic = true;
  service.UpdateConfig(cfg);
  const bool started =
      WaitUntil([&] { return service.Status().monitor_active; }, 1000ms);
  service.Stop();

  bool ok = true;
  if (!explained) {
    std::cerr << "the missing virtual microphone was not explained\n";
    ok = false;
  }
  if (starts != 0) {
    std::cerr << "the monitor tried to start " << starts
              << " times without a virtual microphone\n";
    ok = false;
  }
  if (!started) {
    std::cerr << "the monitor did not start once the microphone appeared\n";
    ok = false;
  }
  return ok;
}

// The output the last start really used is pinned across a restart the user
// did not ask for, but the pin must not outlive the output it names. While the
// output is there a failed start keeps the pin, because a sound server hiccup
// must not move the monitor. Once the output is gone the pin goes with it:
// the service stops, says what happened, and a later restart resolves "auto"
// afresh. Without the expiry a headset unplugged in the same window as a
// failed check leaves the monitor asking for a dead sink for ever.
bool TestServiceDropsThePinnedOutputOnlyWhenItIsGone() {
  MonitorRecorder rec;
  std::atomic<bool> headset_present{true};
  std::atomic<bool> start_fails{false};
  std::mutex asked_mu;
  std::string asked_sink;

  VirtualAudioServiceHooks hooks;
  HookQuietService(&hooks);
  HookMonitor(&hooks, &rec);

  // "auto" resolves to the headset while it is plugged in, and to the built-in
  // speakers once it is gone, the way the Pulse default sink moves.
  auto start_monitor = hooks.start_mic_monitor;
  hooks.start_mic_monitor = [&](const MicMonitorConfig &cfg,
                                const std::string &source, MicMonitorState *out,
                                std::string *error) {
    MicMonitorConfig resolved = cfg;
    if (resolved.sink == "auto") {
      resolved.sink = headset_present.load(std::memory_order_relaxed)
                          ? std::string("headset_test_sink")
                          : std::string("speaker_test_sink");
    }
    {
      std::lock_guard<std::mutex> lock(asked_mu);
      asked_sink = resolved.sink;
    }
    const bool gone = resolved.sink == "headset_test_sink" &&
                      !headset_present.load(std::memory_order_relaxed);
    if (gone || start_fails.load(std::memory_order_relaxed)) {
      rec.starts.fetch_add(1, std::memory_order_relaxed);
      if (error)
        *error = "sink '" + resolved.sink + "' does not exist";
      return false;
    }
    return start_monitor(resolved, source, out, error);
  };
  hooks.mic_monitor_sink_present = [&](const std::string &sink,
                                       std::string *error) {
    if (error)
      error->clear();
    if (sink == "headset_test_sink")
      return std::optional<bool>(
          headset_present.load(std::memory_order_relaxed));
    return std::optional<bool>(true);
  };

  VirtualAudioService service(std::move(hooks));
  auto cfg = MonitorServiceConfig();

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  if (!WaitUntil(
          [&] {
            return service.Status().monitor_sink_active == "headset_test_sink";
          },
          1000ms)) {
    std::cerr << "the monitor did not start on the headset\n";
    service.Stop();
    return false;
  }

  // The check fails, so the restart path takes over, and the start fails too
  // while the headset is still plugged in.
  bool ok = true;
  int starts_before = rec.starts.load(std::memory_order_relaxed);
  start_fails.store(true, std::memory_order_relaxed);
  rec.fail_detect.store(true, std::memory_order_relaxed);
  if (!WaitUntil(
          [&] {
            return rec.starts.load(std::memory_order_relaxed) >=
                   starts_before + 2;
          },
          6000ms)) {
    std::cerr << "the service stopped retrying while the output was there\n";
    service.Stop();
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(asked_mu);
    if (asked_sink != "headset_test_sink") {
      std::cerr << "a failed start moved the monitor to '" << asked_sink
                << "' while the headset was still there\n";
      ok = false;
    }
  }
  if (!service.Status().monitor_note.empty()) {
    std::cerr << "a failed start reported a lost output while the output was "
                 "there: '"
              << service.Status().monitor_note << "'\n";
    ok = false;
  }

  // The headset is unplugged. The check is still failing, so the lost output
  // can only be found by the start that asks for it.
  headset_present.store(false, std::memory_order_relaxed);
  start_fails.store(false, std::memory_order_relaxed);
  if (!WaitUntil(
          [&] {
            const auto st = service.Status();
            return !st.monitor_active &&
                   st.monitor_note.find("headset_test_sink") !=
                       std::string::npos;
          },
          6000ms)) {
    const auto st = service.Status();
    std::cerr << "the lost monitor output was not reported: note='"
              << st.monitor_note << "' error='" << st.monitor_last_error
              << "'\n";
    service.Stop();
    return false;
  }
  if (!service.Status().monitor_last_error.empty()) {
    std::cerr << "the lost output was also reported as an error: '"
              << service.Status().monitor_last_error << "'\n";
    ok = false;
  }

  // Nothing may go on asking the sound server for a sink that is gone.
  starts_before = rec.starts.load(std::memory_order_relaxed);
  std::this_thread::sleep_for(1000ms);
  if (rec.starts.load(std::memory_order_relaxed) != starts_before) {
    std::cerr << "the monitor kept retrying the output that is gone\n";
    ok = false;
  }

  // Turning the monitor off and on again is an explicit restart, and the pin
  // is gone, so the service resolves the default afresh.
  rec.fail_detect.store(false, std::memory_order_relaxed);
  cfg.monitor.enabled = false;
  service.UpdateConfig(cfg);
  if (!WaitUntil([&] { return service.Status().monitor_note.empty(); },
                 2000ms)) {
    std::cerr << "the lost-output note survived turning the monitor off\n";
    ok = false;
  }
  cfg.monitor.enabled = true;
  service.UpdateConfig(cfg);
  if (!WaitUntil(
          [&] {
            return service.Status().monitor_sink_active == "speaker_test_sink";
          },
          2000ms)) {
    std::cerr << "the monitor did not resolve the default again after the "
                 "output was lost\n";
    ok = false;
  }

  service.Stop();
  return ok;
}

// A monitor setting that can never work would otherwise run about four pactl
// processes every retry, for the whole life of the daemon. Repeated failures
// must space the retries out.
bool TestServiceBacksOffRepeatedMonitorStartFailures() {
  MonitorRecorder rec;
  rec.fail_start.store(true, std::memory_order_relaxed);

  VirtualAudioServiceHooks hooks;
  HookQuietService(&hooks);
  HookMonitor(&hooks, &rec);

  VirtualAudioService service(std::move(hooks));
  const auto cfg = MonitorServiceConfig(); // 250 ms floor between retries

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  std::this_thread::sleep_for(3000ms);
  const int starts = rec.starts.load(std::memory_order_relaxed);
  service.Stop();

  bool ok = true;
  if (starts < 2) {
    std::cerr << "the monitor gave up after " << starts << " tries\n";
    ok = false;
  }
  // A fixed 250 ms retry would be about twelve tries in three seconds.
  if (starts > 7) {
    std::cerr << "the monitor retried " << starts
              << " times in three seconds, so it did not back off\n";
    ok = false;
  }
  return ok;
}

// A sound server that cannot remove the leftover loopback keeps the stop
// retry going. Without a backoff that retry runs about four pactl processes
// four times a second, for ever, with the monitor turned off.
bool TestServiceBacksOffRepeatedMonitorStopFailures() {
  MonitorRecorder rec;
  rec.fail_stop.store(true, std::memory_order_relaxed);

  VirtualAudioServiceHooks hooks;
  HookQuietService(&hooks);
  HookMonitor(&hooks, &rec);

  VirtualAudioService service(std::move(hooks));
  auto cfg = MonitorServiceConfig(); // 250 ms floor between retries
  // The monitor is off the whole time. The start-up cleanup still runs, and
  // its failure hands the route to the stop retry.
  cfg.monitor.enabled = false;

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  std::this_thread::sleep_for(3000ms);
  const int stops = rec.stops.load(std::memory_order_relaxed);
  service.Stop();

  bool ok = true;
  if (stops < 2) {
    std::cerr << "the monitor gave up the stop after " << stops << " tries\n";
    ok = false;
  }
  // A fixed 250 ms retry would be about twelve tries in three seconds.
  if (stops > 7) {
    std::cerr << "the monitor tried to stop " << stops
              << " times in three seconds, so it did not back off\n";
    ok = false;
  }
  return ok;
}

// The stop backoff spaces out a stop that keeps failing, but it must not also
// delay the next stop the user asks for. A monitor the user wants again starts
// its stop wait over, so unticking the box again stops the microphone playing
// in the headphones at once and not after the leftover wait.
bool TestServiceStartsTheStopWaitOverWhenTheMonitorIsWantedAgain() {
  MonitorRecorder rec;
  rec.fail_stop.store(true, std::memory_order_relaxed);

  VirtualAudioServiceHooks hooks;
  HookQuietService(&hooks);
  HookMonitor(&hooks, &rec);

  VirtualAudioService service(std::move(hooks));
  auto cfg = MonitorServiceConfig(); // 250 ms floor between retries
  // The monitor is off, so the start-up cleanup runs and its failures build
  // the stop backoff up.
  cfg.monitor.enabled = false;

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  // Five failed stops put the next stop retry 1250 ms away.
  if (!WaitUntil([&] { return rec.stops.load(std::memory_order_relaxed) >= 5; },
                 6000ms)) {
    std::cerr << "the service did not retry the failed stop\n";
    service.Stop();
    return false;
  }

  // The user turns the monitor on again, which is where the stop wait starts
  // over, and the sound server answers again.
  cfg.monitor.enabled = true;
  service.UpdateConfig(cfg);
  rec.fail_stop.store(false, std::memory_order_relaxed);
  if (!WaitUntil([&] { return service.Status().monitor_active; }, 2000ms)) {
    std::cerr << "the monitor did not start when it was wanted again\n";
    service.Stop();
    return false;
  }

  // The user unticks the box.
  const int stops_before = rec.stops.load(std::memory_order_relaxed);
  const auto asked = std::chrono::steady_clock::now();
  cfg.monitor.enabled = false;
  service.UpdateConfig(cfg);
  const bool stopped = WaitUntil(
      [&] {
        return rec.stops.load(std::memory_order_relaxed) > stops_before;
      },
      3000ms);
  const auto waited = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - asked);
  service.Stop();

  if (!stopped) {
    std::cerr << "the monitor was never stopped\n";
    return false;
  }
  // The floor between two stop retries is 250 ms, so anything past that is a
  // deadline the earlier failures left behind.
  if (waited > 500ms) {
    std::cerr << "the stop waited " << waited.count()
              << " ms for a deadline the earlier failures left behind\n";
    return false;
  }
  return true;
}

// The volume retry runs `pactl list sink-inputs` on every try, so a volume
// that can never be set must space its retries out the same way.
bool TestServiceBacksOffRepeatedMonitorVolumeFailures() {
  MonitorRecorder rec;
  rec.fail_volume.store(true, std::memory_order_relaxed);

  VirtualAudioServiceHooks hooks;
  HookQuietService(&hooks);
  HookMonitor(&hooks, &rec);

  VirtualAudioService service(std::move(hooks));
  auto cfg = MonitorServiceConfig(); // 250 ms floor between retries

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  if (!WaitUntil([&] { return service.Status().monitor_active; }, 1000ms)) {
    std::cerr << "the monitor did not start\n";
    service.Stop();
    return false;
  }

  const int calls_before = rec.volume_calls.load(std::memory_order_relaxed);
  cfg.monitor.volume = 60;
  service.UpdateConfig(cfg);

  std::this_thread::sleep_for(3000ms);
  const int calls =
      rec.volume_calls.load(std::memory_order_relaxed) - calls_before;
  service.Stop();

  bool ok = true;
  if (calls < 2) {
    std::cerr << "the monitor gave up the volume after " << calls
              << " tries\n";
    ok = false;
  }
  // A fixed 250 ms retry would be about twelve tries in three seconds.
  if (calls > 7) {
    std::cerr << "the monitor tried to set the volume " << calls
              << " times in three seconds, so it did not back off\n";
    ok = false;
  }
  return ok;
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

// Whole-lifecycle simulation: the service drives the real monitor helper and
// only Pulse is faked, so the recorded pactl transcript proves the wiring.
bool TestServiceDrivesRealHelperThroughPactl() {
  std::mutex log_mu;
  std::vector<std::string> log;
  std::atomic<bool> monitor_loaded{false};

  ScopedPactlExecHook hook([&](const std::string &command) {
    {
      std::lock_guard<std::mutex> lock(log_mu);
      log.push_back(command);
    }

    if (command == "pactl --version 2>&1")
      return ExecResult(0, "pactl 17.0\n");
    if (command == "pactl get-default-sink 2>&1")
      return ExecResult(0, "physical_test_sink\n");
    if (command == "pactl get-default-source 2>&1")
      return ExecResult(0, "physical_test_mic\n");
    if (command == "pactl list short sources 2>&1") {
      return ExecResult(0, "1\tstudiocast_sink.monitor\tmodule-null-sink.c\t"
                           "s16le 2ch 48000Hz\n"
                           "2\tphysical_test_mic\tmodule-alsa-card.c\t"
                           "s16le 2ch 48000Hz\n");
    }
    if (command == "pactl list short sinks 2>&1") {
      return ExecResult(0, "3\tphysical_test_sink\tmodule-alsa-card.c\t"
                           "s16le 2ch 48000Hz\tSUSPENDED\n");
    }
    if (command == "pactl list short modules 2>&1") {
      if (!monitor_loaded.load(std::memory_order_relaxed))
        return ExecResult(0,
                          "10\tmodule-null-sink\tsink_name=studiocast_sink\n");
      return ExecResult(0, "10\tmodule-null-sink\tsink_name=studiocast_sink\n"
                           "551\tmodule-loopback\tsource=studiocast_mic "
                           "sink=physical_test_sink latency_msec=20 "
                           "source_output_properties=media.name="
                           "StudioCast_Microphone_Monitor "
                           "sink_input_properties=media.name="
                           "StudioCast_Microphone_Monitor\n");
    }
    if (command.rfind("pactl load-module ", 0) == 0) {
      monitor_loaded.store(true, std::memory_order_relaxed);
      return ExecResult(0, "551\n");
    }
    if (command == "pactl unload-module 551 2>&1") {
      monitor_loaded.store(false, std::memory_order_relaxed);
      return ExecResult(0, "");
    }
    if (command == "pactl list sink-inputs 2>&1") {
      return ExecResult(0,
                        "Sink Input #900\n"
                        "\tOwner Module: 551\n"
                        "\tSink: 3\n"
                        "\t\tmedia.name = \"StudioCast_Microphone_Monitor\"\n"
                        "\n");
    }
    if (command.rfind("pactl set-sink-input-volume ", 0) == 0)
      return ExecResult(0, "");
    return ExecResult(0, "");
  });

  VirtualAudioServiceHooks hooks;
  HookQuietService(&hooks);

  VirtualAudioService service(std::move(hooks));
  auto cfg = MonitorServiceConfig();
  cfg.monitor.volume = 55;

  std::string err;
  if (!service.Start(cfg, &err)) {
    std::cerr << "service.Start failed: " << err << "\n";
    return false;
  }

  const bool active = WaitUntil(
      [&] {
        const auto st = service.Status();
        return st.monitor_active && st.monitor_module_id == 551;
      },
      1000ms);
  if (!active) {
    std::cerr << "the real helper did not bring the monitor up; error='"
              << service.Status().monitor_last_error << "'\n";
    service.Stop();
    return false;
  }

  service.Stop();

  std::vector<std::string> recorded;
  {
    std::lock_guard<std::mutex> lock(log_mu);
    recorded = log;
  }

  bool ok = true;
  const std::string wantLoad =
      "pactl load-module 'module-loopback' 'source=studiocast_mic' "
      "'sink=physical_test_sink' 'latency_msec=20' "
      "'source_output_properties=media.name=StudioCast_Microphone_Monitor' "
      "'sink_input_properties=media.name=StudioCast_Microphone_Monitor' 2>&1";
  if (!CommandWasRun(recorded, wantLoad)) {
    std::cerr << "the service did not issue the monitor load-module command\n";
    ok = false;
  }
  if (!CommandWasRun(recorded, "pactl set-sink-input-volume 900 55% 2>&1")) {
    std::cerr << "the service did not apply the monitor volume\n";
    ok = false;
  }
  if (!CommandWasRun(recorded, "pactl unload-module 551 2>&1")) {
    std::cerr << "the service did not unload the monitor when it stopped\n";
    ok = false;
  }
  if (monitor_loaded.load(std::memory_order_relaxed)) {
    std::cerr << "a monitor loopback outlived the service\n";
    ok = false;
  }
  return ok;
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
      {"monitor stop reports nothing to clean without pactl",
       &TestStopReportsNothingToCleanWithoutPactl},
      {"monitor stop reports a failure when pactl times out",
       &TestStopReportsAFailureWhenPactlTimesOut},
      {"monitor start reports a timed-out pactl as a busy sound server",
       &TestStartReportsATimedOutPactlAsABusySoundServer},
      {"monitor sink question asks the sound server once",
       &TestSinkPresentAsksTheSoundServerOnce},
      {"monitor sink question gives no answer without pactl",
       &TestSinkPresentGivesNoAnswerWithoutPactl},
      {"monitor sink question gives no answer when the sink list times out",
       &TestSinkPresentGivesNoAnswerWhenTheSinkListTimesOut},
      {"service starts and stops the monitor with config",
       &TestServiceStartsAndStopsMonitorWithConfig},
      {"service restarts the monitor on a sink change",
       &TestServiceRestartsMonitorOnSinkChange},
      {"service stops the monitor when it stops",
       &TestServiceStopsMonitorWhenServiceStops},
      {"service retries the monitor and reports the error",
       &TestServiceRetriesMonitorAndReportsError},
      {"service clears a start failure when the monitor is turned off",
       &TestServiceClearsAStartFailureWhenTheMonitorIsTurnedOff},
      {"service clears a volume error after a later step works",
       &TestServiceClearsAVolumeErrorAfterALaterStepWorks},
      {"service clears the monitor error when it stops",
       &TestServiceClearsMonitorErrorWhenItStops},
      {"service treats a failed monitor check as stopped",
       &TestServiceTreatsAFailedMonitorCheckAsStopped},
      {"service stops the monitor when the resolved output disappears",
       &TestServiceStopsTheMonitorWhenTheResolvedOutputDisappears},
      {"service reports a lost output as a note",
       &TestServiceReportsALostOutputAsANote},
      {"service keeps the resolved output across a failed check",
       &TestServiceKeepsTheResolvedOutputAcrossAFailedCheck},
      {"service drops the pinned output only when it is gone",
       &TestServiceDropsThePinnedOutputOnlyWhenItIsGone},
      {"service backs off repeated monitor start failures",
       &TestServiceBacksOffRepeatedMonitorStartFailures},
      {"service backs off repeated monitor stop failures",
       &TestServiceBacksOffRepeatedMonitorStopFailures},
      {"service starts the stop wait over when the monitor is wanted again",
       &TestServiceStartsTheStopWaitOverWhenTheMonitorIsWantedAgain},
      {"service backs off repeated monitor volume failures",
       &TestServiceBacksOffRepeatedMonitorVolumeFailures},
      {"service waits for the virtual microphone before monitoring",
       &TestServiceWaitsForTheVirtualMicrophoneBeforeMonitoring},
      {"service applies a volume change without reloading",
       &TestServiceAppliesAVolumeChangeWithoutReloading},
      {"service reports an idle monitor as a note",
       &TestServiceReportsAnIdleMonitorAsANote},
      {"service clears a stale monitor at start",
       &TestServiceClearsAStaleMonitorAtStart},
      {"service stops a monitor it can no longer see",
       &TestServiceStopsAMonitorItCanNoLongerSee},
      {"service cleans up the loopback after a lost output",
       &TestServiceCleansUpTheLoopbackAfterALostOutput},
      {"service says a lost output may still play",
       &TestServiceSaysTheLostOutputMayStillPlay},
      {"service stops retrying the lost-output cleanup",
       &TestServiceStopsRetryingTheLostOutputCleanup},
      {"service reports why the sink question had no answer",
       &TestServiceReportsWhyTheSinkQuestionHadNoAnswer},
      {"service keeps the pin when the sound server gives no answer",
       &TestServiceKeepsThePinWhenTheSoundServerGivesNoAnswer},
      {"monitor consumer is counted apart from apps",
       &TestMonitorConsumerIsCountedApartFromApps},
      {"service drives the real helper through pactl",
       &TestServiceDrivesRealHelperThroughPactl},
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
