// Tests for the Maxine feature install markers. StudioCast decides that a
// feature is installed from the directory names under `<SDK root>/features`,
// so these tests build such a directory with the names that the SDK really
// installs.

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

#include "core/maxine/maxine_manager.h"

namespace {
namespace fs = std::filesystem;

bool Require(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << message << "\n";
    return false;
  }
  return true;
}

bool Touch(const fs::path &path) {
  std::ofstream out(path, std::ios::out | std::ios::trunc);
  return out.good();
}

// The three entries that every VFX or AR `features` directory holds, with no
// feature installed. `compute_capability` is a helper program, not a feature.
bool MakeBareFeaturesDir(const fs::path &dir) {
  std::error_code ec;
  fs::create_directories(dir, ec);
  if (ec) {
    std::cerr << "failed to create " << dir.string() << "\n";
    return false;
  }
  return Touch(dir / "compute_capability") &&
         Touch(dir / "install_feature.sh") && Touch(dir / "README.md");
}

bool MakeFeaturesDir(const fs::path &dir,
                     const std::vector<std::string> &feature_dirs) {
  if (!MakeBareFeaturesDir(dir))
    return false;
  std::error_code ec;
  for (const auto &name : feature_dirs) {
    fs::create_directories(dir / name / "lib", ec);
    if (ec) {
      std::cerr << "failed to create " << (dir / name).string() << "\n";
      return false;
    }
  }
  return true;
}

fs::path TempRoot(const std::string &name) {
  return fs::temp_directory_path() /
         ("studiocast-maxine-features-" + name + "-" +
          std::to_string(::getpid()));
}

// SDK Core 1.x names every VFX and AR feature directory after its NGC model,
// for example `nvvfxdenoising`. AFX uses the plain effect names.
bool TestSdkOneXFeatureNamesResolve() {
  const fs::path root = TempRoot("1x");
  std::error_code ec;
  fs::remove_all(root, ec);

  const fs::path vfx = root / "VideoFX" / "features";
  const fs::path ar = root / "ARSDK" / "features";
  const fs::path afx = root / "Audio_Effects_SDK" / "features";

  const bool made =
      MakeFeaturesDir(vfx, {"nvvfxaigsrelighting", "nvvfxbackgroundblur",
                            "nvvfxdenoising", "nvvfxgreenscreen",
                            "nvvfxrelighting", "nvvfxtransfer", "nvvfxupscale",
                            "nvvfxvideosuperres"}) &&
      MakeFeaturesDir(ar, {"nvaractivespeakerdetection", "nvarbodydetection",
                           "nvarbodyposeestimation", "nvarfaceboxdetection",
                           "nvarfaceexpressions", "nvargazeredirection",
                           "nvarlandmarkdetection"}) &&
      MakeFeaturesDir(afx, {"aec", "denoiser", "dereverb", "dereverb_denoiser",
                            "studio_voice", "superres"});
  if (!made) {
    fs::remove_all(root, ec);
    return false;
  }

  struct Case {
    const fs::path &dir;
    const char *feature_id;
  };
  const Case cases[] = {
      {vfx, "greenscreen"},    {vfx, "bgblur"},
      {vfx, "denoise"},        {vfx, "relighting"},
      {ar, "gaze_redirection"}, {ar, "face_detection"},
      {ar, "body_detection"},  {afx, "denoiser"},
      {afx, "dereverb"},       {afx, "dereverb_denoiser"},
      {afx, "studio_voice"},
  };

  bool ok = true;
  for (const Case &tc : cases) {
    ok &= Require(
        studiocast::maxine::FeatureMarkerInstalled(tc.dir, tc.feature_id),
        std::string("expected feature '") + tc.feature_id +
            "' to be installed in " + tc.dir.string());
  }

  fs::remove_all(root, ec);
  return ok;
}

// A features directory with the SDK helpers alone installs no feature.
bool TestBareFeaturesDirResolvesNothing() {
  const fs::path root = TempRoot("bare");
  std::error_code ec;
  fs::remove_all(root, ec);

  const fs::path features = root / "VideoFX" / "features";
  if (!MakeBareFeaturesDir(features)) {
    fs::remove_all(root, ec);
    return false;
  }

  const char *feature_ids[] = {
      "greenscreen",      "bgblur",         "denoise",  "relighting",
      "gaze_redirection", "face_detection", "body_detection",
      "denoiser",         "dereverb",       "dereverb_denoiser",
      "studio_voice",
  };

  bool ok = true;
  for (const char *id : feature_ids) {
    ok &= Require(!studiocast::maxine::FeatureMarkerInstalled(features, id),
                  std::string("expected feature '") + id +
                      "' to be missing in a bare features directory");
  }
  ok &= Require(!studiocast::maxine::FeatureMarkerInstalled(
                    root / "does-not-exist", "denoise"),
                "expected a missing features directory to install nothing");
  ok &= Require(!studiocast::maxine::FeatureMarkerInstalled(fs::path(),
                                                            "denoise"),
                "expected an empty features path to install nothing");

  fs::remove_all(root, ec);
  return ok;
}

// One AFX feature must not make its neighbours look installed: the name
// `dereverb_denoiser` holds both `dereverb` and `denoiser`.
bool TestNeighbourNamesDoNotCount() {
  const fs::path root = TempRoot("neighbour");
  std::error_code ec;
  fs::remove_all(root, ec);

  const fs::path features = root / "Audio_Effects_SDK" / "features";
  if (!MakeFeaturesDir(features, {"dereverb_denoiser"})) {
    fs::remove_all(root, ec);
    return false;
  }

  bool ok = true;
  ok &= Require(studiocast::maxine::FeatureMarkerInstalled(features,
                                                           "dereverb_denoiser"),
                "expected 'dereverb_denoiser' to be installed");
  ok &= Require(!studiocast::maxine::FeatureMarkerInstalled(features,
                                                            "dereverb"),
                "expected 'dereverb' to be missing when only "
                "'dereverb_denoiser' is installed");
  ok &= Require(!studiocast::maxine::FeatureMarkerInstalled(features,
                                                            "denoiser"),
                "expected 'denoiser' to be missing when only "
                "'dereverb_denoiser' is installed");

  fs::remove_all(root, ec);
  return ok;
}

} // namespace

int main() {
  bool ok = true;
  ok = TestSdkOneXFeatureNamesResolve() && ok;
  ok = TestBareFeaturesDirResolvesNothing() && ok;
  ok = TestNeighbourNamesDoNotCount() && ok;

  if (!ok)
    return 1;

  std::cout << "MAXINE FEATURE MARKER TESTS OK\n";
  return 0;
}
