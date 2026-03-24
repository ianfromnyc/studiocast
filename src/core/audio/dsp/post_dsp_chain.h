#pragma once

#include <cstdint>

#include <string>
#include <vector>

namespace studiocast::audio::dsp {

// A lightweight, dependency-free "polish" DSP stage intended to make ML effects
// safer and more consistent in real-time:
//  - DC blocker / rumble high-pass
//  - optional gentle presence shelf (for Studio Voice mode)
//  - safety limiter (prevents clipping)
//
// The chain is designed to be real-time safe:
//  - no allocations during ProcessInPlace()
//  - state is preallocated in Configure()
class PostDspChain {
public:
  struct LimiterConfig {
    bool enabled = true;
    // Target peak ceiling. Keep slightly below 1.0f to avoid hard clipping.
    float threshold = 0.98f;
    // Attack/release smoothing of the gain value (in milliseconds).
    float attack_ms = 5.0f;
    float release_ms = 100.0f;
  };

  struct PresenceShelfConfig {
    bool enabled = false;
    float freq_hz = 3200.0f;
    // Positive gain boosts presence. Suggested range: 0..+3 dB.
    float gain_db = 0.0f;
    // Slope (Q-ish) control. 1.0 is a reasonable default.
    float slope = 1.0f;
  };

  PostDspChain() = default;

  // Configure internal buffers/state for the given format.
  // Returns false on invalid inputs.
  bool Configure(int sample_rate, std::uint32_t channels, std::string *error);

  void Reset();

  // Optional presence shelf. If gain_db is ~0 or disabled=false, this stage is
  // bypassed.
  void SetPresenceShelf(const PresenceShelfConfig &cfg);

  // Limiter configuration.
  void SetLimiter(const LimiterConfig &cfg);

  // Process an interleaved buffer in-place.
  // Buffer length is frames * channels.
  void ProcessInPlace(float *interleaved, std::uint32_t frames,
                      std::uint32_t channels);

private:
  struct Biquad {
    // Direct-form 1 transposed coefficients.
    float b0 = 1.0f;
    float b1 = 0.0f;
    float b2 = 0.0f;
    float a1 = 0.0f;
    float a2 = 0.0f;
  };

  static Biquad MakeHighShelfBiquad(int sample_rate, float freq_hz,
                                    float gain_db, float slope);

  void RecomputePresenceShelfIfNeeded();

  int sample_rate_ = 0;
  std::uint32_t channels_ = 0;

  // DC blocker: y[n] = x[n] - x[n-1] + R*y[n-1]
  // Using R close to 1 gives a low cutoff frequency.
  float dc_block_r_ = 0.999f;
  std::vector<float> dc_prev_x_;
  std::vector<float> dc_prev_y_;

  // Presence shelf filter.
  PresenceShelfConfig presence_cfg_;
  PresenceShelfConfig presence_cfg_cached_;
  Biquad presence_biquad_;
  std::vector<float> presence_z1_;
  std::vector<float> presence_z2_;

  // Limiter.
  LimiterConfig limiter_cfg_;
  float limiter_gain_ = 1.0f;
};

} // namespace studiocast::audio::dsp
