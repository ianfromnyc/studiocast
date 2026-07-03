#include <iostream>

#include "gui/status/daemon_status_snapshot.h"

namespace {

bool Expect(bool condition, const char *message) {
  if (!condition) {
    std::cerr << message << "\n";
    return false;
  }
  return true;
}

bool TestUnreachableStatus() {
  const auto s = studiocast::gui::DaemonStatusSnapshot::Unreachable(
      QStringLiteral("connect failed"));
  return Expect(!s.reachable, "unreachable snapshot should not be reachable") &&
         Expect(s.camera.state ==
                    studiocast::gui::ReadinessState::DaemonUnavailable,
                "camera should report daemon unavailable") &&
         Expect(s.ServiceSummary() == QStringLiteral("Service unavailable"),
                "service summary should report unavailable");
}

bool TestStatusJsonCompatibilityShapes() {
  const QString json = QStringLiteral(
      R"({
        "version":"0.1.0",
        "git_sha":"abc123",
        "socket":"/run/user/1000/studiocast/studiocastd.sock",
        "service_running":true,
        "maxine":{
          "supported":false,
          "ok":false,
          "summary":"Maxine unavailable.",
          "blocked_reason":"driver_missing",
          "blocked_details":["Install a supported NVIDIA driver."]
        },
        "engines":{
          "open_cuda":{
            "ok":true,
            "installed_models":["matting"],
            "models":[{"id":"matting","display_name":"Matting"}],
            "missing_models":{"denoise":"Missing denoise model."},
            "install_hints":["studiocast-open install-hints"]
          }
        },
        "open_audio":{
          "ok":true,
          "installed_models":["rnnoise"],
          "models":[{"id":"rnnoise","display_name":"RNNoise"}],
          "missing_models":{}
        },
        "video":{
          "enabled":false,
          "virtual_device_present":true,
          "virtual_device_available":true,
          "consumer_count":0,
          "video_effects":{"engine":"open_cuda"},
          "effects_plan":{
            "disabled":[
              {"id":"mirror","reason":"Disabled: mirror is not supported."}
            ]
          },
          "pipeline":{"running":false,"starting":false}
        },
        "audio":{
          "mic_present":true,
          "source_error":"",
          "audio_effects":{"engine":"open_source"},
          "pipeline":{"running":false,"starting":false,"last_error":""},
          "speakers":{
            "present":true,
            "target_sink_error":"",
            "routing_active":false,
            "route_mode":"off",
            "last_error":"",
            "pipeline_last_error":""
          }
        }
      })");

  const auto s = studiocast::gui::DaemonStatusSnapshot::FromJson(json);
  return Expect(s.reachable, "snapshot should be reachable") &&
         Expect(s.parsed, "snapshot should parse") &&
         Expect(s.rawJson == json, "raw json should be preserved") &&
         Expect(s.version == QStringLiteral("0.1.0"),
                "version should parse") &&
         Expect(s.serviceRunning, "service_running should parse") &&
         Expect(s.camera.state == studiocast::gui::ReadinessState::Ready,
                "camera should be ready") &&
         Expect(s.camera.disabledReasons.size() == 1,
                "camera disabled effect reasons should parse") &&
         Expect(s.microphone.state == studiocast::gui::ReadinessState::Ready,
                "microphone should be ready") &&
         Expect(s.speakers.state == studiocast::gui::ReadinessState::Ready,
                "speakers should be ready") &&
         Expect(s.videoEffectsEnginePreference == QStringLiteral("open_cuda"),
                "video engine preference should parse") &&
         Expect(s.audioEffectsEnginePreference == QStringLiteral("open_source"),
                "audio engine preference should parse") &&
         Expect(s.maxine.present && !s.maxine.supported,
                "top-level maxine diagnostics should parse") &&
         Expect(s.openCuda.present && s.openCuda.ok &&
                    s.openCuda.missingModelCount == 1,
                "nested open_cuda diagnostics should parse") &&
         Expect(s.openCuda.missingModels.contains(
                    QStringLiteral("denoise: Missing denoise model.")),
                "open_cuda missing model details should parse") &&
         Expect(s.openCuda.installHints.contains(
                    QStringLiteral("studiocast-open install-hints")),
                "open_cuda install hints should parse") &&
         Expect(s.openAudio.present && s.openAudio.ok &&
                    s.openAudio.installedModelCount == 1,
                "top-level open_audio diagnostics should parse");
}

bool TestInvalidJsonPreservesRawPayload() {
  const QString json = QStringLiteral("{not-json");
  const auto s = studiocast::gui::DaemonStatusSnapshot::FromJson(json);
  return Expect(s.reachable, "parse errors still came from a reachable daemon") &&
         Expect(!s.parsed, "invalid json should not parse") &&
         Expect(s.rawJson == json, "invalid raw json should be preserved") &&
         Expect(!s.parseError.isEmpty(), "parse error should be reported");
}

} // namespace

int main() {
  bool ok = true;
  ok = TestUnreachableStatus() && ok;
  ok = TestStatusJsonCompatibilityShapes() && ok;
  ok = TestInvalidJsonPreservesRawPayload() && ok;
  return ok ? 0 : 1;
}
