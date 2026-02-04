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
  string = 2,
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

  // Defaults.
  bool default_bool = false;
  int default_int = 0;
  std::string default_string;
};

struct VideoEffectDescriptor {
  // Stable effect ID (IPC + JSON).
  std::string id;

  // Human-friendly label for UI.
  std::string display_name;

  // Required engine components.
  std::vector<RequiredComponent> required_components;

  // Mutual exclusion groups. Effects in the same group are not intended to be enabled at once.
  std::vector<std::string> mutex_groups;

  // Suggested ordering in the pipeline (lower runs earlier). A hint only.
  int pipeline_order = 0;

  // Parameter schema.
  std::vector<ParamDescriptor> params;
};

// Canonical, stable list of known video effects (Qt-free).
std::vector<VideoEffectDescriptor> VideoEffectDescriptors();

struct UnavailableEffectInfo {
  std::string id;
  std::string reason;
};

struct VideoEffectAvailabilityReport {
  std::vector<std::string> available_effects;
  std::vector<UnavailableEffectInfo> unavailable_effects;
};

// Evaluates which effects are currently runnable based on runtime checks.
VideoEffectAvailabilityReport EvaluateVideoEffectAvailability(const std::vector<VideoEffectDescriptor>& descs);

}  // namespace studiocast::video::effects
