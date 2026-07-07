#include "core/open_audio/open_audio_diagnose.h"

#include "core/audio/effects/broadcast_audio_effect_contract.h"
#include "core/open_audio/model_pack_registry.h"
#include "core/open_audio/open_audio_onnx_session.h"
#include "core/util/xdg.h"

namespace studiocast::open_audio {

OpenAudioDiagnostics DiagnoseOpenAudioDefault() {
  OpenAudioDiagnostics od;

  const auto reg = ModelPackRegistry::ScanDefault();
  od.default_model_id = reg.DefaultModelId();

  for (const auto &m : reg.ListModels()) {
    od.installed_models.push_back(m.id);
    OpenAudioDiagnostics::ModelInfo mi;
    mi.id = m.id;
    mi.display_name = m.display_name;
    mi.effects = m.effects;
    mi.sample_rate = m.sample_rate;
    mi.channels = m.channels;
    od.models.push_back(std::move(mi));
  }
  od.missing_models = reg.Problems();

  const auto modelsRoot = studiocast::util::StudioCastModelsDir();
  const auto openAudioRoot =
      modelsRoot.empty()
          ? std::string("~/.local/share/studiocast/models/open_audio")
          : (modelsRoot / "open_audio").string();

  od.install_hints.push_back(std::string("Model packs: ") + openAudioRoot +
                             "/<model_id>/");
  od.install_hints.push_back(
      "Source builds: run ./scripts/install.sh open-audio-models to install "
      "curated FastEnhancer packs.");
  od.install_hints.push_back("Docs: docs/open_source_audio_models_install.md");
  od.install_hints.push_back(
      "Each pack must contain: model.json, the ONNX file referenced by "
      "onnx_filename, and LICENSE.txt.");

  {
    const auto ort = OpenAudioOrtSession::QueryRuntimeInfo();
    od.onnxruntime_version = ort.version;
    od.onnxruntime_providers = ort.providers;
    od.onnxruntime_cuda_provider_present = ort.cuda_provider_present;
    od.onnxruntime_tensorrt_provider_present = ort.tensorrt_provider_present;
    od.onnxruntime_cpu_provider_present = ort.cpu_provider_present;
    od.onnxruntime_cuda_ep_v2_build = ort.cuda_ep_v2_build;
    od.onnxruntime_library_path = ort.library_path;
    od.onnxruntime_warnings = ort.warnings;
#if STUDIOCAST_HAVE_ONNXRUNTIME
    if (ort.cuda_provider_present) {
      od.acceleration_likely = "cuda";
    } else if (ort.cpu_provider_present) {
      od.acceleration_likely = "cpu_fallback";
    } else {
      od.acceleration_likely = "unknown";
    }
#else
    od.acceleration_likely = "unavailable";
#endif
  }

  const auto block_effects = [&](const char *reason_code) {
    od.blocked_effects[std::string(
        studiocast::audio::effects::contract::kEffectIdNoiseRemoval)] =
        reason_code;
    od.blocked_effects[std::string(
        studiocast::audio::effects::contract::kEffectIdRoomEchoRemoval)] =
        reason_code;
    od.blocked_effects[std::string(
        studiocast::audio::effects::contract::kEffectIdStudioVoice)] =
        reason_code;
  };

#if !STUDIOCAST_ENABLE_OPEN_AUDIO
  od.ok = false;
  block_effects("disabled_in_build");
  od.install_hints.push_back("Open Audio backend is disabled in this build.");
  od.install_hints.push_back("Rebuild with -DSTUDIOCAST_ENABLE_OPEN_AUDIO=ON "
                             "(requires ONNX Runtime).");
#elif STUDIOCAST_HAVE_ONNXRUNTIME
  if (od.installed_models.empty()) {
    od.ok = false;
    block_effects("missing_model_packs");
    od.install_hints.push_back("No usable Open Audio model packs were found.");
  } else {
    od.ok = true;
    od.available_effects.push_back(std::string(
        studiocast::audio::effects::contract::kEffectIdNoiseRemoval));
    od.available_effects.push_back(std::string(
        studiocast::audio::effects::contract::kEffectIdRoomEchoRemoval));
    od.available_effects.push_back(std::string(
        studiocast::audio::effects::contract::kEffectIdStudioVoice));
  }
#else
  od.ok = false;
  block_effects("onnxruntime_not_found");
  od.install_hints.push_back(
      "Open Audio backend is disabled in this build (ONNX Runtime not found).");
  od.install_hints.push_back(
      "Install onnxruntime and rebuild (or set ONNXRUNTIME_ROOT).");
#endif

  return od;
}

} // namespace studiocast::open_audio
