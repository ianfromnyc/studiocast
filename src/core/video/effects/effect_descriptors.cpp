#include "effect_descriptors.h"

#include "core/maxine/availability.h"

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
    d.id = "mirror";
    d.display_name = "Mirror";
    d.required_components = {RequiredComponent::none};
    d.pipeline_order = 10;
    d.params = {
        ParamDescriptor{.id = "enabled", .display_name = "Enabled", .type = ParamType::boolean, .default_bool = false, .default_string = ""},
    };
    out.push_back(d);
  }

  // Virtual Background modes (mutually exclusive).
  // These share the stable mode strings used by IPC: blur/remove/replace.
  {
    VideoEffectDescriptor d;
    d.id = "virtual_background.blur";
    d.display_name = "Virtual Background — Blur";
    d.required_components = {RequiredComponent::maxine_vfx};
    d.mutex_groups = {"virtual_background_mode", "background_or_auto_frame"};
    d.pipeline_order = 40;
    d.params = {
        ParamDescriptor{.id = "strength",
                        .display_name = "Strength",
                        .type = ParamType::integer,
                        .min = 1,
                        .max = 64,
                        .step = 1,
                        .default_int = 8,
                        .default_string = ""},
    };
    out.push_back(d);
  }
  {
    VideoEffectDescriptor d;
    d.id = "virtual_background.remove";
    d.display_name = "Virtual Background — Remove";
    d.required_components = {RequiredComponent::maxine_vfx};
    d.mutex_groups = {"virtual_background_mode", "background_or_auto_frame"};
    d.pipeline_order = 40;
    d.params = {
        ParamDescriptor{.id = "greenscreen_mode",
                        .display_name = "Mode",
                        .type = ParamType::integer,
                        .min = 0,
                        .max = 8,
                        .step = 1,
                        .default_int = 0,
                        .default_string = ""},
        ParamDescriptor{.id = "greenscreen_temporal",
                        .display_name = "Temporal consistency",
                        .type = ParamType::boolean,
                        .default_bool = true,
                        .default_string = ""},
    };
    out.push_back(d);
  }
  {
    VideoEffectDescriptor d;
    d.id = "virtual_background.replace";
    d.display_name = "Virtual Background — Replace";
    d.required_components = {RequiredComponent::maxine_vfx};
    d.mutex_groups = {"virtual_background_mode", "background_or_auto_frame"};
    d.pipeline_order = 40;
    d.params = {
        ParamDescriptor{.id = "replace_path", .display_name = "Image", .type = ParamType::string, .default_string = ""},
        ParamDescriptor{.id = "greenscreen_mode",
                        .display_name = "Mode",
                        .type = ParamType::integer,
                        .min = 0,
                        .max = 8,
                        .step = 1,
                        .default_int = 0,
                        .default_string = ""},
        ParamDescriptor{.id = "greenscreen_temporal",
                        .display_name = "Temporal consistency",
                        .type = ParamType::boolean,
                        .default_bool = true,
                        .default_string = ""},
    };
    out.push_back(d);
  }

  // Maxine AR effects.
  {
    VideoEffectDescriptor d;
    d.id = "auto_frame";
    d.display_name = "Auto Frame";
    d.required_components = {RequiredComponent::maxine_ar};
    d.mutex_groups = {"background_or_auto_frame"};
    d.pipeline_order = 20;
    d.params = {
        ParamDescriptor{.id = "enabled", .display_name = "Enabled", .type = ParamType::boolean, .default_bool = false, .default_string = ""},
        ParamDescriptor{.id = "strength", .display_name = "Strength", .type = ParamType::integer, .min = 0, .max = 100, .step = 1, .default_int = 50, .default_string = ""},
        ParamDescriptor{.id = "smoothing", .display_name = "Smoothing", .type = ParamType::integer, .min = 0, .max = 100, .step = 1, .default_int = 50, .default_string = ""},
    };
    out.push_back(d);
  }
  {
    VideoEffectDescriptor d;
    d.id = "eye_contact";
    d.display_name = "Eye Contact";
    d.required_components = {RequiredComponent::maxine_ar};
    d.pipeline_order = 30;
    d.params = {
        ParamDescriptor{.id = "enabled", .display_name = "Enabled", .type = ParamType::boolean, .default_bool = false, .default_string = ""},
        ParamDescriptor{.id = "strength", .display_name = "Strength", .type = ParamType::integer, .min = 0, .max = 100, .step = 1, .default_int = 50, .default_string = ""},
        ParamDescriptor{.id = "look_away_enabled", .display_name = "Allow look away", .type = ParamType::boolean, .default_bool = true, .default_string = ""},
    };
    out.push_back(d);
  }

  // VFX-ish effects.
  {
    VideoEffectDescriptor d;
    d.id = "video_noise_removal";
    d.display_name = "Video Noise Removal";
    d.required_components = {RequiredComponent::maxine_vfx};
    d.pipeline_order = 50;
    d.params = {
        ParamDescriptor{.id = "enabled", .display_name = "Enabled", .type = ParamType::boolean, .default_bool = false, .default_string = ""},
        ParamDescriptor{.id = "strength", .display_name = "Strength", .type = ParamType::integer, .min = 0, .max = 100, .step = 1, .default_int = 50, .default_string = ""},
    };
    out.push_back(d);
  }
  {
    VideoEffectDescriptor d;
    d.id = "virtual_key_light";
    d.display_name = "Virtual Key Light";
    d.required_components = {RequiredComponent::maxine_vfx};
    d.pipeline_order = 60;
    d.params = {
        ParamDescriptor{.id = "enabled", .display_name = "Enabled", .type = ParamType::boolean, .default_bool = false, .default_string = ""},
        ParamDescriptor{.id = "intensity", .display_name = "Intensity", .type = ParamType::integer, .min = 0, .max = 100, .step = 1, .default_int = 50, .default_string = ""},
        ParamDescriptor{.id = "temperature", .display_name = "Temperature (K)", .type = ParamType::integer, .min = 2000, .max = 6500, .step = 100, .default_int = 4500, .default_string = ""},
    };
    out.push_back(d);
  }
  {
    VideoEffectDescriptor d;
    d.id = "vignette";
    d.display_name = "Vignette";
    d.required_components = {RequiredComponent::gpu_utility};
    d.pipeline_order = 70;
    d.params = {
        ParamDescriptor{.id = "enabled", .display_name = "Enabled", .type = ParamType::boolean, .default_bool = false, .default_string = ""},
        ParamDescriptor{.id = "intensity", .display_name = "Intensity", .type = ParamType::integer, .min = 0, .max = 100, .step = 1, .default_int = 25, .default_string = ""},
    };
    out.push_back(d);
  }

  return out;
}

VideoEffectAvailabilityReport EvaluateVideoEffectAvailability(const std::vector<VideoEffectDescriptor>& descs) {
  VideoEffectAvailabilityReport rep;

  std::string maxineReason;
  const bool maxineOk = studiocast::maxine::RuntimeAvailable(&maxineReason);
  if (maxineReason.empty()) maxineReason = "Maxine runtime unavailable.";

  for (const auto& d : descs) {
    bool ok = true;
    std::string reason;

    for (const auto c : d.required_components) {
      if (c == RequiredComponent::maxine_vfx || c == RequiredComponent::maxine_ar) {
        if (!maxineOk) {
          ok = false;
          reason = maxineReason;
          break;
        }
      }
    }

    if (ok) {
      rep.available_effects.push_back(d.id);
    } else {
      rep.unavailable_effects.push_back(UnavailableEffectInfo{.id = d.id, .reason = reason});
    }
  }

  return rep;
}

}  // namespace studiocast::video::effects
