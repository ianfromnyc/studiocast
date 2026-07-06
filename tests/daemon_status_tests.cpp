#include <filesystem>
#include <iostream>
#include <string>

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

} // namespace

int main() {
  bool ok = true;
  ok = TestVideoStatusReportsAllowCpuResize() && ok;
  ok = TestExplicitOpenCudaEffectUnknownWithoutDiagnostics() && ok;
  ok = TestBuiltinEffectReadyWithoutDiagnostics() && ok;
  return ok ? 0 : 1;
}
