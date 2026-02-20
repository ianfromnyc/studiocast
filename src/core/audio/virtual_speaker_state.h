#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace studiocast::audio {

struct VirtualSpeakerState {
  std::optional<int> null_sink_module_id;
  std::optional<int> loopback_module_id;
  std::optional<std::string> loopback_target_sink_name;
};

std::filesystem::path VirtualSpeakerStatePath();

VirtualSpeakerState LoadVirtualSpeakerState();
bool SaveVirtualSpeakerState(const VirtualSpeakerState &s, std::string *error);
bool ClearVirtualSpeakerState(std::string *error);

} // namespace studiocast::audio
