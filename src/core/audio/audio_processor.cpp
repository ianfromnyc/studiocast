#include "core/audio/audio_processor.h"

#include <algorithm>
#include <cstdint>

namespace studiocast::audio {

bool PassthroughAudioProcessor::Process(const float *in, float *out,
                                        std::uint32_t frames,
                                        std::uint32_t channels,
                                        std::string *error) {
  (void)error;
  if (!in || !out) {
    if (error)
      *error = "null audio buffer";
    return false;
  }
  const std::uint64_t samples64 =
      static_cast<std::uint64_t>(frames) * static_cast<std::uint64_t>(channels);
  const auto samples = static_cast<std::size_t>(samples64);
  std::copy_n(in, samples, out);
  return true;
}

} // namespace studiocast::audio
