#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

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
            << " audio-self-test [--model-id <id>] [--model-path <path>] [--cpu-only]\n";
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

  std::cout << "A model pack is a directory named by <model_id>.\n";
  std::cout << "Required files:\n";
  std::cout << "  - model.json   (metadata)\n";
  std::cout << "  - model.onnx   (ONNX model)\n";
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

  Usage(argv[0]);
  return 1;
}
