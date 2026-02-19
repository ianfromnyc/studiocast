#include "core/open_cuda/open_cuda_diagnose.h"

#include "core/maxine/cuda_driver_api.h"
#include "core/open_cuda/model_pack_registry.h"
#include "core/util/xdg.h"
#include "core/video/effects/broadcast_effect_contract.h"

namespace studiocast::open_cuda {

OpenCudaDiagnostics DiagnoseOpenCudaDefault() {
  OpenCudaDiagnostics od;

  const auto reg = ModelPackRegistry::ScanDefault();
  od.default_model_id = reg.DefaultModelId();
  for (const auto& m : reg.ListModels()) {
    od.installed_models.push_back(m.id);
    OpenCudaDiagnostics::ModelInfo mi;
    mi.id = m.id;
    mi.display_name = m.display_name;
    mi.task = m.task;
    mi.width = m.input.width;
    mi.height = m.input.height;
    od.models.push_back(std::move(mi));
  }
  od.missing_models = reg.Problems();

  const auto modelsRoot = studiocast::util::StudioCastModelsDir();
  const auto openVideoRoot = modelsRoot.empty() ? std::string("~/.local/share/studiocast/models/open_video")
                                               : (modelsRoot / "open_video").string();

  od.install_hints.push_back(std::string("Model packs: ") + openVideoRoot + "/<subject>/<pack_dir>/");
  od.install_hints.push_back("Example: " + openVideoRoot + "/segmentation/Good Quality/model.json");
  od.install_hints.push_back("Each pack must contain: model.json, model.onnx, LICENSE.txt");

  const auto block_open_cuda_effects = [&](const char* reason_code) {
    od.blocked_effects[std::string(studiocast::video::effects::contract::kEffectIdVideoNoiseRemoval)] = reason_code;
    od.blocked_effects[std::string(studiocast::video::effects::contract::kEffectIdVirtualBackgroundBlur)] =
        reason_code;
    od.blocked_effects[std::string(studiocast::video::effects::contract::kEffectIdVirtualBackgroundRemove)] =
        reason_code;
    od.blocked_effects[std::string(studiocast::video::effects::contract::kEffectIdVirtualBackgroundReplace)] =
        reason_code;
    od.blocked_effects[std::string(studiocast::video::effects::contract::kEffectIdAutoFrame)] = reason_code;
    od.blocked_effects[std::string(studiocast::video::effects::contract::kEffectIdVirtualKeyLight)] = reason_code;
  };

  const auto block_open_cuda_matting_effects = [&](const char* reason_code) {
    od.blocked_effects[std::string(studiocast::video::effects::contract::kEffectIdVirtualBackgroundBlur)] =
        reason_code;
    od.blocked_effects[std::string(studiocast::video::effects::contract::kEffectIdVirtualBackgroundRemove)] =
        reason_code;
    od.blocked_effects[std::string(studiocast::video::effects::contract::kEffectIdVirtualBackgroundReplace)] =
        reason_code;
    od.blocked_effects[std::string(studiocast::video::effects::contract::kEffectIdAutoFrame)] = reason_code;
    od.blocked_effects[std::string(studiocast::video::effects::contract::kEffectIdVirtualKeyLight)] = reason_code;
  };

#if !STUDIOCAST_ENABLE_OPEN_CUDA
  od.ok = false;
  block_open_cuda_effects("disabled_in_build");
  od.install_hints.push_back("Open CUDA backend is disabled in this build.");
  od.install_hints.push_back(
      "Rebuild with -DSTUDIOCAST_ENABLE_OPEN_CUDA=ON (requires ONNX Runtime + CUDA EP).");
#elif STUDIOCAST_HAVE_ONNXRUNTIME
  // CUDA driver/device gate. This avoids repeatedly attempting to start the
  // pipeline only to fail deep in Open CUDA initialization when no NVIDIA
  // driver/GPU is available.
  bool cuda_ok = true;
  std::string cuda_err;
  {
    studiocast::maxine::CudaDriverApi cuda;
    std::string e;
    if (!cuda.Initialize(&e)) {
      cuda_ok = false;
      cuda_err = e.empty() ? std::string("CUDA driver API not available.") : e;
    } else if (!cuda.EnsureContext(&e)) {
      cuda_ok = false;
      cuda_err = e.empty() ? std::string("Failed to ensure CUDA context.") : e;
    }
  }

  if (!cuda_ok) {
    od.ok = false;
    block_open_cuda_effects("cuda_unavailable");
    od.install_hints.push_back(std::string("CUDA not available: ") + cuda_err);
  } else {
    od.ok = true;

    // Open CUDA Video Noise Removal is implemented without model packs.
    od.available_effects.push_back(std::string(studiocast::video::effects::contract::kEffectIdVideoNoiseRemoval));

    if (od.installed_models.empty()) {
      // Segmentation/matting-based effects require at least one usable model pack.
      block_open_cuda_matting_effects("missing_model_packs");
      od.install_hints.push_back(
          "No usable Open CUDA model packs were found (required for segmentation-based effects). Video Noise Removal can still run without packs.");
    } else {
      od.available_effects.push_back(
          std::string(studiocast::video::effects::contract::kEffectIdVirtualBackgroundBlur));
      od.available_effects.push_back(
          std::string(studiocast::video::effects::contract::kEffectIdVirtualBackgroundRemove));
      od.available_effects.push_back(
          std::string(studiocast::video::effects::contract::kEffectIdVirtualBackgroundReplace));
      od.available_effects.push_back(std::string(studiocast::video::effects::contract::kEffectIdAutoFrame));
      od.available_effects.push_back(std::string(studiocast::video::effects::contract::kEffectIdVirtualKeyLight));
    }
  }
#else
  od.ok = false;
  block_open_cuda_effects("onnxruntime_not_found");
  od.install_hints.push_back("Open CUDA backend is disabled in this build (ONNX Runtime not found). ");
  od.install_hints.push_back("Rebuild with -DSTUDIOCAST_ENABLE_OPEN_CUDA=ON and ensure ONNXRUNTIME_ROOT is set.");
#endif

  return od;
}

}  // namespace studiocast::open_cuda
