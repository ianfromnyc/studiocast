#pragma once

#include <cstdint>
#include <limits>
#include <string>

#include "core/audio/audio_processor.h"
#include "core/audio/dsp/post_dsp_chain.h"
#include "core/maxine/afx/afx_effect.h"

namespace studiocast::maxine::afx {

// Adapter to run a Maxine `AfxEffect` through the generic `AudioProcessor`
// pipeline.
class AfxAudioProcessor final : public studiocast::audio::AudioProcessor {
public:
  explicit AfxAudioProcessor(AfxEffect *effect) : effect_(effect) {
    std::string derr;
    post_dsp_.Configure(/*sample_rate=*/48000, /*channels=*/1, &derr);
  }

  void Reset() override { post_dsp_.Reset(); }

  bool Process(const float *in, float *out, std::uint32_t frames,
               std::uint32_t channels, std::string *error) override {
    if (!effect_) {
      if (error)
        *error = "AFX effect is null";
      return false;
    }

    const std::uint64_t samples64 = static_cast<std::uint64_t>(frames) *
                                    static_cast<std::uint64_t>(channels);
    if (samples64 > std::numeric_limits<std::uint32_t>::max()) {
      if (error)
        *error = "frame is too large";
      return false;
    }
    if (!effect_->Run(in, out, static_cast<std::uint32_t>(samples64), error)) {
      return false;
    }

    // Apply a small post-processing safety stage (DC blocker + limiter) to
    // avoid harsh clipping when the AFX output overshoots.
    post_dsp_.ProcessInPlace(out, frames, channels);
    return true;
  }

private:
  AfxEffect *effect_ = nullptr; // not owned

  // Safety DSP post-chain (DC blocker + limiter). Presence shelf is left
  // disabled for Maxine output (the Studio Voice model already does its own
  // tonal shaping).
  studiocast::audio::dsp::PostDspChain post_dsp_;
};

} // namespace studiocast::maxine::afx
