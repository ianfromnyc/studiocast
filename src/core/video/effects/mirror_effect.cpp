#include "mirror_effect.h"

#include "core/video/convert.h"

namespace studiocast::video::effects {

void MirrorEffect::Apply(const Rgb24FrameView &frame, EffectContext * /*ctx*/) {
  if (!frame.Valid())
    return;
  MirrorRgb24InPlace(frame.data, frame.width, frame.height, frame.stride_bytes);
}

} // namespace studiocast::video::effects
