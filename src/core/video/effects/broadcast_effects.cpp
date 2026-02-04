#include "broadcast_effects.h"

#include <algorithm>

namespace studiocast::video::effects {
namespace {

std::string ToLowerAscii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        if (c >= 'A' && c <= 'Z') return static_cast<char>(c - 'A' + 'a');
        return static_cast<char>(c);
    });
    return s;
}

}  // namespace

std::string ToString(EffectsEnginePreference v) {
    switch (v) {
        case EffectsEnginePreference::auto_select:
            return "auto";
        case EffectsEnginePreference::maxine:
            return "maxine";
    }
    return "auto";
}

bool ParseEffectsEnginePreference(const std::string& s, EffectsEnginePreference* out) {
    if (!out) return false;
    const auto v = ToLowerAscii(s);

    if (v.empty() || v == "auto" || v == "default") {
        *out = EffectsEnginePreference::auto_select;
        return true;
    }
    if (v == "maxine" || v == "gpu" || v == "nvidia") {
        *out = EffectsEnginePreference::maxine;
        return true;
    }

    return false;
}

std::string ToString(VirtualBackgroundMode v) {
    switch (v) {
        case VirtualBackgroundMode::none:
            return "none";
        case VirtualBackgroundMode::blur:
            return "blur";
        case VirtualBackgroundMode::remove:
            return "remove";
        case VirtualBackgroundMode::replace:
            return "replace";
    }
    return "none";
}

bool ParseVirtualBackgroundMode(const std::string& s, VirtualBackgroundMode* out) {
    if (!out) return false;
    const auto v = ToLowerAscii(s);

    if (v.empty() || v == "none" || v == "off" || v == "disabled") {
        *out = VirtualBackgroundMode::none;
        return true;
    }
    if (v == "blur" || v == "background_blur" || v == "bg_blur") {
        *out = VirtualBackgroundMode::blur;
        return true;
    }
    if (v == "remove" || v == "removal" || v == "background_remove" || v == "bg_remove") {
        *out = VirtualBackgroundMode::remove;
        return true;
    }
    if (v == "replace" || v == "background_replace" || v == "bg_replace") {
        *out = VirtualBackgroundMode::replace;
        return true;
    }

    return false;
}

}  // namespace studiocast::video::effects
