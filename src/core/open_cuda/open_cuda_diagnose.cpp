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
  const auto openCudaRoot = modelsRoot.empty() ? std::string("~/.local/share/studiocast/models/open_cuda")
                                             : (modelsRoot / "open_cuda").string();

  od.install_hints.push_back(std::string("Model packs: ") + openCudaRoot + "/<model_id>/");
  od.install_hints.push_back("Each pack must contain: model.json, model.onnx, LICENSE.txt");

  const auto block_vb_effects = [&](const char* reason_code) {
    od.blocked_effects[std::string(studiocast::video::effects::contract::kEffectIdVirtualBackgroundBlur)] =
        reason_code;
    od.blocked_effects[std::string(studiocast::video::effects::contract::kEffectIdVirtualBackgroundRemove)] =
        reason_code;
    od.blocked_effects[std::string(studiocast::video::effects::contract::kEffectIdVirtualBackgroundReplace)] =
        reason_code;
  };

#if STUDIOCAST_HAVE_ONNXRUNTIME
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
    block_vb_effects("cuda_unavailable");
    od.install_hints.push_back(std::string("CUDA not available: ") + cuda_err);
  } else if (od.installed_models.empty()) {
    od.ok = false;
    block_vb_effects("missing_model_packs");
    od.install_hints.push_back("No usable Open CUDA model packs were found.");
  } else {
    od.ok = true;
    od.available_effects.push_back(
        std::string(studiocast::video::effects::contract::kEffectIdVirtualBackgroundBlur));
    od.available_effects.push_back(
        std::string(studiocast::video::effects::contract::kEffectIdVirtualBackgroundRemove));
    od.available_effects.push_back(
        std::string(studiocast::video::effects::contract::kEffectIdVirtualBackgroundReplace));
  }
#else
  od.ok = false;
  block_vb_effects("onnxruntime_not_found");
  od.install_hints.push_back("Open CUDA backend is disabled in this build (ONNX Runtime not found). ");
  od.install_hints.push_back("Rebuild with -DSTUDIOCAST_ENABLE_OPEN_CUDA=ON and ensure ONNXRUNTIME_ROOT is set.");
#endif

  return od;
}

}  // namespace studiocast::open_cuda
