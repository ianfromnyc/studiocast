#pragma once

#include <string>
#include <vector>

namespace studiocast::video::effects {

enum class RequiredComponent {
  none = 0,
  gpu_utility = 1,
  maxine_vfx = 2,
  maxine_ar = 3,
};

std::string ToString(RequiredComponent v);

enum class ParamType {
  boolean = 0,
  integer = 1,
  floating = 2,
  string = 3,
};

std::string ToString(ParamType v);

struct ParamDescriptor {
  // Stable parameter ID within the effect.
  std::string id;

  // Human-friendly label for UI.
  std::string display_name;

  ParamType type = ParamType::boolean;

  // Integer constraints (meaningful when type==integer).
  int min = 0;
  int max = 0;
  int step = 1;

  // Float constraints (meaningful when type==floating).
  float min_f = 0.0f;
  float max_f = 0.0f;
  float step_f = 0.0f;

  // Defaults.
  bool default_bool = false;
  int default_int = 0;
  float default_float = 0.0f;
  std::string default_string;
};

struct VideoEffectDescriptor {
  // Stable effect ID (IPC + JSON).
  std::string id;

  // Human-friendly label for UI.
  std::string display_name;

  // Required engine components.
  std::vector<RequiredComponent> required_components;

  // Mutual exclusion groups. Effects in the same group are not intended to be
  // enabled at once.
  std::vector<std::string> mutex_groups;

  // Suggested ordering in the pipeline (lower runs earlier). A hint only.
  int pipeline_order = 0;

  // Parameter schema.
  std::vector<ParamDescriptor> params;
};

// Canonical, stable list of known video effects (Qt-free).
std::vector<VideoEffectDescriptor> VideoEffectDescriptors();

} // namespace studiocast::video::effects
