#pragma once

#include <memory>
#include <string>

#include "core/audio/virtual_audio_service.h"

namespace studiocast::audio {

class AudioConsumerDetector {
public:
  virtual ~AudioConsumerDetector() = default;

  virtual AudioConsumerSnapshot
  DetectSourceConsumersByName(const std::string &source_name) = 0;
  virtual AudioConsumerSnapshot
  DetectSinkConsumersByName(const std::string &sink_name) = 0;
};

// Production detector. It prefers a libpulse subscription-backed cache and
// falls back to pactl snapshots if the subscription monitor is unavailable.
std::unique_ptr<AudioConsumerDetector> CreateDefaultAudioConsumerDetector();

} // namespace studiocast::audio
