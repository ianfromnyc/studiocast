// Tests for the AFX feature library and model lookup. AFX 2.1.0 names the
// library after the effect selector when a feature holds more than one effect
// (studio voice), and it ships `.so`, `.so.2` and `.so.2.1.0` for each
// library. These tests build such a features directory and check what
// AfxEffect::Configure resolves.

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unistd.h>
#include <vector>

#include "core/maxine/afx/afx_effect.h"

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

// One AFX library, as the SDK installs it: `libnv_audiofx_<name>.so` and the
// two version links beside it.
bool MakeLibrary(const fs::path &lib_dir, const std::string &name) {
  std::error_code ec;
  fs::create_directories(lib_dir, ec);
  const fs::path real = lib_dir / ("libnv_audiofx_" + name + ".so.2.1.0");
  if (ec || !Touch(real))
    return false;
  fs::create_symlink(real.filename(),
                     lib_dir / ("libnv_audiofx_" + name + ".so.2"), ec);
  if (ec)
    return false;
  fs::create_symlink(real.filename(),
                     lib_dir / ("libnv_audiofx_" + name + ".so"), ec);
  return !ec;
}

// One AFX model, as the SDK installs it: the file carries the frame size, and
// a link without it points at the file.
bool MakeModel(const fs::path &model_dir, const std::string &stem,
               const std::string &suffix) {
  std::error_code ec;
  fs::create_directories(model_dir, ec);
  const fs::path real = model_dir / (stem + "_" + suffix + ".trtpkg");
  if (ec || !Touch(real))
    return false;
  fs::create_symlink(real.filename(), model_dir / (stem + ".trtpkg"), ec);
  return !ec;
}

// The AFX 2.1.0 features directory, as far as these tests need it.
bool MakeFeaturesDir(const fs::path &features) {
  const fs::path sv = features / "studio_voice";
  const fs::path dn = features / "denoiser";
  return MakeLibrary(sv / "lib", "studio_voice_low_latency") &&
         MakeLibrary(sv / "lib", "studio_voice_high_quality") &&
         MakeModel(sv / "models" / "sm_86", "studio_voice_low_latency_48k",
                   "1") &&
         MakeModel(sv / "models" / "sm_86", "studio_voice_high_quality_48k",
                   "1") &&
         MakeLibrary(dn / "lib", "denoiser") &&
         MakeModel(dn / "models" / "sm_86", "denoiser_48k", "3072") &&
         MakeModel(dn / "models" / "sm_86", "denoiser_v2_48k", "1920");
}

fs::path TempRoot(const std::string &name) {
  return fs::temp_directory_path() /
         ("studiocast-afx-effect-" + name + "-" + std::to_string(::getpid()));
}

studiocast::maxine::afx::AfxEffectConfig
MakeConfig(const fs::path &features, const std::string &selector,
           const std::string &feature_id) {
  studiocast::maxine::afx::AfxEffectConfig cfg;
  cfg.effect_selector = selector;
  cfg.feature_id = feature_id;
  cfg.features_dir = features;
  cfg.compute_capability = std::make_pair(8, 6);
  cfg.sample_rate = 48000;
  cfg.frame_samples = 480;
  cfg.channels = 1;
  cfg.intensity = 0.5f;
  return cfg;
}

bool TestResolvesTheRealAfxLayout() {
  const fs::path root = TempRoot("layout");
  std::error_code ec;
  fs::remove_all(root, ec);

  const fs::path features = root / "Audio_Effects_SDK" / "features";
  if (!MakeFeaturesDir(features)) {
    std::cerr << "failed to build the AFX features directory\n";
    fs::remove_all(root, ec);
    return false;
  }

  struct Case {
    const char *name;
    const char *selector;
    const char *feature_id;
    bool denoiser_v2;
    fs::path want_lib;
    fs::path want_model;
  };

  const fs::path sv = features / "studio_voice";
  const fs::path dn = features / "denoiser";
  const std::vector<Case> cases = {
      {"studio voice, low latency", "studio_voice_low_latency", "studio_voice",
       false, sv / "lib" / "libnv_audiofx_studio_voice_low_latency.so",
       sv / "models" / "sm_86" / "studio_voice_low_latency_48k.trtpkg"},
      {"studio voice, high quality", "studio_voice_high_quality",
       "studio_voice", false,
       sv / "lib" / "libnv_audiofx_studio_voice_high_quality.so",
       sv / "models" / "sm_86" / "studio_voice_high_quality_48k.trtpkg"},
      {"denoiser", "denoiser", "denoiser", false,
       dn / "lib" / "libnv_audiofx_denoiser.so",
       dn / "models" / "sm_86" / "denoiser_48k.trtpkg"},
      {"denoiser v2", "denoiser", "denoiser", true,
       dn / "lib" / "libnv_audiofx_denoiser.so",
       dn / "models" / "sm_86" / "denoiser_v2_48k.trtpkg"},
  };

  bool ok = true;
  for (const Case &tc : cases) {
    studiocast::maxine::afx::AfxEffect fx(nullptr);
    auto cfg = MakeConfig(features, tc.selector, tc.feature_id);
    cfg.use_denoiser_v2_model = tc.denoiser_v2;

    std::string err;
    const bool configured = fx.Configure(cfg, &err);
    if (!Require(configured, std::string("expected ") + tc.name +
                                 " to configure: " + err)) {
      ok = false;
      continue;
    }
    ok &= Require(fx.resolved_feature_lib_path() == tc.want_lib,
                  std::string("expected ") + tc.name + " to use " +
                      tc.want_lib.string() + ", got " +
                      fx.resolved_feature_lib_path().string());
    ok &= Require(fx.resolved_model_path() == tc.want_model,
                  std::string("expected ") + tc.name + " to use " +
                      tc.want_model.string() + ", got " +
                      fx.resolved_model_path().string());
  }

  fs::remove_all(root, ec);
  return ok;
}

// AFX 2.1.0 ships no `.so.1`. A tree that keeps the version links alone must
// still resolve.
bool TestResolvesTheVersionedLibrary() {
  const fs::path root = TempRoot("versioned");
  std::error_code ec;
  fs::remove_all(root, ec);

  const fs::path features = root / "Audio_Effects_SDK" / "features";
  const fs::path lib_dir = features / "denoiser" / "lib";
  fs::create_directories(lib_dir, ec);
  const bool made = !ec && Touch(lib_dir / "libnv_audiofx_denoiser.so.2") &&
                    MakeModel(features / "denoiser" / "models" / "sm_86",
                              "denoiser_48k", "3072");
  if (!made) {
    std::cerr << "failed to build the versioned AFX library\n";
    fs::remove_all(root, ec);
    return false;
  }

  studiocast::maxine::afx::AfxEffect fx(nullptr);
  const auto cfg = MakeConfig(features, "denoiser", "denoiser");
  std::string err;

  const bool configured = fx.Configure(cfg, &err);
  bool ok = Require(configured,
                    "expected the versioned library to configure: " + err);
  if (ok) {
    ok &= Require(fx.resolved_feature_lib_path() ==
                      lib_dir / "libnv_audiofx_denoiser.so.2",
                  "expected libnv_audiofx_denoiser.so.2, got " +
                      fx.resolved_feature_lib_path().string());
  }

  fs::remove_all(root, ec);
  return ok;
}

// A feature with no library must still give the clear message, and it must
// name every library that the lookup tried.
bool TestMissingLibraryKeepsTheClearError() {
  const fs::path root = TempRoot("missing");
  std::error_code ec;
  fs::remove_all(root, ec);

  const fs::path features = root / "Audio_Effects_SDK" / "features";
  const fs::path lib_dir = features / "studio_voice" / "lib";
  fs::create_directories(lib_dir, ec);
  const bool made =
      !ec && MakeModel(features / "studio_voice" / "models" / "sm_86",
                       "studio_voice_low_latency_48k", "1");
  if (!made) {
    std::cerr << "failed to build the AFX feature without a library\n";
    fs::remove_all(root, ec);
    return false;
  }

  studiocast::maxine::afx::AfxEffect fx(nullptr);
  const auto cfg =
      MakeConfig(features, "studio_voice_low_latency", "studio_voice");
  std::string err;

  const bool configured = fx.Configure(cfg, &err);
  bool ok =
      Require(!configured, "expected a feature without a library to fail");
  ok &= Require(err.find("AFX feature library not found") != std::string::npos,
                "expected the clear message, got: " + err);
  ok &= Require(err.find("libnv_audiofx_studio_voice_low_latency.so") !=
                    std::string::npos,
                "expected the message to name the effect library, got: " + err);
  ok &=
      Require(err.find("libnv_audiofx_studio_voice.so") != std::string::npos,
              "expected the message to name the feature library, got: " + err);
  ok &= Require(err.find(lib_dir.string()) != std::string::npos,
                "expected the message to name the lib directory, got: " + err);

  fs::remove_all(root, ec);
  return ok;
}

} // namespace

int main() {
  bool ok = true;
  ok = TestResolvesTheRealAfxLayout() && ok;
  ok = TestResolvesTheVersionedLibrary() && ok;
  ok = TestMissingLibraryKeepsTheClearError() && ok;

  if (!ok)
    return 1;

  std::cout << "AFX EFFECT LOOKUP TESTS OK\n";
  return 0;
}
