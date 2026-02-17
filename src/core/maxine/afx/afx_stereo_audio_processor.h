#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "core/audio/audio_processor.h"
#include "core/audio/dsp/post_dsp_chain.h"
#include "core/maxine/afx/afx_effect.h"

namespace studiocast::maxine::afx {

// Stereo (2ch) adapter for Maxine AFX effects that are configured for mono.
//
// Strategy: convert stereo -> Mid/Side, run the effect on Mid only, preserve Side.
// This keeps the speaker path stereo-safe without needing a multi-channel AFX model.
//
// Notes:
//  - Assumes 48kHz / 10ms frames (480 samples).
//  - Fails open: on runtime failures it copies input to output and returns true.
class AfxStereoAudioProcessor final : public studiocast::audio::AudioProcessor {
 public:
  explicit AfxStereoAudioProcessor(AfxEffect* effect) : effect_(effect) {
    std::string derr;
    post_dsp_.Configure(/*sample_rate=*/48000, /*channels=*/2, &derr);
  }

  void Reset() override { post_dsp_.Reset(); }

  bool Process(const float* in,
               float* out,
               std::uint32_t frames,
               std::uint32_t channels,
               std::string* error) override {
    if (!in || !out) {
      if (error) *error = "null audio buffer";
      return false;
    }

    const std::uint64_t samples64 = static_cast<std::uint64_t>(frames) * static_cast<std::uint64_t>(channels);
    const auto samples = static_cast<std::size_t>(samples64);
    if (samples == 0) return true;

    // Only special-case stereo. For other channel counts, fall back to mono-style processing if possible.
    if (channels != 2) {
      if (!effect_) {
        std::copy_n(in, samples, out);
        return true;
      }
      std::string run_err;
      const bool ok = effect_->Run(in, out, static_cast<std::uint32_t>(samples), &run_err);
      if (!ok) {
        if (error && error->empty()) *error = run_err.empty() ? "AFX Run failed" : run_err;
        std::copy_n(in, samples, out);
        return true;
      }
      post_dsp_.ProcessInPlace(out, frames, channels);
      if (error && error->empty()) *error = run_err;
      return true;
    }

    if (!effect_) {
      std::copy_n(in, samples, out);
      return true;
    }

    // Scratch buffers: allocate once (best-effort). If frame size changes unexpectedly,
    // fail open rather than allocating in the real-time thread.
    if (mid_in_.size() < frames || mid_out_.size() < frames || side_.size() < frames) {
      std::copy_n(in, samples, out);
      if (error && error->empty()) *error = "AFX stereo adapter: unexpected frame size";
      return true;
    }

    // Stereo -> mid/side.
    for (std::uint32_t f = 0; f < frames; ++f) {
      const float l = in[static_cast<std::size_t>(f) * 2 + 0];
      const float r = in[static_cast<std::size_t>(f) * 2 + 1];
      mid_in_[f] = 0.5f * (l + r);
      side_[f] = 0.5f * (l - r);
    }

    std::string run_err;
    const bool ok = effect_->Run(mid_in_.data(), mid_out_.data(), frames, &run_err);
    if (!ok) {
      if (error && error->empty()) *error = run_err.empty() ? "AFX Run failed" : run_err;
      std::copy_n(in, samples, out);
      return true;
    }

    // Mid/Side -> stereo.
    for (std::uint32_t f = 0; f < frames; ++f) {
      const float mid = mid_out_[f];
      const float side = side_[f];
      out[static_cast<std::size_t>(f) * 2 + 0] = mid + side;
      out[static_cast<std::size_t>(f) * 2 + 1] = mid - side;
    }

    // Safety post-processing (DC blocker + limiter) on the reconstructed stereo.
    post_dsp_.ProcessInPlace(out, frames, channels);

    if (error && error->empty()) *error = run_err;
    return true;
  }

 private:
  AfxEffect* effect_ = nullptr;  // not owned

  // Scratch buffers sized for 10ms @ 48kHz.
  std::vector<float> mid_in_ = std::vector<float>(480);
  std::vector<float> mid_out_ = std::vector<float>(480);
  std::vector<float> side_ = std::vector<float>(480);

  studiocast::audio::dsp::PostDspChain post_dsp_;
};

}  // namespace studiocast::maxine::afx
