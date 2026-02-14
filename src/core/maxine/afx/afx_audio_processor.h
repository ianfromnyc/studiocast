#pragma once

#include <cstdint>
#include <limits>
#include <string>

#include "core/audio/audio_processor.h"
#include "core/maxine/afx/afx_effect.h"

namespace studiocast::maxine::afx {

// Adapter to run a Maxine `AfxEffect` through the generic `AudioProcessor` pipeline.
class AfxAudioProcessor final : public studiocast::audio::AudioProcessor {
public:
    explicit AfxAudioProcessor(AfxEffect* effect) : effect_(effect) {}

    bool Process(const float* in,
                 float* out,
                 std::uint32_t frames,
                 std::uint32_t channels,
                 std::string* error) override {
        if (!effect_) {
            if (error) *error = "AFX effect is null";
            return false;
        }

        const std::uint64_t samples64 = static_cast<std::uint64_t>(frames) * static_cast<std::uint64_t>(channels);
        if (samples64 > std::numeric_limits<std::uint32_t>::max()) {
            if (error) *error = "frame is too large";
            return false;
        }
        return effect_->Run(in, out, static_cast<std::uint32_t>(samples64), error);
    }

private:
    AfxEffect* effect_ = nullptr;  // not owned
};

}  // namespace studiocast::maxine::afx
