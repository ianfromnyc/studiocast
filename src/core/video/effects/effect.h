#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// NOTE: This is a CPU-only prototype effect interface operating on RGB24
// frames. The production path for StudioCast camera effects will use GPU/Maxine
// effects (see `core/maxine/effects/maxine_effect.h`) operating on
// `studiocast::video::GpuFrame`.
namespace studiocast::video::effects {

struct Rgb24FrameView {
  std::uint8_t *data = nullptr;
  int width = 0;
  int height = 0;
  std::size_t stride_bytes = 0; // bytes per row

  bool Valid() const {
    return data && width > 0 && height > 0 &&
           stride_bytes >= static_cast<std::size_t>(width) * 3u;
  }
};

// Per-pipeline effect context. Effects can use this for scratch buffers if they
// want, but each effect is also free to manage its own scratch memory.
struct EffectContext {
  // A single shared scratch buffer to avoid repeated allocations.
  // Effects should Resize and use it as needed.
  std::vector<std::uint8_t> scratch;
};

class IVideoEffect {
public:
  virtual ~IVideoEffect() = default;

  // Stable identifier used in config / IPC.
  virtual const char *Id() const = 0;

  // Human-friendly display name (GUI can override / localize later).
  virtual const char *DisplayName() const = 0;

  // Applies the effect in-place to an RGB24 frame.
  virtual void Apply(const Rgb24FrameView &frame, EffectContext *ctx) = 0;

  // Backend label for status/debug (e.g. "cpu", "maxine").
  virtual const char *Backend() const { return "cpu"; }
};

} // namespace studiocast::video::effects
