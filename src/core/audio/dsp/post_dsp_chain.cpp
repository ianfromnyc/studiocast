#include "core/audio/dsp/post_dsp_chain.h"

#include <algorithm>
#include <cmath>

namespace studiocast::audio::dsp {

namespace {

constexpr double kPi = 3.141592653589793238462643383279502884;

float ClampFloat(float x, float lo, float hi) {
  if (x < lo)
    return lo;
  if (x > hi)
    return hi;
  return x;
}

} // namespace

PostDspChain::Biquad PostDspChain::MakeHighShelfBiquad(int sample_rate,
                                                       float freq_hz,
                                                       float gain_db,
                                                       float slope) {
  // RBJ cookbook high-shelf.
  PostDspChain::Biquad bq;
  if (sample_rate <= 0)
    return bq;
  if (freq_hz <= 0.0f)
    return bq;

  const float fs = static_cast<float>(sample_rate);
  const float f = ClampFloat(freq_hz, 10.0f, 0.49f * fs);
  const float S = ClampFloat(slope, 0.1f, 5.0f);

  const float A = std::pow(10.0f, gain_db / 40.0f);
  const double w0 =
      (2.0 * kPi) * static_cast<double>(f) / static_cast<double>(fs);
  const double cosw0 = std::cos(w0);
  const double sinw0 = std::sin(w0);
  const double sqrtA = std::sqrt(static_cast<double>(A));

  // alpha = sin(w0)/2 * sqrt( (A + 1/A) * (1/S - 1) + 2 )
  const double alpha =
      (sinw0 / 2.0) *
      std::sqrt((static_cast<double>(A) + 1.0 / static_cast<double>(A)) *
                    (1.0 / static_cast<double>(S) - 1.0) +
                2.0);

  const double Ap1 = static_cast<double>(A) + 1.0;
  const double Am1 = static_cast<double>(A) - 1.0;

  const double b0 =
      static_cast<double>(A) * (Ap1 + Am1 * cosw0 + 2.0 * sqrtA * alpha);
  const double b1 = -2.0 * static_cast<double>(A) * (Am1 + Ap1 * cosw0);
  const double b2 =
      static_cast<double>(A) * (Ap1 + Am1 * cosw0 - 2.0 * sqrtA * alpha);
  const double a0 = Ap1 - Am1 * cosw0 + 2.0 * sqrtA * alpha;
  const double a1 = 2.0 * (Am1 - Ap1 * cosw0);
  const double a2 = Ap1 - Am1 * cosw0 - 2.0 * sqrtA * alpha;

  if (a0 == 0.0)
    return bq;

  bq.b0 = static_cast<float>(b0 / a0);
  bq.b1 = static_cast<float>(b1 / a0);
  bq.b2 = static_cast<float>(b2 / a0);
  bq.a1 = static_cast<float>(a1 / a0);
  bq.a2 = static_cast<float>(a2 / a0);
  return bq;
}

bool PostDspChain::Configure(int sample_rate, std::uint32_t channels,
                             std::string *error) {
  if (error)
    error->clear();
  if (sample_rate <= 0) {
    if (error)
      *error = "PostDspChain: sample_rate must be > 0";
    return false;
  }
  if (channels == 0) {
    if (error)
      *error = "PostDspChain: channels must be > 0";
    return false;
  }
  sample_rate_ = sample_rate;
  channels_ = channels;

  dc_prev_x_.assign(channels_, 0.0f);
  dc_prev_y_.assign(channels_, 0.0f);

  presence_z1_.assign(channels_, 0.0f);
  presence_z2_.assign(channels_, 0.0f);

  limiter_gain_ = 1.0f;

  // Force recompute.
  presence_cfg_cached_ = PresenceShelfConfig{};
  RecomputePresenceShelfIfNeeded();
  return true;
}

void PostDspChain::Reset() {
  std::fill(dc_prev_x_.begin(), dc_prev_x_.end(), 0.0f);
  std::fill(dc_prev_y_.begin(), dc_prev_y_.end(), 0.0f);
  std::fill(presence_z1_.begin(), presence_z1_.end(), 0.0f);
  std::fill(presence_z2_.begin(), presence_z2_.end(), 0.0f);
  limiter_gain_ = 1.0f;
}

void PostDspChain::SetPresenceShelf(const PresenceShelfConfig &cfg) {
  presence_cfg_ = cfg;
  RecomputePresenceShelfIfNeeded();
}

void PostDspChain::SetLimiter(const LimiterConfig &cfg) { limiter_cfg_ = cfg; }

void PostDspChain::RecomputePresenceShelfIfNeeded() {
  // Avoid recomputing coefficients unless needed.
  const bool changed =
      (presence_cfg_.enabled != presence_cfg_cached_.enabled) ||
      (presence_cfg_.freq_hz != presence_cfg_cached_.freq_hz) ||
      (presence_cfg_.gain_db != presence_cfg_cached_.gain_db) ||
      (presence_cfg_.slope != presence_cfg_cached_.slope);
  if (!changed)
    return;

  presence_cfg_cached_ = presence_cfg_;

  if (!presence_cfg_.enabled || std::fabs(presence_cfg_.gain_db) < 0.001f) {
    presence_biquad_ = Biquad{};
    return;
  }

  presence_biquad_ =
      MakeHighShelfBiquad(sample_rate_, presence_cfg_.freq_hz,
                          presence_cfg_.gain_db, presence_cfg_.slope);
}

void PostDspChain::ProcessInPlace(float *interleaved, std::uint32_t frames,
                                  std::uint32_t channels) {
  if (!interleaved)
    return;
  if (frames == 0 || channels == 0)
    return;
  if (sample_rate_ <= 0 || channels_ == 0)
    return;
  if (channels != channels_) {
    // Unexpected format change. Avoid allocations in the RT thread;
    // fall back to a no-op rather than partially processing channels.
    return;
  }

  // --- Stage 1: DC blocker + optional presence shelf.
  float peak = 0.0f;

  const bool presence_on =
      presence_cfg_.enabled && (std::fabs(presence_cfg_.gain_db) >= 0.001f);

  const std::size_t stride = static_cast<std::size_t>(channels);
  for (std::uint32_t i = 0; i < frames; ++i) {
    float *frame = interleaved + static_cast<std::size_t>(i) * stride;
    for (std::uint32_t ch = 0; ch < channels; ++ch) {
      float x = frame[ch];

      // DC blocker.
      float y = x - dc_prev_x_[ch] + dc_block_r_ * dc_prev_y_[ch];
      dc_prev_x_[ch] = x;
      dc_prev_y_[ch] = y;

      // Presence shelf (biquad, transposed DF1).
      if (presence_on) {
        const auto &bq = presence_biquad_;
        float out = bq.b0 * y + presence_z1_[ch];
        presence_z1_[ch] = bq.b1 * y - bq.a1 * out + presence_z2_[ch];
        presence_z2_[ch] = bq.b2 * y - bq.a2 * out;
        y = out;
      }

      frame[ch] = y;
      const float a = std::fabs(y);
      if (a > peak)
        peak = a;
    }
  }

  // --- Stage 2: safety limiter.
  if (limiter_cfg_.enabled) {
    const float thr = ClampFloat(limiter_cfg_.threshold, 0.1f, 0.9999f);
    const float target_gain = (peak > thr && peak > 0.0f) ? (thr / peak) : 1.0f;

    // Smooth gain per frame.
    const float frame_sec =
        static_cast<float>(frames) / static_cast<float>(sample_rate_);
    const float attack_sec =
        std::max(0.0001f, limiter_cfg_.attack_ms / 1000.0f);
    const float release_sec =
        std::max(0.0001f, limiter_cfg_.release_ms / 1000.0f);
    const float alpha_attack = std::exp(-frame_sec / attack_sec);
    const float alpha_release = std::exp(-frame_sec / release_sec);

    const float alpha =
        (target_gain < limiter_gain_) ? alpha_attack : alpha_release;
    limiter_gain_ = alpha * limiter_gain_ + (1.0f - alpha) * target_gain;

    // Apply gain + safety clamp.
    const std::size_t n = static_cast<std::size_t>(frames) * stride;
    for (std::size_t idx = 0; idx < n; ++idx) {
      float v = interleaved[idx] * limiter_gain_;
      // Clamp to [-1, 1] as a final safety measure.
      if (v > 1.0f)
        v = 1.0f;
      if (v < -1.0f)
        v = -1.0f;
      interleaved[idx] = v;
    }
  }
}

} // namespace studiocast::audio::dsp
