#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace studiocast::audio {

struct VirtualMicState {
  std::optional<int> null_sink_module_id;
  std::optional<int> remap_source_module_id;
  // Legacy compatibility only.
  // Production “processed feed” mode does not create or rely on
  // module-loopback.
  std::optional<int> loopback_module_id;
};

std::filesystem::path VirtualMicStatePath();

VirtualMicState LoadVirtualMicState();
bool SaveVirtualMicState(const VirtualMicState &s, std::string *error);
bool ClearVirtualMicState(std::string *error);

} // namespace studiocast::audio
