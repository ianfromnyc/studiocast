#include <filesystem>
#include <iostream>
#include <string>
#include <system_error>

#include "core/maxine/gpu.h"
#include "core/probe/probe.h"
#include "core/util/xdg.h"

namespace fs = std::filesystem;

static void Usage(const char* argv0) {
  std::cout
      << "StudioCast Maxine Helper\n"
      << "Usage:\n"
      << "  " << argv0 << " paths\n"
      << "  " << argv0 << " init\n"
      << "  " << argv0 << " install-hints\n"
      << "  " << argv0 << " doctor\n";
}

static std::string SuggestVfxArGpuArg() {
  const auto rep = studiocast::probe::Run(false);

  for (const auto& g : rep.gpus) {
    if (!g.compute_cap) continue;
    if (auto arg = studiocast::maxine::MaxineGpuArgFromComputeCap(*g.compute_cap)) {
      return *arg;
    }
  }
  return {};
}

int main(int argc, char** argv) {
  if (argc < 2) {
    Usage(argv[0]);
    return 1;
  }

  const std::string cmd = argv[1];

  const fs::path base = studiocast::util::StudioCastMaxineDir();
  const fs::path vfx  = studiocast::util::DefaultVfxRoot();
  const fs::path ar   = studiocast::util::DefaultArRoot();
  const fs::path afx  = studiocast::util::DefaultAfxRoot();

  if (cmd == "paths") {
    std::cout << "StudioCast Maxine Paths\n";
    std::cout << "  Base: " << base.string() << "\n";
    std::cout << "  VFX : " << vfx.string() << "\n";
    std::cout << "  AR  : " << ar.string() << "\n";
    std::cout << "  AFX : " << afx.string() << "\n";
    std::cout << "\nOverrides:\n";
    std::cout << "  STUDIOCAST_VFX_SDK_ROOT\n";
    std::cout << "  STUDIOCAST_AR_SDK_ROOT\n";
    std::cout << "  AFX_SDK_ROOT\n";
    return 0;
  }

  if (cmd == "init") {
    std::error_code ec;
    fs::create_directories(base, ec);
    if (ec) {
      std::cerr << "Failed to create: " << base.string() << "\n";
      std::cerr << "Error: " << ec.message() << "\n";
      return 2;
    }

    std::cout << "Created (or already existed): " << base.string() << "\n";
    std::cout << "Expected SDK roots:\n";
    std::cout << "  " << vfx.string() << "\n";
    std::cout << "  " << ar.string() << "\n";
    std::cout << "  " << afx.string() << "\n";
    return 0;
  }

  if (cmd == "install-hints") {
    const std::string gpuArg = SuggestVfxArGpuArg();

    std::cout << "StudioCast Maxine Install Hints\n\n";
    std::cout << "Base dir:\n  " << base.string() << "\n\n";

    std::cout << "VFX core (extract so that '" << vfx.string() << "' exists):\n";
    std::cout << "  mkdir -p \"" << base.string() << "\"\n";
    std::cout << "  tar -xvf NVIDIA_VFX_SDK_linux_<version>.tar.gz -C \"" << base.string() << "\"\n\n";

    std::cout << "AR core (extract so that '" << ar.string() << "' exists):\n";
    std::cout << "  mkdir -p \"" << base.string() << "\"\n";
    std::cout << "  tar -xvf NVIDIA_AR_SDK_linux_<version>.tar.gz -C \"" << base.string() << "\"\n\n";

    std::cout << "AFX core (create '" << afx.string() << "'):\n";
    std::cout << "  mkdir -p \"" << base.string() << "\"\n";
    std::cout << "  cd \"" << base.string() << "\"\n";
    std::cout << "  tar xvf --one-top-level Audio_Effects_SDK.tar.gz\n\n";

    std::cout << "Install VFX/AR features (NGC_CLI_API_KEY required):\n";
    if (!gpuArg.empty()) {
      std::cout << "  # Suggested for this machine: --gpu " << gpuArg << "\n";
    } else {
      std::cout << "  # Suggested: run `studiocast-probe` on a Tensor Core GPU to get a --gpu value.\n";
    }
    std::cout << "  export NGC_CLI_API_KEY=\"<your_api_key>\"\n";
    std::cout << "  cd \"" << vfx.string() << "/features\" && ./install_feature.sh --gpu "
              << (gpuArg.empty() ? "<gpu>" : gpuArg)
              << " --feature all --ngc-org nvidia --ngc-team maxine\n";
    std::cout << "  cd \"" << ar.string() << "/features\" && ./install_feature.sh --gpu "
              << (gpuArg.empty() ? "<gpu>" : gpuArg)
              << " --feature all --ngc-org nvidia --ngc-team maxine\n\n";

    std::cout << "Install AFX features (NGC_API_KEY required):\n";
    std::cout << "  export NGC_API_KEY=\"<your_api_key>\"\n";
    std::cout << "  cd \"" << afx.string() << "/features\" && ./download_features.sh\n";

    return 0;
  }

  if (cmd == "doctor") {
    const auto rep = studiocast::probe::Run(false);
    std::cout << rep.ToText() << "\n";
    return rep.AllChecksPassed() ? 0 : 3;
  }

  Usage(argv[0]);
  return 1;
}
