#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <iomanip>
#include <string>
#include <string_view>
#include <vector>

#include "core/audio/effects/broadcast_audio_effects.h"
#include "core/open_audio/model_pack_registry.h"
#include "core/open_audio/open_audio_audio_processor.h"
#include "core/open_audio/open_audio_onnx_session.h"
#include "core/open_cuda/model_pack_registry.h"
#include "core/util/xdg.h"

namespace fs = std::filesystem;

namespace {

std::string DefaultOpenCudaRootHint() {
  return "~/.local/share/studiocast/models/open_cuda";
}

std::string DefaultOpenAudioRootHint() {
  return "~/.local/share/studiocast/models/open_audio";
}

fs::path OpenCudaRootPath() {
  const auto modelsRoot = studiocast::util::StudioCastModelsDir();
  if (modelsRoot.empty()) return {};
  return modelsRoot / "open_cuda";
}

std::string OpenCudaRootForDisplay() {
  const auto p = OpenCudaRootPath();
  return p.empty() ? DefaultOpenCudaRootHint() : p.string();
}

fs::path OpenAudioRootPath() {
  const auto modelsRoot = studiocast::util::StudioCastModelsDir();
  if (modelsRoot.empty()) return {};
  return modelsRoot / "open_audio";
}

std::string OpenAudioRootForDisplay() {
  const auto p = OpenAudioRootPath();
  return p.empty() ? DefaultOpenAudioRootHint() : p.string();
}

static void Usage(const char* argv0) {
  std::cout << "StudioCast Open Model Helper\n\n"
            << "Usage:\n"
            << "  " << argv0 << " paths\n"
            << "  " << argv0 << " list-models\n"
            << "  " << argv0 << " install-hints\n"
            << "\n"
            << "  " << argv0 << " audio-paths\n"
            << "  " << argv0 << " audio-list-models\n"
            << "  " << argv0 << " audio-install-hints\n"
            << "  " << argv0
            << " audio-self-test [--model-id <id>] [--model-path <path>] [--cpu-only]\n"
            << "  " << argv0
            << " audio-bench [--effect <noise_removal|room_echo_removal|noise_echo_removal|studio_voice|speaker_noise_removal>]"
               " [--strength <0-100>] [--seconds <N>] [--frames <N>] [--warmup <N>] [--model-id <id>] [--model-path <path>]"
               " [--cpu-only] [--csv]\n";
}

bool HasArg(int argc, char** argv, std::string_view flag) {
  for (int i = 1; i < argc; ++i) {
    if (argv[i] && std::string_view(argv[i]) == flag) return true;
  }
  return false;
}

std::string GetArgValue(int argc, char** argv, std::string_view key) {
  for (int i = 1; i + 1 < argc; ++i) {
    if (argv[i] && std::string_view(argv[i]) == key) {
      return argv[i + 1] ? std::string(argv[i + 1]) : "";
    }
  }
  return "";
}

static int CmdPaths() {
  std::cout << "StudioCast Paths (Open CUDA)\n";
  std::cout << "  Open CUDA models root: " << OpenCudaRootForDisplay() << "\n";
  std::cout << "\nExpected model pack layout:\n";
  std::cout << "  " << OpenCudaRootForDisplay() << "/<model_id>/model.json\n";
  std::cout << "  " << OpenCudaRootForDisplay() << "/<model_id>/model.onnx\n";
  std::cout << "  " << OpenCudaRootForDisplay() << "/<model_id>/LICENSE.txt\n";
  return 0;
}

static int CmdListModels() {
  std::cout << "StudioCast Open CUDA Model Packs\n\n";
  std::cout << "Scan root:\n  " << OpenCudaRootForDisplay() << "\n\n";

  const auto reg = studiocast::open_cuda::ModelPackRegistry::ScanDefault();
  const auto& models = reg.ListModels();
  const auto& problems = reg.Problems();

  if (models.empty()) {
    std::cout << "Valid model packs: (none)\n";
  } else {
    std::cout << "Valid model packs:\n";
    for (const auto& m : models) {
      std::cout << "  - " << m.id;
      if (!m.display_name.empty()) std::cout << " (" << m.display_name << ")";
      if (!m.task.empty()) std::cout << " task=" << m.task;
      std::cout << "\n";
      std::cout << "      manifest: " << m.manifest_path.string() << "\n";
      std::cout << "      onnx    : " << m.onnx_path.string() << "\n";
      if (m.license_path) std::cout << "      license : " << m.license_path->string() << "\n";
    }
  }

  std::cout << "\n";

  if (problems.empty()) {
    std::cout << "Problems: (none)\n";
  } else {
    std::cout << "Problems:\n";
    for (const auto& [key, reason] : problems) {
      std::cout << "  - " << key << ": " << reason << "\n";
    }
  }

  return 0;
}

static int CmdInstallHints(const char* argv0) {
  std::cout << "StudioCast Open CUDA Install Hints\n\n";
  std::cout << "Model packs root:\n  " << OpenCudaRootForDisplay() << "\n\n";

  std::cout << "A model pack is a directory named by <model_id>.\n";
  std::cout << "Required files:\n";
  std::cout << "  - model.json   (metadata)\n";
  std::cout << "  - model.onnx   (ONNX model)\n";
  std::cout << "  - LICENSE.txt  (model license text)\n\n";

  std::cout << "Create a new pack (example):\n";
  std::cout << "  mkdir -p \"" << OpenCudaRootForDisplay() << "/modnet\"\n";
  std::cout << "  cp /path/to/model.onnx \"" << OpenCudaRootForDisplay() << "/modnet/model.onnx\"\n";
  std::cout << "  cp /path/to/model.json \"" << OpenCudaRootForDisplay() << "/modnet/model.json\"\n";
  std::cout << "  cp /path/to/LICENSE.txt \"" << OpenCudaRootForDisplay() << "/modnet/LICENSE.txt\"\n\n";

  std::cout << "Validate discovery:\n";
  std::cout << "  " << argv0 << " list-models\n";

  return 0;
}

static int CmdAudioPaths() {
  std::cout << "StudioCast Paths (Open Audio)\n";
  std::cout << "  Open Audio models root: " << OpenAudioRootForDisplay() << "\n";
  std::cout << "\nExpected model pack layout:\n";
  std::cout << "  " << OpenAudioRootForDisplay() << "/<model_id>/model.json\n";
  std::cout << "  " << OpenAudioRootForDisplay() << "/<model_id>/model.onnx\n";
  std::cout << "  " << OpenAudioRootForDisplay() << "/<model_id>/LICENSE.txt\n";
  return 0;
}

static int CmdAudioListModels() {
  std::cout << "StudioCast Open Audio Model Packs\n\n";
  std::cout << "Scan root:\n  " << OpenAudioRootForDisplay() << "\n\n";

  const auto reg = studiocast::open_audio::ModelPackRegistry::ScanDefault();
  const auto& models = reg.ListModels();
  const auto& problems = reg.Problems();

  if (models.empty()) {
    std::cout << "Valid model packs: (none)\n";
  } else {
    std::cout << "Valid model packs:\n";
    for (const auto& m : models) {
      std::cout << "  - " << m.id;
      if (!m.display_name.empty()) std::cout << " (" << m.display_name << ")";
      if (!m.effects.empty()) {
        std::cout << " effects=";
        for (std::size_t i = 0; i < m.effects.size(); ++i) {
          if (i) std::cout << ",";
          std::cout << m.effects[i];
        }
      }
      std::cout << " sr=" << m.sample_rate << " ch=" << m.channels << "\n";
      std::cout << "      manifest: " << m.manifest_path.string() << "\n";
      std::cout << "      onnx    : " << m.onnx_path.string() << "\n";
      if (m.license_path) std::cout << "      license : " << m.license_path->string() << "\n";
    }
  }

  std::cout << "\n";

  if (problems.empty()) {
    std::cout << "Problems: (none)\n";
  } else {
    std::cout << "Problems:\n";
    for (const auto& [key, reason] : problems) {
      std::cout << "  - " << key << ": " << reason << "\n";
    }
  }

  return 0;
}

static int CmdAudioInstallHints(const char* argv0) {
  std::cout << "StudioCast Open Audio Install Hints\n\n";
  std::cout << "Model packs root:\n  " << OpenAudioRootForDisplay() << "\n\n";

  std::cout << "Quick install (source builds):\n";
  std::cout << "  ./scripts/install_open_audio_models.sh\n\n";
  std::cout << "Curated pack IDs: fastenhancer_s_vd_v1, fastenhancer_m_vd_v1, fastenhancer_l_vd_v1\n";
  std::cout << "Docs: docs/open_audio_install.md\n\n";

  std::cout << "A model pack is a directory named by <model_id>.\n";
  std::cout << "Required files:\n";
  std::cout << "  - model.json   (metadata)\n";
  std::cout << "  - <model>.onnx (ONNX model)\n";
  std::cout << "  - LICENSE.txt  (model license text)\n\n";

  std::cout << "Create a new pack (example):\n";
  std::cout << "  mkdir -p \"" << OpenAudioRootForDisplay() << "/my_audio_model\"\n";
  std::cout << "  cp /path/to/model.onnx \"" << OpenAudioRootForDisplay()
            << "/my_audio_model/model.onnx\"\n";
  std::cout << "  cp /path/to/model.json \"" << OpenAudioRootForDisplay()
            << "/my_audio_model/model.json\"\n";
  std::cout << "  cp /path/to/LICENSE.txt \"" << OpenAudioRootForDisplay()
            << "/my_audio_model/LICENSE.txt\"\n\n";

  std::cout << "Validate discovery:\n";
  std::cout << "  " << argv0 << " audio-list-models\n";

  std::cout << "\nValidate ONNX Runtime session creation:\n";
  std::cout << "  " << argv0 << " audio-self-test --model-id my_audio_model\n";

  return 0;
}

static int CmdAudioSelfTest(int argc, char** argv) {
  std::cout << "StudioCast Open Audio Self-Test\n\n";

  const bool cpu_only = HasArg(argc, argv, "--cpu-only");
  const std::string model_id = GetArgValue(argc, argv, "--model-id");
  const std::string model_path = GetArgValue(argc, argv, "--model-path");

#if !STUDIOCAST_ENABLE_OPEN_AUDIO
  std::cerr << "ERROR: Open Audio backend is disabled in this build (STUDIOCAST_ENABLE_OPEN_AUDIO=0).\n";
  return 2;
#elif !STUDIOCAST_HAVE_ONNXRUNTIME
  std::cerr << "ERROR: This build was compiled without ONNX Runtime (STUDIOCAST_HAVE_ONNXRUNTIME=0).\n";
  std::cerr << "Rebuild with ONNX Runtime available (set ONNXRUNTIME_ROOT or install onnxruntime dev package).\n";
  return 2;
#else
  const auto ort = studiocast::open_audio::OpenAudioOrtSession::QueryRuntimeInfo();
  std::cout << "ONNX Runtime version: " << (ort.version.empty() ? "(unknown)" : ort.version) << "\n";
  if (ort.providers.empty()) {
    std::cout << "Available providers: (unknown)\n";
  } else {
    std::cout << "Available providers:\n";
    for (const auto& p : ort.providers) {
      std::cout << "  - " << p << "\n";
    }
  }

  studiocast::audio::effects::BroadcastAudioEffects fx;
  fx.microphone.model_id = model_id;
  fx.microphone.model_path = model_path;

  studiocast::open_audio::ResolvedOpenAudioModel resolved;
  std::string resolve_err;
  if (!studiocast::open_audio::ResolveOpenAudioModelForMicrophone(fx, &resolved, &resolve_err)) {
    std::cerr << "ERROR: Failed to resolve Open Audio model: "
              << (resolve_err.empty() ? "unknown error" : resolve_err) << "\n";
    std::cerr << "Tip: run '" << argv[0] << " audio-list-models' to see installed packs.\n";
    return 2;
  }

  std::cout << "\nResolved model:\n";
  std::cout << "  id          : " << (resolved.model_id.empty() ? "(user_path)" : resolved.model_id) << "\n";
  std::cout << "  display_name: " << resolved.display_name << "\n";
  std::cout << "  onnx_path   : " << resolved.onnx_path.string() << "\n";

  studiocast::open_audio::OrtSessionOptions opts;
  opts.prefer_cuda = !cpu_only;
  opts.cuda_device_id = 0;

  studiocast::open_audio::OrtSessionInfo si;
  std::string err;
  auto session = studiocast::open_audio::OpenAudioOrtSession::Create(resolved.onnx_path, opts, &si, &err);
  if (!session) {
    std::cerr << "ERROR: " << (err.empty() ? "Failed to create ONNX Runtime session" : err) << "\n";
    return 2;
  }

  std::cout << "\nSession created successfully. Provider: " << (si.using_cuda ? "CUDA" : "CPU") << "\n";

  std::cout << "\nInputs:\n";
  for (std::size_t i = 0; i < si.input_names.size(); ++i) {
    const std::string name = si.input_names[i];
    const std::string desc = i < si.input_descriptions.size() ? si.input_descriptions[i] : "";
    std::cout << "  - " << (name.empty() ? "<unnamed>" : name) << ": " << desc << "\n";
  }

  std::cout << "\nOutputs:\n";
  for (std::size_t i = 0; i < si.output_names.size(); ++i) {
    const std::string name = si.output_names[i];
    const std::string desc = i < si.output_descriptions.size() ? si.output_descriptions[i] : "";
    std::cout << "  - " << (name.empty() ? "<unnamed>" : name) << ": " << desc << "\n";
  }

  return 0;
#endif
}

static int CmdAudioBench(int argc, char** argv) {
  std::cout << "StudioCast Open Audio Bench\n\n";

  const bool cpu_only = HasArg(argc, argv, "--cpu-only");
  const bool csv = HasArg(argc, argv, "--csv");
  const std::string model_id = GetArgValue(argc, argv, "--model-id");
  const std::string model_path = GetArgValue(argc, argv, "--model-path");
  const std::string effect = GetArgValue(argc, argv, "--effect");

  int strength = 60;
  if (const std::string strength_s = GetArgValue(argc, argv, "--strength"); !strength_s.empty()) {
    try {
      strength = std::stoi(strength_s);
    } catch (...) {
      std::cerr << "ERROR: Invalid --strength value: " << strength_s << "\n";
      return 2;
    }
  }
  if (strength < 0) strength = 0;
  if (strength > 100) strength = 100;

  int warmup = 50;
  if (const std::string warmup_s = GetArgValue(argc, argv, "--warmup"); !warmup_s.empty()) {
    try {
      warmup = std::stoi(warmup_s);
    } catch (...) {
      std::cerr << "ERROR: Invalid --warmup value: " << warmup_s << "\n";
      return 2;
    }
  }
  if (warmup < 0) warmup = 0;

  int frames = 0;
  if (const std::string frames_s = GetArgValue(argc, argv, "--frames"); !frames_s.empty()) {
    try {
      frames = std::stoi(frames_s);
    } catch (...) {
      std::cerr << "ERROR: Invalid --frames value: " << frames_s << "\n";
      return 2;
    }
  } else if (const std::string seconds_s = GetArgValue(argc, argv, "--seconds"); !seconds_s.empty()) {
    try {
      const double seconds = std::stod(seconds_s);
      frames = static_cast<int>(seconds * 100.0 + 0.5);  // 10ms frames.
    } catch (...) {
      std::cerr << "ERROR: Invalid --seconds value: " << seconds_s << "\n";
      return 2;
    }
  } else {
    frames = 500;  // 5 seconds @ 10ms.
  }

  if (frames <= 0) {
    std::cerr << "ERROR: frames must be > 0.\n";
    return 2;
  }

#if !STUDIOCAST_ENABLE_OPEN_AUDIO
  std::cerr << "ERROR: Open Audio backend is disabled in this build (STUDIOCAST_ENABLE_OPEN_AUDIO=0).\n";
  return 2;
#elif !STUDIOCAST_HAVE_ONNXRUNTIME
  std::cerr << "ERROR: This build was compiled without ONNX Runtime (STUDIOCAST_HAVE_ONNXRUNTIME=0).\n";
  std::cerr << "Rebuild with ONNX Runtime available (set ONNXRUNTIME_ROOT or install onnxruntime dev package).\n";
  return 2;
#else
  std::string effect_kind = effect;
  if (effect_kind.empty()) effect_kind = "noise_removal";

  bool speaker = false;

  studiocast::audio::effects::BroadcastAudioEffects fx;
  fx.engine = studiocast::audio::effects::AudioEffectsEnginePreference::kOpenSource;

  if (effect_kind == "speaker_noise_removal" || effect_kind == "speaker") {
    speaker = true;
    fx.speaker.noise_removal_enabled = true;
    fx.speaker.strength = strength;
    fx.speaker.model_id = model_id;
    fx.speaker.model_path = model_path;
  } else if (effect_kind == "room_echo_removal" || effect_kind == "echo") {
    fx.microphone.room_echo_removal_enabled = true;
    fx.microphone.strength = strength;
    fx.microphone.model_id = model_id;
    fx.microphone.model_path = model_path;
  } else if (effect_kind == "noise_echo_removal" || effect_kind == "noise+echo") {
    fx.microphone.noise_removal_enabled = true;
    fx.microphone.room_echo_removal_enabled = true;
    fx.microphone.strength = strength;
    fx.microphone.model_id = model_id;
    fx.microphone.model_path = model_path;
  } else if (effect_kind == "studio_voice" || effect_kind == "studio") {
    fx.microphone.studio_voice_enabled = true;
    fx.microphone.strength = strength;
    fx.microphone.model_id = model_id;
    fx.microphone.model_path = model_path;
  } else if (effect_kind == "noise_removal" || effect_kind == "noise") {
    fx.microphone.noise_removal_enabled = true;
    fx.microphone.strength = strength;
    fx.microphone.model_id = model_id;
    fx.microphone.model_path = model_path;
  } else {
    std::cerr << "ERROR: Unknown --effect: " << effect_kind << "\n";
    std::cerr << "Allowed: noise_removal, room_echo_removal, noise_echo_removal, studio_voice, speaker_noise_removal\n";
    return 2;
  }

  std::cout << "Effect        : " << effect_kind << "\n";
  std::cout << "Strength      : " << strength << "\n";
  std::cout << "Frames        : " << frames << "\n";
  std::cout << "Warmup frames : " << warmup << "\n";
  std::cout << "Provider pref : " << (cpu_only ? "CPU-only" : "CUDA (fallback to CPU)") << "\n\n";

  const auto ort = studiocast::open_audio::OpenAudioOrtSession::QueryRuntimeInfo();
  std::cout << "ONNX Runtime version: " << (ort.version.empty() ? "(unknown)" : ort.version) << "\n";
  if (!ort.providers.empty()) {
    std::cout << "Available providers:\n";
    for (const auto& p : ort.providers) {
      std::cout << "  - " << p << "\n";
    }
  }
  std::cout << "\n";

  studiocast::open_audio::OrtSessionOptions opts;
  opts.prefer_cuda = !cpu_only;
  opts.cuda_device_id = 0;

  studiocast::open_audio::ResolvedOpenAudioModel resolved;
  std::string err;

  std::unique_ptr<studiocast::open_audio::OpenAudioAudioProcessor> processor;
  if (speaker) {
    processor = studiocast::open_audio::OpenAudioAudioProcessor::CreateForSpeakerWithOrtOptions(
        fx, opts, &resolved, &err);
  } else {
    processor = studiocast::open_audio::OpenAudioAudioProcessor::CreateForMicrophoneWithOrtOptions(
        fx, opts, &resolved, &err);
  }

  if (!processor) {
    std::cerr << "ERROR: Failed to create Open Audio processor: " << (err.empty() ? "unknown error" : err) << "\n";
    std::cerr << "Tip: run '" << argv[0] << " audio-list-models' to see installed packs.\n";
    return 2;
  }

  std::cout << "Resolved model:\n";
  std::cout << "  id          : " << (resolved.model_id.empty() ? "(user_path)" : resolved.model_id) << "\n";
  std::cout << "  display_name: " << resolved.display_name << "\n";
  std::cout << "  onnx_path   : " << resolved.onnx_path.string() << "\n";
  std::cout << "  sample_rate : " << resolved.sample_rate << "\n";
  std::cout << "  channels    : " << resolved.channels << "\n\n";
  
  constexpr std::uint32_t kFrameSamples = 480;
  const std::uint32_t kChannels = speaker ? 2 : 1;
  constexpr std::uint64_t kBudgetUs = 10000;

  std::vector<float> in(kFrameSamples * kChannels);
  std::vector<float> out(kFrameSamples * kChannels);

  // Deterministic synthetic input: sine + a touch of noise.
  std::uint32_t rng = 0x12345678u;
  double phase = 0.0;
  const double phase_inc = 2.0 * 3.14159265358979323846 * 220.0 / 48000.0;

  auto fill_frame = [&]() {
    for (std::size_t i = 0; i < in.size(); ++i) {
      rng = rng * 1664525u + 1013904223u;
      const float n = static_cast<float>((rng >> 9) & 0x7FFFFF) / static_cast<float>(0x7FFFFF);
      const float noise = (n * 2.0f - 1.0f) * 0.015f;
      const float s = static_cast<float>(std::sin(phase)) * 0.12f;
      phase += phase_inc;
      if (phase > 2.0 * 3.14159265358979323846) phase -= 2.0 * 3.14159265358979323846;
      in[i] = s + noise;
    }
  };

  // Warmup (allocator / kernel cache).
  processor->Reset();
  for (int i = 0; i < warmup; ++i) {
    fill_frame();
    std::string perr;
    (void)processor->Process(in.data(), out.data(), kFrameSamples, kChannels, &perr);
  }

  std::vector<std::uint64_t> us;
  us.reserve(static_cast<std::size_t>(frames));

  std::uint64_t over_budget = 0;
  for (int i = 0; i < frames; ++i) {
    fill_frame();
    const auto t0 = std::chrono::steady_clock::now();
    std::string perr;
    const bool ok = processor->Process(in.data(), out.data(), kFrameSamples, kChannels, &perr);
    const auto t1 = std::chrono::steady_clock::now();

    if (!ok) {
      std::cerr << "ERROR: Process failed at frame " << i << ": " << perr << "\n";
      return 3;
    }

    const std::uint64_t dt_us =
        static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count());
    us.push_back(dt_us);
    if (dt_us > kBudgetUs) ++over_budget;
  }

  std::uint64_t sum_us = 0;
  std::uint64_t max_us = 0;
  for (auto v : us) {
    sum_us += v;
    if (v > max_us) max_us = v;
  }

  std::vector<std::uint64_t> sorted = us;
  std::sort(sorted.begin(), sorted.end());
  auto pct = [&](double p) -> std::uint64_t {
    if (sorted.empty()) return 0;
    const double idx = (p / 100.0) * (static_cast<double>(sorted.size() - 1));
    const std::size_t i = static_cast<std::size_t>(idx + 0.5);
    return sorted[std::min(i, sorted.size() - 1)];
  };

  const double mean_ms = static_cast<double>(sum_us) / static_cast<double>(us.size()) / 1000.0;
  const double p50_ms = static_cast<double>(pct(50.0)) / 1000.0;
  const double p90_ms = static_cast<double>(pct(90.0)) / 1000.0;
  const double p95_ms = static_cast<double>(pct(95.0)) / 1000.0;
  const double p99_ms = static_cast<double>(pct(99.0)) / 1000.0;
  const double max_ms = static_cast<double>(max_us) / 1000.0;

  std::cout << std::fixed << std::setprecision(3);
  std::cout << "Results (per 10ms @ 48kHz):\n";
  std::cout << "  mean : " << mean_ms << " ms\n";
  std::cout << "  p50  : " << p50_ms << " ms\n";
  std::cout << "  p90  : " << p90_ms << " ms\n";
  std::cout << "  p95  : " << p95_ms << " ms\n";
  std::cout << "  p99  : " << p99_ms << " ms\n";
  std::cout << "  max  : " << max_ms << " ms\n";
  std::cout << "  budget (" << (kBudgetUs / 1000.0) << " ms) overruns: " << over_budget << " / " << us.size()
            << "\n";

  if (csv) {
    std::cout << "\nframe,process_us\n";
    for (std::size_t i = 0; i < us.size(); ++i) {
      std::cout << i << "," << us[i] << "\n";
    }
  }

  return 0;
#endif
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    Usage(argv[0]);
    return 1;
  }

  const std::string cmd = argv[1];
  if (cmd == "--help" || cmd == "-h" || cmd == "help") {
    Usage(argv[0]);
    return 0;
  }

  if (cmd == "paths") return CmdPaths();
  if (cmd == "list-models") return CmdListModels();
  if (cmd == "install-hints") return CmdInstallHints(argv[0]);

  if (cmd == "audio-paths") return CmdAudioPaths();
  if (cmd == "audio-list-models") return CmdAudioListModels();
  if (cmd == "audio-install-hints") return CmdAudioInstallHints(argv[0]);
  if (cmd == "audio-self-test") return CmdAudioSelfTest(argc, argv);
  if (cmd == "audio-bench") return CmdAudioBench(argc, argv);

  Usage(argv[0]);
  return 1;
}
