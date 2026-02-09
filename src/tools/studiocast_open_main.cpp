#include <filesystem>
#include <iostream>
#include <string>

#include "core/open_cuda/model_pack_registry.h"
#include "core/util/xdg.h"

namespace fs = std::filesystem;

namespace {

std::string DefaultOpenCudaRootHint() {
  return "~/.local/share/studiocast/models/open_cuda";
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

static void Usage(const char* argv0) {
  std::cout << "StudioCast Open CUDA Helper\n\n"
            << "Usage:\n"
            << "  " << argv0 << " paths\n"
            << "  " << argv0 << " list-models\n"
            << "  " << argv0 << " install-hints\n";
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

  Usage(argv[0]);
  return 1;
}
