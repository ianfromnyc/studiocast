#pragma once

#include <optional>
#include <string>
#include <vector>

namespace studiocast::audio {

struct AudioSourceResolution {
  bool ok = false;
  std::string source_name;
  std::string error;
  std::vector<std::string> warnings;
};

bool IsStudioCastVirtualSourceName(const std::string &name);
bool IsPulseMonitorSourceName(const std::string &name);
bool IsUnsafeInputSourceName(const std::string &name, std::string *reason);

bool IsStudioCastVirtualSinkName(const std::string &name);
bool IsUnsafeSpeakerTargetSinkName(const std::string &name,
                                   std::string *reason);

AudioSourceResolution
ResolveSafeInputSourceName(const std::string &configured_source);

std::optional<std::string>
ChooseSafeSpeakerTargetSinkName(const std::string &configured_target,
                                std::string *error);

} // namespace studiocast::audio
