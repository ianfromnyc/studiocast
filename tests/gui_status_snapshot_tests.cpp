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
        "version":"0.2.0",
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
         Expect(s.version == QStringLiteral("0.2.0"),
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

bool TestEngineModelDetailsAndConfiguredSelections() {
  const QString json = QStringLiteral(
      R"({
        "service_running":true,
        "engines":{
          "maxine":{
            "supported":false,
            "ok":false,
            "summary":"Maxine VideoFX SDK not found.",
            "blocked_reason":"sdk_missing",
            "blocked_details":["Install Maxine VideoFX and AudioFX."],
            "hints":["Run studiocast-maxine install-hints"],
            "components":{
              "vfx":{
                "feature_status":{
                  "BackgroundBlur":{"installed":false,"details":"Missing feature file."}
                }
              }
            }
          },
          "open_cuda":{
            "ok":true,
            "onnxruntime_version":"1.20.0",
            "onnxruntime_providers":["CUDAExecutionProvider","CPUExecutionProvider"],
            "onnxruntime_cuda_provider_present":true,
            "onnxruntime_tensorrt_provider_present":false,
            "onnxruntime_cpu_provider_present":true,
            "onnxruntime_cuda_ep_v2_build":true,
            "onnxruntime_library_path":"/opt/ort/lib/libonnxruntime.so",
            "cuda_driver_api_available":true,
            "cuda_context_available":true,
            "cuda_device_count":1,
            "cuda_driver_version":12040,
            "installed_models":["matting-good"],
            "default_model_id":"matting-good",
            "models":[
              {"id":"matting-good","display_name":"Good Matting","task":"matting","width":256,"height":256}
            ],
            "missing_models":{"configured-missing":"model.json is missing"},
            "available_effects":["video_noise_removal"],
            "blocked_effects":{"auto_frame":"missing_model_packs"},
            "install_hints":["Open Video hint"]
          },
          "open_audio":{
            "ok":true,
            "onnxruntime_version":"1.20.0",
            "onnxruntime_providers":["CPUExecutionProvider"],
            "onnxruntime_cuda_provider_present":false,
            "onnxruntime_tensorrt_provider_present":false,
            "onnxruntime_cpu_provider_present":true,
            "onnxruntime_cuda_ep_v2_build":false,
            "onnxruntime_library_path":"/opt/ort/lib/libonnxruntime.so",
            "acceleration_likely":"cpu_fallback",
            "installed_models":["fast-enhancer"],
            "models":[
              {"id":"fast-enhancer","display_name":"Fast Enhancer","effects":["noise_removal"],"sample_rate":48000,"channels":1}
            ],
            "missing_models":{"gone":"No such model pack"},
            "install_hints":["Open Audio hint"]
          }
        },
        "video":{
          "enabled":true,
          "virtual_device_present":true,
          "virtual_device_available":true,
          "consumer_count":1,
          "video_effects":{
            "engine":"open_cuda",
            "virtual_background":{"model_id":"matting-good"},
            "auto_frame":{"model_id":"configured-missing"},
            "eye_contact":{"model_id":"not-reported"}
          },
          "pipeline":{
            "running":true,
            "starting":false,
            "effects_backends":"virtual_background.blur:open_cuda,mirror:passthrough"
          }
        },
        "audio":{
          "mic_present":true,
          "source_error":"",
          "audio_effects":{
            "engine":"open_source",
            "microphone":{"model_id":"fast-enhancer","model_path":""},
            "speaker":{"model_id":"gone","model_path":"/tmp/explicit.onnx"}
          },
          "pipeline":{"running":true,"starting":false,"last_error":"","backend_active":"open_audio"},
          "speakers":{
            "present":true,
            "target_sink_error":"",
            "routing_active":true,
            "route_mode":"pipeline",
            "backend_active":"open_audio",
            "last_error":"",
            "pipeline_last_error":""
          }
        }
      })");

  const auto s = studiocast::gui::DaemonStatusSnapshot::FromJson(json);
  return Expect(s.parsed, "engine model details payload should parse") &&
         Expect(s.camera.state == studiocast::gui::ReadinessState::Processing,
                "running camera should report processing") &&
         Expect(s.microphone.state ==
                    studiocast::gui::ReadinessState::Processing,
                "running microphone should report processing") &&
         Expect(s.speakers.state == studiocast::gui::ReadinessState::Processing,
                "active speaker routing should report processing") &&
         Expect(s.videoEffectsActiveBackends.contains(QStringLiteral("open_cuda")),
                "video active backends should parse") &&
         Expect(s.microphoneActiveBackend == QStringLiteral("open_audio"),
                "microphone active backend should parse") &&
         Expect(s.speakersActiveBackend == QStringLiteral("open_audio"),
                "speaker active backend should parse") &&
         Expect(s.maxine.installHints.contains(
                    QStringLiteral("Run studiocast-maxine install-hints")),
                "maxine hints should be preserved") &&
         Expect(s.maxine.missingModelCount == 1,
                "maxine missing feature state should parse") &&
         Expect(!s.openCuda.rawJson.isEmpty(),
                "raw open_cuda diagnostics should be preserved") &&
         Expect(s.openCuda.rawJson.contains(
                    QStringLiteral("onnxruntime_cuda_provider_present")),
                "raw open_cuda diagnostics should preserve runtime fields") &&
         Expect(s.openCuda.rawJson.contains(
                    QStringLiteral("cuda_driver_version")),
                "raw open_cuda diagnostics should preserve cuda fields") &&
         Expect(s.openCuda.installedModels.size() == 1 &&
                    s.openCuda.installedModels.front().displayName ==
                        QStringLiteral("Good Matting"),
                "open video model display names should parse") &&
         Expect(s.openCuda.configuredModels.size() == 3,
                "configured open video model IDs should parse") &&
         Expect(s.openCuda.configuredMissingModelCount == 2,
                "configured missing open video IDs should be counted") &&
         Expect(s.openAudio.installedModels.size() == 1 &&
                    s.openAudio.installedModels.front().displayName ==
                        QStringLiteral("Fast Enhancer"),
                "open audio model display names should parse") &&
         Expect(s.openAudio.rawJson.contains(
                    QStringLiteral("acceleration_likely")),
                "raw open_audio diagnostics should preserve runtime fields") &&
         Expect(s.openAudio.rawJson.contains(
                    QStringLiteral("onnxruntime_cpu_provider_present")),
                "raw open_audio diagnostics should preserve provider fields") &&
         Expect(s.openAudio.configuredModels.size() == 2,
                "configured open audio model IDs should parse") &&
         Expect(s.openAudio.configuredMissingModelCount == 1,
                "configured missing open audio IDs should be counted") &&
         Expect(s.openAudio.configuredModels.back().modelPath ==
                    QStringLiteral("/tmp/explicit.onnx"),
                "explicit model paths should be preserved in details");
}

bool TestOpenAudioRuntimeStatusFields() {
  const QString json = QStringLiteral(
      R"({
        "service_running":true,
        "audio":{
          "enabled":true,
          "mic_present":true,
          "source_error":"",
          "audio_effects":{"engine":"open_source"},
          "pipeline":{
            "running":true,
            "starting":false,
            "state":"running",
            "backend_active":"open_source",
            "last_error":"",
            "open_audio_runtime":{
              "active":true,
              "active_provider":"cpu",
              "using_cpu_fallback":true,
              "disabled":false,
              "selected_model_id":"fast-enhancer",
              "selected_model_path":"/models/open_audio/fast/model.onnx",
              "last_runtime_warning":"Open Audio: switched to CPU fallback after a CUDA runtime failure."
            }
          },
          "speakers":{
            "enabled":true,
            "present":true,
            "target_sink_error":"",
            "routing_active":false,
            "route_mode":"pipeline",
            "backend_active":"open_source",
            "last_error":"",
            "pipeline_last_error":"",
            "open_audio_runtime":{
              "active":false,
              "active_provider":"disabled",
              "using_cpu_fallback":true,
              "disabled":true,
              "selected_model_id":"speaker-enhancer",
              "selected_model_path":"/models/open_audio/speaker/model.onnx",
              "last_runtime_warning":"Open Audio: disabled after repeated runtime failures."
            }
          }
        }
      })");

  const auto s = studiocast::gui::DaemonStatusSnapshot::FromJson(json);
  return Expect(s.parsed, "open audio runtime payload should parse") &&
         Expect(s.microphoneOpenAudioRuntime.present,
                "microphone runtime status should be present") &&
         Expect(s.microphoneOpenAudioRuntime.active,
                "microphone runtime active flag should parse") &&
         Expect(s.microphoneOpenAudioRuntime.activeProvider ==
                    QStringLiteral("cpu"),
                "microphone runtime provider should parse") &&
         Expect(s.microphoneOpenAudioRuntime.usingCpuFallback,
                "microphone CPU fallback should parse") &&
         Expect(!s.microphoneOpenAudioRuntime.disabled,
                "microphone disabled flag should parse") &&
         Expect(s.microphoneOpenAudioRuntime.selectedModelId ==
                    QStringLiteral("fast-enhancer"),
                "microphone runtime model id should parse") &&
         Expect(s.microphoneOpenAudioRuntime.selectedModelPath ==
                    QStringLiteral("/models/open_audio/fast/model.onnx"),
                "microphone runtime model path should parse") &&
         Expect(s.microphoneOpenAudioRuntime.lastRuntimeWarning.contains(
                    QStringLiteral("CPU fallback")),
                "microphone runtime warning should parse") &&
         Expect(s.speakersOpenAudioRuntime.present,
                "speaker runtime status should be present") &&
         Expect(!s.speakersOpenAudioRuntime.active,
                "speaker runtime active flag should parse") &&
         Expect(s.speakersOpenAudioRuntime.disabled,
                "speaker runtime disabled flag should parse") &&
         Expect(s.speakersOpenAudioRuntime.activeProvider ==
                    QStringLiteral("disabled"),
                "speaker runtime provider should parse") &&
         Expect(s.speakersOpenAudioRuntime.usingCpuFallback,
                "speaker CPU fallback should parse") &&
         Expect(s.speakersOpenAudioRuntime.selectedModelId ==
                    QStringLiteral("speaker-enhancer"),
                "speaker runtime model id should parse") &&
         Expect(s.speakersOpenAudioRuntime.selectedModelPath ==
                    QStringLiteral("/models/open_audio/speaker/model.onnx"),
                "speaker runtime model path should parse") &&
         Expect(s.speakersOpenAudioRuntime.lastRuntimeWarning.contains(
                    QStringLiteral("repeated runtime failures")),
                "speaker runtime warning should parse");
}

bool TestMissingVirtualDevices() {
  const QString json = QStringLiteral(
      R"({
        "service_running":true,
        "video":{
          "enabled":true,
          "virtual_device_present":false,
          "virtual_device_available":false,
          "virtual_device_error":"v4l2loopback device is missing.",
          "consumer_count":0,
          "pipeline":{"running":false,"starting":false,"state":"device_unavailable"}
        },
        "audio":{
          "enabled":true,
          "mic_present":false,
          "source_error":"",
          "pipeline":{"running":false,"starting":false,"state":"disabled","last_error":""},
          "speakers":{
            "enabled":true,
            "present":false,
            "target_sink_error":"",
            "routing_active":false,
            "route_mode":"off",
            "last_error":"",
            "pipeline_last_error":""
          }
        }
      })");

  const auto s = studiocast::gui::DaemonStatusSnapshot::FromJson(json);
  return Expect(s.parsed, "missing virtual device payload should parse") &&
         Expect(s.camera.state ==
                    studiocast::gui::ReadinessState::MissingVirtualDevice,
                "camera should report missing virtual camera") &&
         Expect(s.microphone.state ==
                    studiocast::gui::ReadinessState::MissingVirtualDevice,
                "microphone should report missing virtual microphone") &&
         Expect(s.speakers.state ==
                    studiocast::gui::ReadinessState::MissingVirtualDevice,
                "speakers should report missing virtual speakers");
}

bool TestMissingPhysicalDevices() {
  const QString json = QStringLiteral(
      R"({
        "service_running":true,
        "video":{
          "enabled":true,
          "virtual_device_present":true,
          "virtual_device_available":true,
          "consumer_count":1,
          "last_error":"Pipeline start failed: No readable camera device found (no non-loopback /dev/video* with read access).",
          "pipeline":{"running":false,"starting":false,"state":"backing_off"}
        },
        "audio":{
          "enabled":true,
          "mic_present":true,
          "source_error":"Select a physical microphone/input source before enabling audio.",
          "pipeline":{"running":false,"starting":false,"state":"idle_no_consumer","last_error":""},
          "speakers":{
            "enabled":true,
            "present":true,
            "target_sink_error":"Failed to choose a physical speaker target sink. Choose a physical output sink.",
            "routing_active":false,
            "route_mode":"pipeline",
            "pipeline_running":false,
            "pipeline_starting":false,
            "pipeline_state":"idle_no_consumer",
            "last_error":"",
            "pipeline_last_error":""
          }
        }
      })");

  const auto s = studiocast::gui::DaemonStatusSnapshot::FromJson(json);
  return Expect(s.parsed, "missing physical device payload should parse") &&
         Expect(s.camera.state == studiocast::gui::ReadinessState::NoPhysicalDevice,
                "camera should report physical input problem when supported") &&
         Expect(s.microphone.state ==
                    studiocast::gui::ReadinessState::NoPhysicalDevice,
                "microphone should report physical source problem") &&
         Expect(s.speakers.state ==
                    studiocast::gui::ReadinessState::NoPhysicalDevice,
                "speakers should report physical target problem");
}

bool TestIdleNoConsumerStates() {
  const QString json = QStringLiteral(
      R"({
        "service_running":true,
        "video":{
          "enabled":true,
          "virtual_device_present":true,
          "virtual_device_available":true,
          "consumer_present":false,
          "consumer_count":0,
          "pipeline":{
            "running":false,
            "starting":false,
            "state":"idle_no_consumer",
            "idle_reason":"no_consumer"
          }
        },
        "audio":{
          "enabled":true,
          "mic_present":true,
          "source_error":"",
          "mic_consumer_present":false,
          "mic_consumer_count":0,
          "mic_consumer_error":"",
          "pipeline":{
            "running":false,
            "starting":false,
            "state":"idle_no_consumer",
            "idle_reason":"No active virtual microphone consumer.",
            "last_error":""
          },
          "speakers":{
            "enabled":true,
            "present":true,
            "target_sink_error":"",
            "consumer_present":false,
            "consumer_count":0,
            "consumer_error":"",
            "routing_active":false,
            "route_mode":"pipeline",
            "pipeline_running":false,
            "pipeline_starting":false,
            "pipeline_active_needed":false,
            "pipeline_state":"idle_no_consumer",
            "pipeline_idle_reason":"No active virtual speakers consumer.",
            "last_error":"",
            "pipeline_last_error":""
          }
        }
      })");

  const auto s = studiocast::gui::DaemonStatusSnapshot::FromJson(json);
  return Expect(s.parsed, "idle no-consumer payload should parse") &&
         Expect(s.camera.state == studiocast::gui::ReadinessState::Idle,
                "camera should report idle no-consumer") &&
         Expect(s.microphone.state == studiocast::gui::ReadinessState::Idle,
                "microphone should report idle no-consumer") &&
         Expect(s.speakers.state == studiocast::gui::ReadinessState::Idle,
                "speakers should report idle no-consumer");
}

bool TestConsumerDetectionErrorsAreNotIdle() {
  const QString json = QStringLiteral(
      R"({
        "service_running":true,
        "video":{
          "enabled":true,
          "virtual_device_present":true,
          "virtual_device_available":true,
          "consumer_error":"Failed to scan consumers.",
          "consumer_count":0,
          "pipeline":{"running":false,"starting":false,"state":"consumer_detection_error"}
        },
        "audio":{
          "enabled":true,
          "mic_present":true,
          "source_error":"",
          "mic_consumer_error":"PulseAudio consumer detection unavailable.",
          "pipeline":{"running":false,"starting":false,"state":"idle_no_consumer","last_error":""},
          "speakers":{
            "enabled":true,
            "present":true,
            "target_sink_error":"",
            "consumer_error":"PulseAudio consumer detection unavailable.",
            "routing_active":false,
            "route_mode":"pipeline",
            "pipeline_running":false,
            "pipeline_starting":false,
            "pipeline_state":"idle_no_consumer",
            "last_error":"",
            "pipeline_last_error":""
          }
        }
      })");

  const auto s = studiocast::gui::DaemonStatusSnapshot::FromJson(json);
  return Expect(s.parsed, "consumer detection error payload should parse") &&
         Expect(s.camera.state ==
                    studiocast::gui::ReadinessState::RecoverableError,
                "camera consumer detection error should not report idle") &&
         Expect(s.microphone.state ==
                    studiocast::gui::ReadinessState::RecoverableError,
                "microphone consumer detection error should not report idle") &&
         Expect(s.speakers.state ==
                    studiocast::gui::ReadinessState::RecoverableError,
                "speakers consumer detection error should not report idle");
}

bool TestInvalidJsonPreservesRawPayload() {
  const QString json = QStringLiteral("{not-json");
  const auto s = studiocast::gui::DaemonStatusSnapshot::FromJson(json);
  return Expect(s.reachable, "parse errors still came from a reachable daemon") &&
         Expect(!s.parsed, "invalid json should not parse") &&
         Expect(s.rawJson == json, "invalid raw json should be preserved") &&
         Expect(!s.parseError.isEmpty(), "parse error should be reported") &&
         Expect(s.RawDiagnosticsText() == json,
                "raw diagnostics should preserve invalid raw payloads");
}

bool TestRawDiagnosticsFallbacks() {
  const auto unreachable = studiocast::gui::DaemonStatusSnapshot::Unreachable(
      QStringLiteral("connect failed"));
  studiocast::gui::DaemonStatusSnapshot empty;
  return Expect(unreachable.RawDiagnosticsText() ==
                    QStringLiteral("Daemon unavailable: connect failed"),
                "unreachable raw diagnostics should include transport error") &&
         Expect(empty.RawDiagnosticsText() ==
                    QStringLiteral("Daemon status has not been read."),
                "empty raw diagnostics should explain status is unread");
}

} // namespace

int main() {
  bool ok = true;
  ok = TestUnreachableStatus() && ok;
  ok = TestStatusJsonCompatibilityShapes() && ok;
  ok = TestEngineModelDetailsAndConfiguredSelections() && ok;
  ok = TestOpenAudioRuntimeStatusFields() && ok;
  ok = TestMissingVirtualDevices() && ok;
  ok = TestMissingPhysicalDevices() && ok;
  ok = TestIdleNoConsumerStates() && ok;
  ok = TestConsumerDetectionErrorsAreNotIdle() && ok;
  ok = TestInvalidJsonPreservesRawPayload() && ok;
  ok = TestRawDiagnosticsFallbacks() && ok;
  return ok ? 0 : 1;
}
