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
#include "core/maxine/afx/afx_loader_path.h"
#include "core/maxine/afx_api.h"

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

// A stand-in for the AFX SDK. AFX 2.1.0 takes the model path, the input
// sample rate and the input frame size on every microphone effect, and it
// rejects the channel count on all of them. Studio voice also rejects the
// intensity, the effect version and the VAD flag.
namespace fake_afx {

using studiocast::maxine::afx::AfxApi;

constexpr int kInvalidParam = 3; // NVAFX_STATUS_INVALID_PARAM

std::vector<std::string> g_set_order;
std::vector<std::string> g_rejected;
std::uint32_t g_channels_readback = 1;
bool g_have_get_u32 = true;
bool g_loaded = false;

void Reset() {
  g_set_order.clear();
  g_rejected.clear();
  g_channels_readback = 1;
  g_have_get_u32 = true;
  g_loaded = false;
}

bool Rejected(const char *param) {
  for (const auto &r : g_rejected) {
    if (r == param)
      return true;
  }
  return false;
}

int Record(const char *param) {
  g_set_order.push_back(param);
  return Rejected(param) ? kInvalidParam : 0;
}

int CreateEffect(const char *, void **handle) {
  *handle = reinterpret_cast<void *>(0x1);
  return 0;
}
int DestroyEffect(void *) { return 0; }
int SetString(void *, const char *param, const char *) { return Record(param); }
int SetU32(void *, const char *param, std::uint32_t) { return Record(param); }
int SetFloat(void *, const char *param, float) { return Record(param); }
int GetU32(void *, const char *param, std::uint32_t *value) {
  const std::string p = param;
  if (p == "num_input_channels" || p == "num_channels") {
    *value = g_channels_readback;
    return 0;
  }
  return kInvalidParam;
}
int Load(void *) {
  g_loaded = true;
  return 0;
}

// The parameters that AFX 2.1.0 rejects on studio voice.
std::vector<std::string> StudioVoiceRejects() {
  return {"num_input_channels", "num_output_channels",
          "num_channels",       "output_sample_rate",
          "intensity_ratio",    "effect_version",
          "enable_vad",         "num_samples_per_output_frame"};
}

void Install(AfxApi *api) {
  AfxApi::Functions f{};
  f.NvAFX_CreateEffect = &CreateEffect;
  f.NvAFX_DestroyEffect = &DestroyEffect;
  f.NvAFX_SetString = &SetString;
  f.NvAFX_SetU32 = &SetU32;
  f.NvAFX_SetFloat = &SetFloat;
  f.NvAFX_GetU32 = g_have_get_u32 ? &GetU32 : nullptr;
  f.NvAFX_Load = &Load;
  api->SetFunctionsForTesting(f);
}

// Index of the first call for `param`, or -1.
int IndexOf(const std::string &param) {
  for (size_t i = 0; i < g_set_order.size(); ++i) {
    if (g_set_order[i] == param)
      return static_cast<int>(i);
  }
  return -1;
}

} // namespace fake_afx

// Studio voice takes the model, the sample rate and the frame size, and
// nothing else. A parameter that the effect does not take must not stop the
// effect from loading.
bool TestLoadsStudioVoiceWithoutTheOptionalParameters() {
  const fs::path root = TempRoot("params");
  std::error_code ec;
  fs::remove_all(root, ec);

  const fs::path features = root / "Audio_Effects_SDK" / "features";
  if (!MakeFeaturesDir(features)) {
    std::cerr << "failed to build the AFX features directory\n";
    fs::remove_all(root, ec);
    return false;
  }

  fake_afx::Reset();
  fake_afx::g_rejected = fake_afx::StudioVoiceRejects();
  studiocast::maxine::afx::AfxApi api;
  fake_afx::Install(&api);

  studiocast::maxine::afx::AfxEffect fx(&api);
  auto cfg = MakeConfig(features, "studio_voice_low_latency", "studio_voice");
  cfg.vad_enabled = true;
  cfg.effect_version = 2u;

  std::string err;
  const bool configured = fx.Configure(cfg, &err);
  bool ok = Require(configured, "expected studio voice to configure: " + err);
  if (!ok) {
    fs::remove_all(root, ec);
    return false;
  }

  const bool loaded = fx.Load(&err);
  ok &= Require(loaded, "expected studio voice to load: " + err);
  ok &= Require(fake_afx::g_loaded, "expected NvAFX_Load to be called");

  // The model comes first, then the stream format.
  const int model = fake_afx::IndexOf("model_path");
  const int rate = fake_afx::IndexOf("input_sample_rate");
  const int frame = fake_afx::IndexOf("num_samples_per_input_frame");
  ok &= Require(model == 0, "expected model_path first");
  ok &= Require(rate > model, "expected the sample rate after the model");
  ok &= Require(frame > rate, "expected the frame size after the sample rate");

  bool mentions_channels = false;
  for (const auto &w : fx.warnings()) {
    if (w.find("channel") != std::string::npos)
      mentions_channels = true;
  }
  ok &=
      Require(mentions_channels, "expected a warning about the channel count");

  fs::remove_all(root, ec);
  return ok;
}

// The effect runs with a fixed number of channels. When that is not what the
// caller asked for, loading must fail and say so.
bool TestChannelCountMismatchFails() {
  const fs::path root = TempRoot("channels");
  std::error_code ec;
  fs::remove_all(root, ec);

  const fs::path features = root / "Audio_Effects_SDK" / "features";
  if (!MakeFeaturesDir(features)) {
    std::cerr << "failed to build the AFX features directory\n";
    fs::remove_all(root, ec);
    return false;
  }

  fake_afx::Reset();
  fake_afx::g_rejected = fake_afx::StudioVoiceRejects();
  fake_afx::g_channels_readback = 1;
  studiocast::maxine::afx::AfxApi api;
  fake_afx::Install(&api);

  studiocast::maxine::afx::AfxEffect fx(&api);
  auto cfg = MakeConfig(features, "studio_voice_low_latency", "studio_voice");
  cfg.channels = 2;

  std::string err;
  const bool configured = fx.Configure(cfg, &err);
  bool ok = Require(configured, "expected the effect to configure: " + err);
  if (!ok) {
    fs::remove_all(root, ec);
    return false;
  }

  const bool loaded = fx.Load(&err);
  ok &= Require(!loaded, "expected a channel count mismatch to fail");
  ok &= Require(err.find("channel") != std::string::npos,
                "expected the error to name the channel count, got: " + err);

  fs::remove_all(root, ec);
  return ok;
}

// A parameter that every effect needs must still stop the load.
bool TestMissingRequiredParameterFails() {
  const fs::path root = TempRoot("required");
  std::error_code ec;
  fs::remove_all(root, ec);

  const fs::path features = root / "Audio_Effects_SDK" / "features";
  if (!MakeFeaturesDir(features)) {
    std::cerr << "failed to build the AFX features directory\n";
    fs::remove_all(root, ec);
    return false;
  }

  fake_afx::Reset();
  fake_afx::g_rejected = {"input_sample_rate", "sample_rate"};
  studiocast::maxine::afx::AfxApi api;
  fake_afx::Install(&api);

  studiocast::maxine::afx::AfxEffect fx(&api);
  const auto cfg = MakeConfig(features, "denoiser", "denoiser");

  std::string err;
  const bool configured = fx.Configure(cfg, &err);
  bool ok = Require(configured, "expected the denoiser to configure: " + err);
  if (!ok) {
    fs::remove_all(root, ec);
    return false;
  }

  const bool loaded = fx.Load(&err);
  ok &= Require(!loaded, "expected a rejected sample rate to fail");
  ok &= Require(err.find("sample_rate") != std::string::npos,
                "expected the error to name the sample rate, got: " + err);

  fs::remove_all(root, ec);
  return ok;
}

// The AFX core loads a feature library by its bare name, so every
// `<features>/<feature>/lib` directory must be on the loader path.
bool TestFeatureLibDirsAreListed() {
  const fs::path root = TempRoot("libdirs");
  std::error_code ec;
  fs::remove_all(root, ec);

  const fs::path features = root / "Audio_Effects_SDK" / "features";
  if (!MakeFeaturesDir(features)) {
    std::cerr << "failed to build the AFX features directory\n";
    fs::remove_all(root, ec);
    return false;
  }
  // A feature without a lib directory, and the helper files beside them.
  fs::create_directories(features / "voice_font" / "models", ec);
  Touch(features / "download_features.sh");
  Touch(features / "README.md");

  const auto dirs = studiocast::maxine::afx::AfxFeatureLibDirs(features);

  bool ok =
      Require(dirs.size() == 2, "expected 2 feature lib directories, got " +
                                    std::to_string(dirs.size()));
  if (ok) {
    ok &=
        Require(dirs[0] == features / "denoiser" / "lib",
                "expected the denoiser lib dir first, got " + dirs[0].string());
    ok &= Require(dirs[1] == features / "studio_voice" / "lib",
                  "expected the studio voice lib dir second, got " +
                      dirs[1].string());
  }

  ok &= Require(studiocast::maxine::afx::AfxFeatureLibDirs(fs::path()).empty(),
                "expected no directories for an empty features path");

  fs::remove_all(root, ec);
  return ok;
}

// The new LD_LIBRARY_PATH keeps what is there and adds what is missing.
bool TestLoaderPathValue() {
  using studiocast::maxine::afx::LdLibraryPathWithDirs;

  const std::vector<fs::path> dirs = {"/afx/denoiser/lib",
                                      "/afx/studio_voice/lib"};

  bool ok = true;

  const auto from_empty = LdLibraryPathWithDirs("", dirs);
  ok &= Require(from_empty.has_value(), "expected a value for an empty path");
  if (from_empty) {
    ok &= Require(*from_empty == "/afx/denoiser/lib:/afx/studio_voice/lib",
                  "expected both directories, got " + *from_empty);
  }

  const auto keeps = LdLibraryPathWithDirs("/opt/lib", dirs);
  ok &= Require(keeps.has_value(), "expected a value when a directory is new");
  if (keeps) {
    ok &= Require(*keeps == "/afx/denoiser/lib:/afx/studio_voice/lib:/opt/lib",
                  "expected the old value to stay last, got " + *keeps);
  }

  const auto partial =
      LdLibraryPathWithDirs("/afx/denoiser/lib:/opt/lib", dirs);
  ok &= Require(partial.has_value(), "expected a value for a partial path");
  if (partial) {
    ok &= Require(
        *partial == "/afx/studio_voice/lib:/afx/denoiser/lib:/opt/lib",
        "expected only the missing directory to be added, got " + *partial);
  }

  const auto complete =
      LdLibraryPathWithDirs("/afx/studio_voice/lib:/afx/denoiser/lib", dirs);
  ok &= Require(!complete.has_value(),
                "expected nothing to do when every directory is there");

  ok &= Require(!LdLibraryPathWithDirs("/opt/lib", {}).has_value(),
                "expected nothing to do without directories");

  return ok;
}

} // namespace

int main() {
  bool ok = true;
  ok = TestResolvesTheRealAfxLayout() && ok;
  ok = TestResolvesTheVersionedLibrary() && ok;
  ok = TestMissingLibraryKeepsTheClearError() && ok;
  ok = TestLoadsStudioVoiceWithoutTheOptionalParameters() && ok;
  ok = TestChannelCountMismatchFails() && ok;
  ok = TestMissingRequiredParameterFails() && ok;
  ok = TestFeatureLibDirsAreListed() && ok;
  ok = TestLoaderPathValue() && ok;

  if (!ok)
    return 1;

  std::cout << "AFX EFFECT LOOKUP TESTS OK\n";
  return 0;
}
