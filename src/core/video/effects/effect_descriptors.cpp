#include "effect_descriptors.h"

#include "core/video/effects/broadcast_effect_contract.h"

namespace studiocast::video::effects {

std::string ToString(RequiredComponent v) {
  switch (v) {
  case RequiredComponent::none:
    return "none";
  case RequiredComponent::gpu_utility:
    return "gpu_utility";
  case RequiredComponent::maxine_vfx:
    return "maxine_vfx";
  case RequiredComponent::maxine_ar:
    return "maxine_ar";
  }
  return "none";
}

std::string ToString(ParamType v) {
  switch (v) {
  case ParamType::boolean:
    return "bool";
  case ParamType::integer:
    return "int";
  case ParamType::floating:
    return "float";
  case ParamType::string:
    return "string";
  }
  return "bool";
}

std::vector<VideoEffectDescriptor> VideoEffectDescriptors() {
  // Important: IDs are stable across persistence/IPC/GUI.
  // Display strings should be Broadcast-style but generic (no vendor branding).
  std::vector<VideoEffectDescriptor> out;

  {
    VideoEffectDescriptor d;
    d.id = std::string(contract::kEffectIdMirror);
    d.display_name = "Mirror";
    d.required_components = {RequiredComponent::none};
    d.pipeline_order = 10;
    d.params = {
        ParamDescriptor{.id = std::string(contract::param::kEnabled),
                        .display_name = "Enabled",
                        .type = ParamType::boolean,
                        .default_bool = false,
                        .default_string = ""},
    };
    out.push_back(d);
  }

  // Virtual Background modes (mutually exclusive).
  // These share the stable mode strings used by IPC: blur/remove/replace.
  {
    VideoEffectDescriptor d;
    d.id = std::string(contract::kEffectIdVirtualBackgroundBlur);
    d.display_name = "Virtual Background — Blur";
    d.required_components = {RequiredComponent::maxine_vfx};
    d.mutex_groups = {std::string(contract::kMutexGroupVirtualBackgroundMode)};
    d.pipeline_order = 40;
    d.params = {
        ParamDescriptor{.id = std::string(contract::param::kEnabled),
                        .display_name = "Enabled",
                        .type = ParamType::boolean,
                        .default_bool = false,
                        .default_string = ""},
        ParamDescriptor{.id = std::string(contract::param::kStrength),
                        .display_name = "Strength",
                        .type = ParamType::integer,
                        .min = contract::kVbStrengthMin,
                        .max = contract::kVbStrengthMax,
                        .step = 1,
                        .default_int = contract::kVbStrengthDefault,
                        .default_string = ""},
    };
    out.push_back(d);
  }
  {
    VideoEffectDescriptor d;
    d.id = std::string(contract::kEffectIdVirtualBackgroundRemove);
    d.display_name = "Virtual Background — Remove";
    d.required_components = {RequiredComponent::maxine_vfx};
    d.mutex_groups = {std::string(contract::kMutexGroupVirtualBackgroundMode)};
    d.pipeline_order = 40;
    d.params = {
        ParamDescriptor{.id = std::string(contract::param::kEnabled),
                        .display_name = "Enabled",
                        .type = ParamType::boolean,
                        .default_bool = false,
                        .default_string = ""},
        ParamDescriptor{.id = std::string(contract::param::kStrength),
                        .display_name = "Strength",
                        .type = ParamType::integer,
                        .min = contract::kVbStrengthMin,
                        .max = contract::kVbStrengthMax,
                        .step = 1,
                        .default_int = contract::kVbStrengthDefault,
                        .default_string = ""},
        ParamDescriptor{.id = std::string(contract::param::kVbRemoveColor),
                        .display_name = "Remove color",
                        .type = ParamType::string,
                        .default_string = "#000000"},
        ParamDescriptor{.id = std::string(contract::param::kGreenscreenMode),
                        .display_name = "Mode",
                        .type = ParamType::integer,
                        .min = 0,
                        .max = 8,
                        .step = 1,
                        .default_int = contract::kGreenscreenModeDefault,
                        .default_string = ""},
        ParamDescriptor{.id =
                            std::string(contract::param::kGreenscreenTemporal),
                        .display_name = "Temporal consistency",
                        .type = ParamType::boolean,
                        .default_bool = contract::kGreenscreenTemporalDefault,
                        .default_string = ""},
    };
    out.push_back(d);
  }
  {
    VideoEffectDescriptor d;
    d.id = std::string(contract::kEffectIdVirtualBackgroundReplace);
    d.display_name = "Virtual Background — Replace";
    d.required_components = {RequiredComponent::maxine_vfx};
    d.mutex_groups = {std::string(contract::kMutexGroupVirtualBackgroundMode)};
    d.pipeline_order = 40;
    d.params = {
        ParamDescriptor{.id = std::string(contract::param::kEnabled),
                        .display_name = "Enabled",
                        .type = ParamType::boolean,
                        .default_bool = false,
                        .default_string = ""},
        ParamDescriptor{.id = std::string(contract::param::kStrength),
                        .display_name = "Strength",
                        .type = ParamType::integer,
                        .min = contract::kVbStrengthMin,
                        .max = contract::kVbStrengthMax,
                        .step = 1,
                        .default_int = contract::kVbStrengthDefault,
                        .default_string = ""},
        ParamDescriptor{.id = std::string(contract::param::kVbReplacePath),
                        .display_name = "Image",
                        .type = ParamType::string,
                        .default_string = ""},
        ParamDescriptor{.id = std::string(contract::param::kVbRemoveColor),
                        .display_name = "Remove color",
                        .type = ParamType::string,
                        .default_string = "#000000"},
        ParamDescriptor{.id = std::string(contract::param::kGreenscreenMode),
                        .display_name = "Mode",
                        .type = ParamType::integer,
                        .min = 0,
                        .max = 8,
                        .step = 1,
                        .default_int = contract::kGreenscreenModeDefault,
                        .default_string = ""},
        ParamDescriptor{.id =
                            std::string(contract::param::kGreenscreenTemporal),
                        .display_name = "Temporal consistency",
                        .type = ParamType::boolean,
                        .default_bool = contract::kGreenscreenTemporalDefault,
                        .default_string = ""},
    };
    out.push_back(d);
  }

  // Maxine AR effects.
  {
    VideoEffectDescriptor d;
    d.id = std::string(contract::kEffectIdAutoFrame);
    d.display_name = "Auto Frame";
    // Auto Frame can run either via Maxine AR (when available) or via the Open
    // CUDA fallback. We only require the generic GPU utility component here.
    d.required_components = {RequiredComponent::gpu_utility};
    d.pipeline_order = 20;
    d.params = {
        ParamDescriptor{.id = std::string(contract::param::kEnabled),
                        .display_name = "Enabled",
                        .type = ParamType::boolean,
                        .default_bool = false,
                        .default_string = ""},
        ParamDescriptor{.id = std::string(contract::param::kStrength),
                        .display_name = "Strength",
                        .type = ParamType::integer,
                        .min = contract::kAutoFrameStrengthMin,
                        .max = contract::kAutoFrameStrengthMax,
                        .step = 1,
                        .default_int = contract::kAutoFrameStrengthDefault,
                        .default_string = ""},
        ParamDescriptor{.id = std::string(contract::param::kSmoothing),
                        .display_name = "Smoothing",
                        .type = ParamType::integer,
                        .min = 0,
                        .max = 100,
                        .step = 1,
                        .default_int = contract::kAutoFrameSmoothingDefault,
                        .default_string = ""},
        ParamDescriptor{.id = std::string(contract::param::kHeadroom),
                        .display_name = "Headroom",
                        .type = ParamType::floating,
                        .min_f = contract::kAutoFrameHeadroomMin,
                        .max_f = contract::kAutoFrameHeadroomMax,
                        .step_f = 0.01f,
                        .default_float = contract::kAutoFrameHeadroomDefault,
                        .default_string = ""},
    };
    out.push_back(d);
  }
  {
    VideoEffectDescriptor d;
    d.id = std::string(contract::kEffectIdEyeContact);
    d.display_name = "Eye Contact";
    // Auto Frame can run either via Maxine AR (when available) or via the Open
    // CUDA fallback. We only require the generic GPU utility component here.
    d.required_components = {RequiredComponent::gpu_utility};
    d.pipeline_order = 30;
    d.params = {
        ParamDescriptor{.id = std::string(contract::param::kEnabled),
                        .display_name = "Enabled",
                        .type = ParamType::boolean,
                        .default_bool = false,
                        .default_string = ""},
        ParamDescriptor{.id = std::string(contract::param::kStrength),
                        .display_name = "Strength",
                        .type = ParamType::integer,
                        .min = 0,
                        .max = 100,
                        .step = 1,
                        .default_int = contract::kEyeContactStrengthDefault,
                        .default_string = ""},
        ParamDescriptor{.id = std::string(contract::param::kLookAwayEnabled),
                        .display_name = "Allow look away",
                        .type = ParamType::boolean,
                        .default_bool = true,
                        .default_string = ""},
    };
    out.push_back(d);
  }

  // VFX-ish effects.
  {
    VideoEffectDescriptor d;
    d.id = std::string(contract::kEffectIdVideoNoiseRemoval);
    d.display_name = "Video Noise Removal";
    // Open-source (Open CUDA) fallback exists; keep this available when Maxine
    // is missing.
    d.required_components = {RequiredComponent::gpu_utility};
    d.pipeline_order = 50;
    d.params = {
        ParamDescriptor{.id = std::string(contract::param::kEnabled),
                        .display_name = "Enabled",
                        .type = ParamType::boolean,
                        .default_bool = false,
                        .default_string = ""},
        ParamDescriptor{.id = std::string(contract::param::kStrength),
                        .display_name = "Strength",
                        .type = ParamType::integer,
                        .min = 0,
                        .max = 100,
                        .step = 1,
                        .default_int =
                            contract::kVideoNoiseRemovalStrengthDefault,
                        .default_string = ""},
    };
    out.push_back(d);
  }
  {
    VideoEffectDescriptor d;
    d.id = std::string(contract::kEffectIdVirtualKeyLight);
    d.display_name = "Virtual Key Light";
    // Open-source (Open CUDA) fallback exists; keep this available when Maxine
    // is missing.
    d.required_components = {RequiredComponent::gpu_utility};
    d.pipeline_order = 60;
    d.params = {
        ParamDescriptor{.id = std::string(contract::param::kEnabled),
                        .display_name = "Enabled",
                        .type = ParamType::boolean,
                        .default_bool = false,
                        .default_string = ""},
        ParamDescriptor{.id = std::string(contract::param::kIntensity),
                        .display_name = "Intensity",
                        .type = ParamType::integer,
                        .min = 0,
                        .max = 100,
                        .step = 1,
                        .default_int =
                            contract::kVirtualKeyLightIntensityDefault,
                        .default_string = ""},
        ParamDescriptor{.id = std::string(contract::param::kTemperaturePreset),
                        .display_name = "Temperature preset",
                        .type = ParamType::string,
                        .default_string = "neutral"},
        ParamDescriptor{.id =
                            std::string(contract::param::kDirectionPanDegrees),
                        .display_name = "Direction pan (degrees)",
                        .type = ParamType::integer,
                        .min = contract::kVirtualKeyLightPanMin,
                        .max = contract::kVirtualKeyLightPanMax,
                        .step = 1,
                        .default_int = 0,
                        .default_string = ""},
        ParamDescriptor{.id = std::string(contract::param::kHdriPath),
                        .display_name = "HDRI path",
                        .type = ParamType::string,
                        .default_string = ""},
    };
    out.push_back(d);
  }
  {
    VideoEffectDescriptor d;
    d.id = std::string(contract::kEffectIdVignette);
    d.display_name = "Vignette";
    d.required_components = {RequiredComponent::gpu_utility};
    d.pipeline_order = 70;
    d.params = {
        ParamDescriptor{.id = std::string(contract::param::kEnabled),
                        .display_name = "Enabled",
                        .type = ParamType::boolean,
                        .default_bool = false,
                        .default_string = ""},
        ParamDescriptor{.id = std::string(contract::param::kIntensity),
                        .display_name = "Intensity",
                        .type = ParamType::integer,
                        .min = 0,
                        .max = 100,
                        .step = 1,
                        .default_int = contract::kVignetteIntensityDefault,
                        .default_string = ""},
        ParamDescriptor{.id =
                            std::string(contract::param::kCenterOnTrackedFace),
                        .display_name = "Center on tracked face",
                        .type = ParamType::boolean,
                        .default_bool = true,
                        .default_string = ""},
    };
    out.push_back(d);
  }

  return out;
}

} // namespace studiocast::video::effects
