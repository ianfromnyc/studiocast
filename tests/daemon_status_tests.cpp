#include <filesystem>
#include <iostream>
#include <string>

#include "core/audio/pulse/pactl.h"
#include "core/util/exec.h"

#define main studiocastd_main_disabled_for_tests
#include "daemon/studiocastd_main.cpp"
#undef main

namespace {

bool Expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << "\n";
    return false;
  }
  return true;
}

const JsonObject *ObjectAt(const JsonObject &obj, const std::string &key,
                           const char *message) {
  const JsonObject *child = JsonObjectField(obj, key);
  if (!child)
    std::cerr << message << "\n";
  return child;
}

const std::string *StringAt(const JsonObject &obj, const std::string &key,
                            const char *message) {
  const std::string *value = JsonStringField(obj, key);
  if (!value)
    std::cerr << message << "\n";
  return value;
}

const JsonArray *ArrayAt(const JsonObject &obj, const std::string &key,
                         const char *message) {
  const auto it = obj.find(key);
  if (it == obj.end()) {
    std::cerr << message << "\n";
    return nullptr;
  }
  const JsonArray *value = it->second.AsArray();
  if (!value)
    std::cerr << message << "\n";
  return value;
}

struct ReadinessFields {
  std::string state;
  std::string reason;
  std::string backend;
  bool present = false;
};

ReadinessFields ReadinessEntryFor(const std::string &statusJson,
                                  const std::string &effectId) {
  ReadinessFields out;
  studiocast::util::json::Value rootValue;
  std::string error;
  if (!studiocast::util::json::Parse(statusJson, &rootValue, &error)) {
    std::cerr << "status JSON should parse: " << error << "\n";
    return out;
  }

  const JsonObject *root = rootValue.AsObject();
  if (!root) {
    std::cerr << "status root should be an object\n";
    return out;
  }

  const JsonObject *video = ObjectAt(*root, "video", "video should exist");
  if (!video)
    return out;
  const JsonObject *readiness =
      ObjectAt(*video, "effect_readiness", "effect_readiness should exist");
  if (!readiness)
    return out;
  const JsonObject *entry = ObjectAt(*readiness, effectId.c_str(),
                                     "effect readiness entry should exist");
  if (!entry)
    return out;

  const std::string *state =
      StringAt(*entry, "state", "readiness state should exist");
  const std::string *reason =
      StringAt(*entry, "reason", "readiness reason should exist");
  const std::string *backend =
      StringAt(*entry, "backend", "readiness backend should exist");
  if (!state || !reason || !backend)
    return out;

  out.state = *state;
  out.reason = *reason;
  out.backend = *backend;
  out.present = true;
  return out;
}

std::string StatusForEffects(
    const studiocast::video::effects::BroadcastCameraEffects &effects) {
  studiocast::video::VirtualCameraServiceStatus videoStatus;
  videoStatus.service_running = true;
  videoStatus.virtual_device_present = true;
  videoStatus.virtual_device_available = true;

  studiocast::video::VirtualCameraServiceConfig videoConfig;
  videoConfig.enabled = true;
  videoConfig.pipeline.effects = effects;

  studiocast::audio::VirtualAudioServiceStatus audioStatus;
  audioStatus.service_running = true;
  audioStatus.mic_present = true;
  audioStatus.speakers_present = true;

  studiocast::audio::VirtualAudioServiceConfig audioConfig;

  return StatusToJson(videoStatus, videoConfig, audioStatus, audioConfig,
                      std::filesystem::path("/tmp/studiocastd-test.sock"),
                      /*maxineJson=*/"", /*openCudaJson=*/"",
                      /*openAudioJson=*/"", /*loopbackJson=*/"");
}

std::string StatusForVideoConfig(
    const studiocast::video::VirtualCameraServiceConfig &videoConfig) {
  studiocast::video::VirtualCameraServiceStatus videoStatus;
  videoStatus.service_running = true;
  videoStatus.virtual_device_present = true;
  videoStatus.virtual_device_available = true;

  studiocast::audio::VirtualAudioServiceStatus audioStatus;
  audioStatus.service_running = true;
  audioStatus.mic_present = true;
  audioStatus.speakers_present = true;

  studiocast::audio::VirtualAudioServiceConfig audioConfig;

  return StatusToJson(videoStatus, videoConfig, audioStatus, audioConfig,
                      std::filesystem::path("/tmp/studiocastd-test.sock"),
                      /*maxineJson=*/"", /*openCudaJson=*/"",
                      /*openAudioJson=*/"", /*loopbackJson=*/"");
}

bool TestVideoStatusReportsAllowCpuResize() {
  studiocast::video::VirtualCameraServiceConfig videoConfig;
  videoConfig.pipeline.allow_cpu_resize = false;

  studiocast::util::json::Value rootValue;
  std::string error;
  if (!studiocast::util::json::Parse(StatusForVideoConfig(videoConfig),
                                     &rootValue, &error)) {
    std::cerr << "status JSON should parse: " << error << "\n";
    return false;
  }

  const JsonObject *root = rootValue.AsObject();
  if (!root) {
    std::cerr << "status root should be an object\n";
    return false;
  }

  const JsonObject *video = ObjectAt(*root, "video", "video should exist");
  if (!video)
    return false;

  return Expect(!JsonBoolField(*video, "allow_cpu_resize", true),
                "video status should report allow_cpu_resize=false");
}

bool TestVideoStatusReportsRequestedOutputFormat() {
  studiocast::video::VirtualCameraServiceConfig videoConfig;
  videoConfig.pipeline.output_format = studiocast::video::PixelFormat::yuyv;

  studiocast::util::json::Value rootValue;
  std::string error;
  if (!studiocast::util::json::Parse(StatusForVideoConfig(videoConfig),
                                     &rootValue, &error)) {
    std::cerr << "status JSON should parse: " << error << "\n";
    return false;
  }

  const JsonObject *root = rootValue.AsObject();
  if (!root) {
    std::cerr << "status root should be an object\n";
    return false;
  }

  const JsonObject *video = ObjectAt(*root, "video", "video should exist");
  if (!video)
    return false;

  const std::string *requested =
      StringAt(*video, "output_format_requested",
               "video status should include requested output format");
  if (!requested)
    return false;

  const JsonObject *actual =
      ObjectAt(*video, "output_format", "actual output_format should exist");
  if (!actual)
    return false;

  studiocast::util::json::Value configValue;
  if (!studiocast::util::json::Parse(ConfigToJson(videoConfig), &configValue,
                                     &error)) {
    std::cerr << "config JSON should parse: " << error << "\n";
    return false;
  }
  const JsonObject *config = configValue.AsObject();
  if (!config) {
    std::cerr << "config root should be an object\n";
    return false;
  }
  const std::string *configRequested =
      StringAt(*config, "output_format_requested",
               "config should include requested output format");
  if (!configRequested)
    return false;

  return Expect(*requested == "yuyv",
                "video status should report yuyv requested output format") &&
         Expect(*configRequested == "yuyv",
                "video config should report yuyv requested output format") &&
         Expect(actual->find("width") != actual->end(),
                "actual output_format should remain negotiated object");
}

bool TestVideoStatusReportsCaptureFallbackState() {
  studiocast::video::VirtualCameraServiceStatus videoStatus;
  videoStatus.service_running = true;
  videoStatus.virtual_device_present = true;
  videoStatus.virtual_device_available = true;
  videoStatus.pipeline.running = true;
  videoStatus.pipeline.capture_fallback_state =
      "raw_after_mjpeg_decode_failure";
  videoStatus.pipeline.capture_fallback_reason =
      "MJPEG decode failed: synthetic bad frame.";

  studiocast::video::VirtualCameraServiceConfig videoConfig;
  videoConfig.enabled = true;

  studiocast::audio::VirtualAudioServiceStatus audioStatus;
  audioStatus.service_running = true;
  audioStatus.mic_present = true;

  studiocast::audio::VirtualAudioServiceConfig audioConfig;

  studiocast::util::json::Value rootValue;
  std::string error;
  if (!studiocast::util::json::Parse(
          StatusToJson(videoStatus, videoConfig, audioStatus, audioConfig,
                       std::filesystem::path("/tmp/studiocastd-test.sock"),
                       /*maxineJson=*/"", /*openCudaJson=*/"",
                       /*openAudioJson=*/"", /*loopbackJson=*/""),
          &rootValue, &error)) {
    std::cerr << "status JSON should parse: " << error << "\n";
    return false;
  }

  const JsonObject *root = rootValue.AsObject();
  if (!root) {
    std::cerr << "status root should be an object\n";
    return false;
  }
  const JsonObject *video = ObjectAt(*root, "video", "video should exist");
  if (!video)
    return false;
  const JsonObject *fallback =
      ObjectAt(*video, "capture_fallback", "capture_fallback should exist");
  if (!fallback)
    return false;

  const std::string *state =
      StringAt(*fallback, "state", "fallback state should exist");
  const std::string *reason =
      StringAt(*fallback, "reason", "fallback reason should exist");
  if (!state || !reason)
    return false;

  return Expect(*state == "raw_after_mjpeg_decode_failure",
                "status should report capture fallback state") &&
         Expect(reason->find("MJPEG decode failed") != std::string::npos,
                "status should report capture fallback reason");
}

bool TestVideoStatusReportsConfiguredDevicesWhenPipelineIdle() {
  studiocast::video::VirtualCameraServiceStatus videoStatus;
  videoStatus.service_running = true;
  videoStatus.virtual_device_present = true;
  videoStatus.virtual_device_available = true;

  studiocast::video::VirtualCameraServiceConfig videoConfig;
  videoConfig.enabled = true;
  videoConfig.pipeline.input_device = "/dev/video0";
  videoConfig.pipeline.output_device = "/dev/video10";

  studiocast::audio::VirtualAudioServiceStatus audioStatus;
  audioStatus.service_running = true;
  audioStatus.mic_present = true;

  studiocast::audio::VirtualAudioServiceConfig audioConfig;

  studiocast::util::json::Value rootValue;
  std::string error;
  if (!studiocast::util::json::Parse(
          StatusToJson(videoStatus, videoConfig, audioStatus, audioConfig,
                       std::filesystem::path("/tmp/studiocastd-test.sock"),
                       /*maxineJson=*/"", /*openCudaJson=*/"",
                       /*openAudioJson=*/"", /*loopbackJson=*/""),
          &rootValue, &error)) {
    std::cerr << "status JSON should parse: " << error << "\n";
    return false;
  }

  const JsonObject *root = rootValue.AsObject();
  if (!root) {
    std::cerr << "status root should be an object\n";
    return false;
  }
  const JsonObject *video = ObjectAt(*root, "video", "video should exist");
  if (!video)
    return false;

  const std::string *input =
      StringAt(*video, "input_device", "input_device should exist");
  const std::string *output =
      StringAt(*video, "output_device", "output_device should exist");
  if (!input || !output)
    return false;

  return Expect(*input == "/dev/video0",
                "idle video status should keep configured input device") &&
         Expect(*output == "/dev/video10",
                "idle video status should keep resolved output device");
}

bool TestExplicitOpenCudaEffectUnknownWithoutDiagnostics() {
  studiocast::video::effects::BroadcastCameraEffects effects;
  effects.engine =
      studiocast::video::effects::EffectsEnginePreference::open_cuda;
  effects.virtual_background.mode =
      studiocast::video::effects::VirtualBackgroundMode::blur;

  const ReadinessFields entry =
      ReadinessEntryFor(StatusForEffects(effects), "virtual_background.blur");
  if (!entry.present)
    return false;

  return Expect(entry.backend == "open_cuda",
                "explicit Open CUDA effect should keep backend attribution") &&
         Expect(entry.state == "unknown",
                "explicit Open CUDA readiness should be unknown when "
                "diagnostics are absent") &&
         Expect(entry.reason == "diagnostics_unavailable",
                "unknown readiness should explain missing diagnostics");
}

bool TestBuiltinEffectReadyWithoutDiagnostics() {
  studiocast::video::effects::BroadcastCameraEffects effects;
  effects.engine =
      studiocast::video::effects::EffectsEnginePreference::open_cuda;
  effects.vignette.enabled = true;

  const ReadinessFields entry =
      ReadinessEntryFor(StatusForEffects(effects), "vignette");
  if (!entry.present)
    return false;

  return Expect(entry.backend == "builtin",
                "vignette should report the builtin backend") &&
         Expect(entry.state == "ready",
                "builtin effects should remain ready without diagnostics");
}

bool TestAudioStatusReportsResolvedSourceAndWarnings() {
  studiocast::video::VirtualCameraServiceStatus videoStatus;
  studiocast::video::VirtualCameraServiceConfig videoConfig;

  studiocast::audio::VirtualAudioServiceStatus audioStatus;
  audioStatus.service_running = true;
  audioStatus.mic_present = true;
  audioStatus.selected_source = "alsa_input.usb_status_mic";
  audioStatus.source_warnings.push_back(
      "Using safe source 'alsa_input.usb_status_mic' instead of unsafe Pulse "
      "default source 'studiocast_speakers.monitor'.");

  studiocast::audio::VirtualAudioServiceConfig audioConfig;
  audioConfig.source_name.clear(); // auto

  studiocast::util::json::Value rootValue;
  std::string error;
  if (!studiocast::util::json::Parse(
          StatusToJson(videoStatus, videoConfig, audioStatus, audioConfig,
                       std::filesystem::path("/tmp/studiocastd-test.sock"),
                       /*maxineJson=*/"", /*openCudaJson=*/"",
                       /*openAudioJson=*/"", /*loopbackJson=*/""),
          &rootValue, &error)) {
    std::cerr << "status JSON should parse: " << error << "\n";
    return false;
  }

  const JsonObject *root = rootValue.AsObject();
  if (!root) {
    std::cerr << "status root should be an object\n";
    return false;
  }
  const JsonObject *audio = ObjectAt(*root, "audio", "audio should exist");
  if (!audio)
    return false;

  const std::string *source =
      StringAt(*audio, "source", "audio source should exist");
  const std::string *resolved =
      StringAt(*audio, "source_resolved", "resolved source should exist");
  const JsonArray *warnings =
      ArrayAt(*audio, "source_warnings", "source_warnings should exist");
  if (!source || !resolved || !warnings)
    return false;

  return Expect(*source == "auto",
                "configured source should remain auto in status") &&
         Expect(*resolved == "alsa_input.usb_status_mic",
                "status should report resolved physical source") &&
         Expect(!warnings->empty(),
                "status should propagate source resolution warnings");
}

bool TestAudioStatusPropagatesSourceErrorFromService() {
  studiocast::video::VirtualCameraServiceStatus videoStatus;
  studiocast::video::VirtualCameraServiceConfig videoConfig;

  studiocast::audio::VirtualAudioServiceStatus audioStatus;
  audioStatus.service_running = true;
  audioStatus.mic_present = true;
  audioStatus.selected_source = "alsa_input.disconnected_mic";
  audioStatus.source_error =
      "Configured Pulse source 'alsa_input.disconnected_mic' is not currently "
      "available.";

  studiocast::audio::VirtualAudioServiceConfig audioConfig;
  audioConfig.source_name = "alsa_input.disconnected_mic";

  studiocast::util::json::Value rootValue;
  std::string error;
  if (!studiocast::util::json::Parse(
          StatusToJson(videoStatus, videoConfig, audioStatus, audioConfig,
                       std::filesystem::path("/tmp/studiocastd-test.sock"),
                       /*maxineJson=*/"", /*openCudaJson=*/"",
                       /*openAudioJson=*/"", /*loopbackJson=*/""),
          &rootValue, &error)) {
    std::cerr << "status JSON should parse: " << error << "\n";
    return false;
  }

  const JsonObject *root = rootValue.AsObject();
  if (!root) {
    std::cerr << "status root should be an object\n";
    return false;
  }
  const JsonObject *audio = ObjectAt(*root, "audio", "audio should exist");
  if (!audio)
    return false;

  const std::string *source =
      StringAt(*audio, "source", "audio source should exist");
  const std::string *resolved =
      StringAt(*audio, "source_resolved", "resolved source should exist");
  const std::string *sourceError =
      StringAt(*audio, "source_error", "source_error should exist");
  if (!source || !resolved || !sourceError)
    return false;

  return Expect(*source == "alsa_input.disconnected_mic",
                "configured disconnected source should be preserved") &&
         Expect(*resolved == "alsa_input.disconnected_mic",
                "resolved source should still identify selected source") &&
         Expect(sourceError->find("not currently available") !=
                    std::string::npos,
                "source_error should propagate service availability error");
}

int NumberFieldOr(const JsonObject &obj, const std::string &key, int fallback) {
  const auto it = obj.find(key);
  if (it == obj.end())
    return fallback;
  const double *value = it->second.AsNumber();
  return value ? static_cast<int>(*value) : fallback;
}

bool TestAudioConfigJsonRoundTripsMonitor() {
  studiocast::audio::VirtualAudioServiceConfig cfg;
  cfg.monitor.enabled = true;
  cfg.monitor.sink = "alsa_output.usb_headset";
  cfg.monitor.latency_ms = 35;
  cfg.monitor.volume = 60;

  const std::string json = AudioConfigToJson(cfg);

  studiocast::audio::VirtualAudioServiceConfig parsed;
  std::vector<std::string> warnings;
  std::string error;
  if (!ApplyAudioConfigPatchJsonText(json, &parsed, &warnings, &error)) {
    std::cerr << "monitor config patch should apply: " << error << "\n";
    return false;
  }

  return Expect(parsed.monitor.enabled, "monitor enabled should round trip") &&
         Expect(parsed.monitor.sink == "alsa_output.usb_headset",
                "monitor sink should round trip") &&
         Expect(parsed.monitor.latency_ms == 35,
                "monitor latency should round trip") &&
         Expect(parsed.monitor.volume == 60,
                "monitor volume should round trip");
}

// A JSON number can also be far outside the int range, or an infinity when the
// exponent overflows a double. Converting either one to an int is undefined,
// so the parser must judge the double first and say what is wrong with it.
// The grammar has no NaN literal, so no case below can produce one.
bool TestAudioConfigPatchRejectsExtremeMonitorNumbers() {
  struct Case {
    const char *json;
    const char *want_error;
  };
  const Case cases[] = {
      {"{\"monitor\":{\"latency_ms\":1e300}}",
       "monitor.latency_ms out of range (expected 1..500)"},
      {"{\"monitor\":{\"latency_ms\":-1e300}}",
       "monitor.latency_ms out of range (expected 1..500)"},
      {"{\"monitor\":{\"volume\":1e400}}",
       "monitor.volume must be a finite number"},
      {"{\"monitor\":{\"volume\":-1e400}}",
       "monitor.volume must be a finite number"},
      {"{\"speaker_latency_ms\":1e300}",
       "speaker_latency_ms out of range (expected 1..5000)"},
  };

  bool ok = true;
  for (const Case &tc : cases) {
    studiocast::audio::VirtualAudioServiceConfig cfg;
    std::vector<std::string> warnings;
    std::string error;
    const bool applied =
        ApplyAudioConfigPatchJsonText(tc.json, &cfg, &warnings, &error);
    const std::string refused = std::string(tc.json) + " should be refused";
    ok = Expect(!applied, refused.c_str()) && ok;

    const std::string wanted = std::string(tc.json) + " should say \"" +
                               tc.want_error + "\", got: " + error;
    ok = Expect(error == tc.want_error, wanted.c_str()) && ok;
  }

  return ok;
}

// JSON has one number type, so a fractional value reaches the parser. The
// monitor fields count whole milliseconds and whole percent, so they must
// refuse it the way speaker_latency_ms does, instead of rounding it.
bool TestAudioConfigPatchRejectsFractionalMonitorNumbers() {
  struct Case {
    const char *json;
    const char *field;
  };
  const Case cases[] = {
      {"{\"monitor\":{\"latency_ms\":35.5}}", "monitor.latency_ms"},
      {"{\"monitor\":{\"volume\":60.5}}", "monitor.volume"},
      {"{\"speaker_latency_ms\":40.5}", "speaker_latency_ms"},
  };

  bool ok = true;
  for (const Case &tc : cases) {
    studiocast::audio::VirtualAudioServiceConfig cfg;
    std::vector<std::string> warnings;
    std::string error;
    const bool applied =
        ApplyAudioConfigPatchJsonText(tc.json, &cfg, &warnings, &error);
    const std::string refused =
        std::string(tc.field) + " should refuse a fractional number";
    ok = Expect(!applied, refused.c_str()) && ok;

    const std::string wanted = std::string(tc.field) +
                               " should say it wants an integer, got: " + error;
    ok = Expect(error == std::string(tc.field) + " must be an integer",
                wanted.c_str()) &&
         ok;
  }

  // A whole number written with a decimal point is still whole.
  studiocast::audio::VirtualAudioServiceConfig cfg;
  std::vector<std::string> warnings;
  std::string error;
  const bool applied = ApplyAudioConfigPatchJsonText(
      "{\"monitor\":{\"volume\":60.0}}", &cfg, &warnings, &error);
  const std::string whole = "a whole number should still apply: " + error;
  ok = Expect(applied, whole.c_str()) && ok;
  ok =
      Expect(cfg.monitor.volume == 60, "the whole number should be kept") && ok;

  return ok;
}

// Runs every pactl command through `hook` for the life of the object, so a
// check that reads the sound server never touches the host.
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

studiocast::util::ExecResult PactlResult(int exit_code,
                                         std::string stdout_str = {}) {
  studiocast::util::ExecResult result;
  result.exit_code = exit_code;
  result.stdout_str = std::move(stdout_str);
  return result;
}

// The monitor is a listening aid. When no safe output is left, refusing the
// whole audio config would also refuse microphone processing, the feature the
// user actually asked for. The monitor is turned off with a warning instead.
bool TestUnsatisfiableMonitorDoesNotRefuseAudio() {
  // One physical input and no output at all.
  ScopedPactlExecHook hook([](const std::string &command) {
    if (command == "pactl --version 2>&1")
      return PactlResult(0, "pactl 17.0\n");
    if (command == "pactl list short sources 2>&1") {
      return PactlResult(0, "2\tphysical_test_mic\tmodule-alsa-card.c\t"
                            "s16le 2ch 48000Hz\n");
    }
    if (command == "pactl get-default-source 2>&1")
      return PactlResult(0, "physical_test_mic\n");
    if (command == "pactl list short sinks 2>&1")
      return PactlResult(0, "");
    if (command == "pactl get-default-sink 2>&1")
      return PactlResult(1, "no default sink\n");
    return PactlResult(0, "");
  });

  studiocast::audio::VirtualAudioServiceConfig cfg;
  cfg.enabled = true;
  cfg.source_name = "physical_test_mic";
  cfg.monitor.enabled = true;
  cfg.monitor.sink = "auto";

  std::vector<std::string> warnings;
  std::string error;
  const bool safe = ValidateAudioConfigSafetyForDaemon(
      &cfg, "physical_test_mic", &warnings, &error);

  bool ok = Expect(safe, "microphone processing should still be allowed");
  if (!ok)
    std::cerr << "the refusal was: " << error << "\n";
  ok = Expect(!cfg.monitor.enabled,
              "the monitor that cannot be satisfied should be turned off") &&
       ok;

  bool named = false;
  for (const auto &warning : warnings) {
    if (warning.find("monitor") != std::string::npos)
      named = true;
  }
  ok = Expect(named, "a warning should say the monitor was turned off") && ok;
  return ok;
}

bool TestAudioConfigPatchRejectsUnsafeMonitorSink() {
  studiocast::audio::VirtualAudioServiceConfig cfg;
  std::vector<std::string> warnings;
  std::string error;
  if (!ApplyAudioConfigPatchJsonText(
          "{\"monitor\":{\"enabled\":true,\"sink\":\"studiocast_sink\"}}", &cfg,
          &warnings, &error)) {
    std::cerr << "the patch itself should parse: " << error << "\n";
    return false;
  }

  error.clear();
  const bool safe = ValidateAudioConfigSafetyForDaemon(
      &cfg, /*resolvedSource=*/"", &warnings, &error);
  return Expect(!safe, "a StudioCast sink should be refused for the monitor") &&
         Expect(!error.empty(), "the refusal should carry a reason");
}

// On "auto" the configured source names no input, so the feedback check must
// use the source the service resolved instead.
bool TestAudioConfigSafetyUsesTheResolvedMicSource() {
  // The service stays disabled, so the check does not need a sound server to
  // resolve "auto" into a real input name.
  studiocast::audio::VirtualAudioServiceConfig cfg;
  cfg.source_name = "auto";
  cfg.monitor.enabled = true;
  cfg.monitor.sink = "alsa_output.usb_headset";

  std::vector<std::string> warnings;
  std::string error;
  const bool safe = ValidateAudioConfigSafetyForDaemon(
      &cfg, "alsa_output.usb_headset.monitor", &warnings, &error);

  return Expect(!safe,
                "a sink the resolved microphone monitors should be refused") &&
         Expect(error.find("feedback") != std::string::npos,
                "the refusal should name the feedback loop");
}

// The status keeps a plain note apart from an error, so a reader can tell an
// ordinary idle monitor from one that needs attention.
bool TestAudioStatusKeepsTheMonitorNoteApartFromTheError() {
  studiocast::video::VirtualCameraServiceStatus videoStatus;
  studiocast::video::VirtualCameraServiceConfig videoConfig;

  studiocast::audio::VirtualAudioServiceStatus audioStatus;
  audioStatus.mic_present = true;
  audioStatus.monitor_note =
      "Microphone processing is off, so the monitor stays idle.";

  studiocast::audio::VirtualAudioServiceConfig audioConfig;
  audioConfig.monitor.enabled = true;

  studiocast::util::json::Value rootValue;
  std::string error;
  if (!studiocast::util::json::Parse(
          StatusToJson(videoStatus, videoConfig, audioStatus, audioConfig,
                       std::filesystem::path("/tmp/studiocastd-test.sock"),
                       /*maxineJson=*/"", /*openCudaJson=*/"",
                       /*openAudioJson=*/"", /*loopbackJson=*/""),
          &rootValue, &error)) {
    std::cerr << "status JSON should parse: " << error << "\n";
    return false;
  }

  const JsonObject *root = rootValue.AsObject();
  if (!root)
    return false;
  const JsonObject *audio = ObjectAt(*root, "audio", "audio object missing");
  if (!audio)
    return false;
  const JsonObject *monitor =
      ObjectAt(*audio, "monitor", "monitor object missing");
  if (!monitor)
    return false;

  const std::string *note = StringAt(*monitor, "note", "monitor note missing");
  const std::string *lastError =
      StringAt(*monitor, "last_error", "monitor last_error missing");
  if (!note || !lastError)
    return false;

  return Expect(note->find("stays idle") != std::string::npos,
                "the note should carry the daemon sentence") &&
         Expect(lastError->empty(), "an ordinary note is not an error");
}

bool TestAudioStatusReportsMonitor() {
  studiocast::video::VirtualCameraServiceStatus videoStatus;
  studiocast::video::VirtualCameraServiceConfig videoConfig;

  studiocast::audio::VirtualAudioServiceStatus audioStatus;
  audioStatus.mic_present = true;
  audioStatus.monitor_active = true;
  audioStatus.monitor_module_id = 551;
  audioStatus.monitor_sink_active = "alsa_output.usb_headset";
  audioStatus.monitor_latency_ms_active = 20;
  audioStatus.monitor_volume_active = 100;
  audioStatus.mic_consumer_count = 2;
  audioStatus.mic_app_consumer_count = 1;
  audioStatus.mic_monitor_consumer_count = 1;

  studiocast::audio::VirtualAudioServiceConfig audioConfig;
  audioConfig.monitor.enabled = true;
  audioConfig.monitor.sink = "auto";
  audioConfig.monitor.latency_ms = 20;

  studiocast::util::json::Value rootValue;
  std::string error;
  if (!studiocast::util::json::Parse(
          StatusToJson(videoStatus, videoConfig, audioStatus, audioConfig,
                       std::filesystem::path("/tmp/studiocastd-test.sock"),
                       /*maxineJson=*/"", /*openCudaJson=*/"",
                       /*openAudioJson=*/"", /*loopbackJson=*/""),
          &rootValue, &error)) {
    std::cerr << "status JSON should parse: " << error << "\n";
    return false;
  }

  const JsonObject *root = rootValue.AsObject();
  if (!root)
    return false;
  const JsonObject *audio = ObjectAt(*root, "audio", "audio should exist");
  if (!audio)
    return false;
  const JsonObject *monitor =
      ObjectAt(*audio, "monitor", "audio.monitor should exist");
  if (!monitor)
    return false;

  const std::string *sink =
      StringAt(*monitor, "sink", "monitor sink should exist");
  const std::string *resolved =
      StringAt(*monitor, "sink_resolved", "monitor sink_resolved should exist");
  if (!sink || !resolved)
    return false;

  return Expect(JsonBoolField(*monitor, "enabled", false),
                "monitor should report enabled") &&
         Expect(JsonBoolField(*monitor, "active", false),
                "monitor should report active") &&
         Expect(*sink == "auto", "monitor should echo the requested sink") &&
         Expect(*resolved == "alsa_output.usb_headset",
                "monitor should report the resolved sink") &&
         Expect(JsonBoolField(*audio, "mic_consumer_present", false) == false,
                "consumer presence should stay untouched") &&
         Expect(NumberFieldOr(*audio, "mic_app_consumer_count", -1) == 1,
                "status should report the app consumer count") &&
         Expect(NumberFieldOr(*audio, "mic_monitor_consumer_count", -1) == 1,
                "status should report the monitor consumer count");
}

// The same feedback check runs in the status. With the source on "auto" the
// configured name is empty, so the check must read the resolved name.
bool TestAudioStatusFlagsMonitorFeedbackOnTheResolvedSource() {
  studiocast::video::VirtualCameraServiceStatus videoStatus;
  studiocast::video::VirtualCameraServiceConfig videoConfig;

  studiocast::audio::VirtualAudioServiceStatus audioStatus;
  audioStatus.selected_source = "alsa_output.usb_headset.monitor";
  audioStatus.monitor_active = true;
  audioStatus.monitor_sink_active = "alsa_output.usb_headset";

  studiocast::audio::VirtualAudioServiceConfig audioConfig;
  audioConfig.enabled = true;
  audioConfig.source_name = "auto";
  audioConfig.monitor.enabled = true;
  audioConfig.monitor.sink = "alsa_output.usb_headset";

  studiocast::util::json::Value rootValue;
  std::string error;
  if (!studiocast::util::json::Parse(
          StatusToJson(videoStatus, videoConfig, audioStatus, audioConfig,
                       std::filesystem::path("/tmp/studiocastd-test.sock"),
                       /*maxineJson=*/"", /*openCudaJson=*/"",
                       /*openAudioJson=*/"", /*loopbackJson=*/""),
          &rootValue, &error)) {
    std::cerr << "status JSON should parse: " << error << "\n";
    return false;
  }

  const JsonObject *root = rootValue.AsObject();
  if (!root)
    return false;
  const JsonObject *audio = ObjectAt(*root, "audio", "audio should exist");
  if (!audio)
    return false;
  const JsonObject *monitor =
      ObjectAt(*audio, "monitor", "audio.monitor should exist");
  if (!monitor)
    return false;
  const std::string *sinkError =
      StringAt(*monitor, "sink_error", "monitor sink_error should exist");
  if (!sinkError)
    return false;

  return Expect(!sinkError->empty(),
                "a sink the resolved microphone monitors should be flagged") &&
         Expect(sinkError->find("feedback") != std::string::npos,
                "the sink error should name the feedback loop");
}

bool TestMicrophoneReadinessNamesMonitorOnlyListener() {
  studiocast::audio::VirtualAudioServiceStatus ast;
  ast.mic_present = true;
  ast.pipeline_running = true;
  ast.pipeline_state = "running";
  ast.monitor_active = true;
  ast.mic_consumer_count = 1;
  ast.mic_app_consumer_count = 0;
  ast.mic_monitor_consumer_count = 1;

  studiocast::audio::VirtualAudioServiceConfig acfg;
  acfg.enabled = true;
  acfg.monitor.enabled = true;

  const auto readiness = BuildMicrophoneEndpointReadiness(ast, acfg, "");
  return Expect(readiness.state == "processing",
                "the pipeline runs while only the monitor listens") &&
         Expect(readiness.detail.find("monitor") != std::string::npos,
                "readiness detail should name the monitor as the listener") &&
         Expect(readiness.detail.find("Waiting for an app") !=
                    std::string::npos,
                "readiness detail should still wait for an app");
}

} // namespace

int main() {
  bool ok = true;
  ok = TestVideoStatusReportsAllowCpuResize() && ok;
  ok = TestVideoStatusReportsRequestedOutputFormat() && ok;
  ok = TestVideoStatusReportsCaptureFallbackState() && ok;
  ok = TestVideoStatusReportsConfiguredDevicesWhenPipelineIdle() && ok;
  ok = TestExplicitOpenCudaEffectUnknownWithoutDiagnostics() && ok;
  ok = TestBuiltinEffectReadyWithoutDiagnostics() && ok;
  ok = TestAudioStatusReportsResolvedSourceAndWarnings() && ok;
  ok = TestAudioStatusPropagatesSourceErrorFromService() && ok;
  ok = TestAudioConfigJsonRoundTripsMonitor() && ok;
  ok = TestAudioConfigPatchRejectsFractionalMonitorNumbers() && ok;
  ok = TestAudioConfigPatchRejectsExtremeMonitorNumbers() && ok;
  ok = TestAudioConfigPatchRejectsUnsafeMonitorSink() && ok;
  ok = TestUnsatisfiableMonitorDoesNotRefuseAudio() && ok;
  ok = TestAudioStatusReportsMonitor() && ok;
  ok = TestAudioStatusKeepsTheMonitorNoteApartFromTheError() && ok;
  ok = TestAudioConfigSafetyUsesTheResolvedMicSource() && ok;
  ok = TestAudioStatusFlagsMonitorFeedbackOnTheResolvedSource() && ok;
  ok = TestMicrophoneReadinessNamesMonitorOnlyListener() && ok;
  return ok ? 0 : 1;
}
