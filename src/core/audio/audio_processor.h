#pragma once

#include <cstdint>
#include <string>

namespace studiocast::audio {

// Simple interface used by the audio pipeline.
// Implementations should be real-time safe: avoid allocations in Process().
class AudioProcessor {
public:
    virtual ~AudioProcessor() = default;

    // Process one frame.
    // - in/out are float PCM samples in [-1, 1].
    // - frames is the number of frames (not samples), channels is channel count.
    virtual bool Process(const float* in,
                         float* out,
                         std::uint32_t frames,
                         std::uint32_t channels,
                         std::string* error) = 0;

    virtual void Reset() {}
};

class PassthroughAudioProcessor final : public AudioProcessor {
public:
    bool Process(const float* in,
                 float* out,
                 std::uint32_t frames,
                 std::uint32_t channels,
                 std::string* error) override;
};

}  // namespace studiocast::audio
