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

  for (const auto& m : reg.ListModels()) {
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
  const auto openAudioRoot = modelsRoot.empty()
                                 ? std::string("~/.local/share/studiocast/models/open_audio")
                                 : (modelsRoot / "open_audio").string();

  od.install_hints.push_back(std::string("Model packs: ") + openAudioRoot + "/<model_id>/");
  od.install_hints.push_back(
      "Source builds: run ./scripts/install_open_audio_models.sh to install the curated FastEnhancer pack.");
  od.install_hints.push_back("Docs: docs/open_audio_install.md");
  od.install_hints.push_back(
      "Each pack must contain: model.json, the ONNX file referenced by onnx_filename, and LICENSE.txt.");

  const auto block_effects = [&](const char* reason_code) {
    od.blocked_effects[std::string(studiocast::audio::effects::contract::kEffectIdNoiseRemoval)] = reason_code;
    od.blocked_effects[std::string(studiocast::audio::effects::contract::kEffectIdRoomEchoRemoval)] = reason_code;
    od.blocked_effects[std::string(studiocast::audio::effects::contract::kEffectIdStudioVoice)] = reason_code;
  };

#if !STUDIOCAST_ENABLE_OPEN_AUDIO
  od.ok = false;
  block_effects("disabled_in_build");
  od.install_hints.push_back("Open Audio backend is disabled in this build.");
  od.install_hints.push_back("Rebuild with -DSTUDIOCAST_ENABLE_OPEN_AUDIO=ON (requires ONNX Runtime).");
#elif STUDIOCAST_HAVE_ONNXRUNTIME
  {
    const auto ort = OpenAudioOrtSession::QueryRuntimeInfo();
    od.onnxruntime_version = ort.version;
    od.onnxruntime_providers = ort.providers;
  }

  if (od.installed_models.empty()) {
    od.ok = false;
    block_effects("missing_model_packs");
    od.install_hints.push_back("No usable Open Audio model packs were found.");
  } else {
    od.ok = true;
    od.available_effects.push_back(std::string(studiocast::audio::effects::contract::kEffectIdNoiseRemoval));
    od.available_effects.push_back(std::string(studiocast::audio::effects::contract::kEffectIdRoomEchoRemoval));
    od.available_effects.push_back(std::string(studiocast::audio::effects::contract::kEffectIdStudioVoice));
  }
#else
  od.ok = false;
  block_effects("onnxruntime_not_found");
  od.install_hints.push_back("Open Audio backend is disabled in this build (ONNX Runtime not found).");
  od.install_hints.push_back("Install onnxruntime and rebuild (or set ONNXRUNTIME_ROOT).");
#endif

  return od;
}

}  // namespace studiocast::open_audio
