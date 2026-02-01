#pragma once

#include <filesystem>

namespace studiocast::util {

std::filesystem::path HomeDir();
std::filesystem::path XdgDataHome();
std::filesystem::path XdgConfigHome();

std::filesystem::path
StudioCastDataDir(); // ~/.local/share/studiocast (by default)
std::filesystem::path
StudioCastConfigDir(); // ~/.config/studiocast (by default)
std::filesystem::path
StudioCastMaxineDir(); // ~/.local/share/studiocast/maxine (by default)

// Canonical Maxine roots in the user-local layout
std::filesystem::path DefaultVfxRoot(); // .../maxine/VideoFX
std::filesystem::path DefaultArRoot();  // .../maxine/ARSDK
std::filesystem::path DefaultAfxRoot(); // .../maxine/Audio_Effects_SDK

std::filesystem::path XdgStateHome();
std::filesystem::path StudioCastStateDir();

} // namespace studiocast::util
