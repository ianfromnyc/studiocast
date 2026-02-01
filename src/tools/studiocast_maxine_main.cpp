#include <filesystem>
#include <iostream>
#include <set>
#include <string>

#include "core/config/settings.h"
#include "core/probe/probe.h"
#include "core/util/xdg.h"

namespace fs = std::filesystem;

static void Usage(const char *argv0) {
  std::cout << "StudioCast Maxine Helper\n\n"
            << "Usage:\n"
            << "  " << argv0 << " paths\n"
            << "  " << argv0 << " init\n"
            << "  " << argv0 << " doctor\n"
            << "  " << argv0 << " gpu list\n"
            << "  " << argv0 << " gpu select --auto\n"
            << "  " << argv0 << " gpu select --index <N>\n"
            << "  " << argv0 << " gpu select --uuid <GPU-UUID>\n"
            << "  " << argv0 << " install-hints\n";
}

static void PrintGpus(const studiocast::probe::Report &rep) {
  if (rep.gpus.empty()) {
    std::cout << "No GPUs detected via nvidia-smi.\n";
    return;
  }

  for (const auto &g : rep.gpus) {
    std::cout << "[" << g.index << "] " << g.name;
    if (!g.uuid.empty())
      std::cout << " (" << g.uuid << ")";
    if (g.compute_cap)
      std::cout << " cc " << *g.compute_cap;
    std::cout << (g.likely_supported ? " [supported]" : " [unsupported]");
    if (g.maxine_gpu_arg)
      std::cout << " (maxine --gpu " << *g.maxine_gpu_arg << ")";
    std::cout << "\n";
  }
}

static std::set<std::string>
UniqueMaxineGpuArgs(const studiocast::probe::Report &rep) {
  std::set<std::string> out;
  for (const auto &g : rep.gpus) {
    if (!g.likely_supported)
      continue;
    if (g.maxine_gpu_arg)
      out.insert(*g.maxine_gpu_arg);
  }
  return out;
}

int main(int argc, char **argv) {
  if (argc < 2) {
    Usage(argv[0]);
    return 1;
  }

  const std::string cmd = argv[1];

  const fs::path base = studiocast::util::StudioCastMaxineDir();
  const fs::path vfx = studiocast::util::DefaultVfxRoot();
  const fs::path ar = studiocast::util::DefaultArRoot();
  const fs::path afx = studiocast::util::DefaultAfxRoot();

  if (cmd == "paths") {
    std::cout << "StudioCast Paths\n";
    std::cout << "  Settings: " << studiocast::config::SettingsPath().string()
              << "\n";
    std::cout << "  Maxine base: " << base.string() << "\n";
    std::cout << "  VFX : " << vfx.string() << "\n";
    std::cout << "  AR  : " << ar.string() << "\n";
    std::cout << "  AFX : " << afx.string() << "\n";
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

  if (cmd == "doctor") {
    const auto rep = studiocast::probe::Run(false);
    std::cout << rep.ToText() << "\n";
    return rep.AllChecksPassed() ? 0 : 3;
  }

  if (cmd == "gpu") {
    if (argc < 3) {
      Usage(argv[0]);
      return 1;
    }

    const std::string sub = argv[2];
    if (sub == "list") {
      const auto rep = studiocast::probe::Run(false);
      PrintGpus(rep);
      std::cout << "\nSelected GPU policy: " << rep.gpu_selection_mode << "\n";
      if (rep.selected_gpu_index) {
        std::cout << "Selected GPU index: " << *rep.selected_gpu_index << "\n";
      }
      if (!rep.selected_gpu_uuid.empty()) {
        std::cout << "Selected GPU uuid: " << rep.selected_gpu_uuid << "\n";
      }
      return 0;
    }

    if (sub == "select") {
      studiocast::config::Settings s;

      // Default: keep existing settings then modify.
      s = studiocast::config::LoadSettings();

      bool changed = false;
      for (int i = 3; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--auto") {
          s.gpu.mode = studiocast::config::GpuSelectMode::Auto;
          s.gpu.index.reset();
          s.gpu.uuid.clear();
          changed = true;
        } else if (arg == "--index" && i + 1 < argc) {
          s.gpu.mode = studiocast::config::GpuSelectMode::Index;
          s.gpu.index = std::atoi(argv[i + 1]);
          s.gpu.uuid.clear();
          changed = true;
          ++i;
        } else if (arg == "--uuid" && i + 1 < argc) {
          s.gpu.mode = studiocast::config::GpuSelectMode::Uuid;
          s.gpu.uuid = argv[i + 1];
          s.gpu.index.reset();
          changed = true;
          ++i;
        }
      }

      if (!changed) {
        std::cerr << "No selection provided.\n";
        return 2;
      }

      std::string err;
      if (!studiocast::config::SaveSettings(s, &err)) {
        std::cerr << "Failed to save settings: " << err << "\n";
        return 3;
      }

      std::cout << "Saved GPU selection to: "
                << studiocast::config::SettingsPath().string() << "\n";
      const auto rep = studiocast::probe::Run(false);
      std::cout << "\n" << rep.ToText() << "\n";
      return rep.AllChecksPassed() ? 0 : 4;
    }

    Usage(argv[0]);
    return 1;
  }

  if (cmd == "install-hints") {
    const auto rep = studiocast::probe::Run(false);
    const auto args = UniqueMaxineGpuArgs(rep);

    std::cout << "StudioCast Maxine Install Hints\n\n";
    std::cout << "Maxine base:\n  " << base.string() << "\n\n";

    std::cout << "GPU policy:\n";
    std::cout << "  settings: " << studiocast::config::SettingsPath().string()
              << "\n";
    std::cout << "  mode: " << rep.gpu_selection_mode << "\n";
    if (rep.selected_gpu_index)
      std::cout << "  selected index: " << *rep.selected_gpu_index << "\n";
    if (!rep.selected_gpu_uuid.empty())
      std::cout << "  selected uuid: " << rep.selected_gpu_uuid << "\n\n";

    std::cout << "Detected GPUs:\n";
    PrintGpus(rep);
    std::cout << "\n";

    std::cout << "VFX core (extract so that '" << vfx.string()
              << "' exists):\n";
    std::cout << "  mkdir -p \"" << base.string() << "\"\n";
    std::cout << "  tar -xvf NVIDIA_VFX_SDK_linux_<version>.tar.gz -C \""
              << base.string() << "\"\n\n";

    std::cout << "AR core (extract so that '" << ar.string() << "' exists):\n";
    std::cout << "  mkdir -p \"" << base.string() << "\"\n";
    std::cout << "  tar -xvf NVIDIA_AR_SDK_linux_<version>.tar.gz -C \""
              << base.string() << "\"\n\n";

    std::cout << "AFX core (create '" << afx.string() << "'):\n";
    std::cout << "  mkdir -p \"" << base.string() << "\"\n";
    std::cout << "  cd \"" << base.string() << "\"\n";
    std::cout << "  tar xvf --one-top-level Audio_Effects_SDK.tar.gz\n\n";

    if (args.empty()) {
      std::cout << "VFX/AR feature install:\n";
      std::cout
          << "  No supported GPUs with known --gpu mapping were detected.\n";
      std::cout << "  Run this on a Tensor Core GPU machine (Turing+).\n\n";
    } else {
      std::cout
          << "VFX/AR feature install (run once per unique --gpu value):\n";
      std::cout << "  export NGC_CLI_API_KEY=\"<your_api_key>\"\n";
      for (const auto &a : args) {
        std::cout << "  cd \"" << vfx.string()
                  << "/features\" && ./install_feature.sh --gpu " << a
                  << " --feature all --ngc-org nvidia --ngc-team maxine\n";
        std::cout << "  cd \"" << ar.string()
                  << "/features\" && ./install_feature.sh --gpu " << a
                  << " --feature all --ngc-org nvidia --ngc-team maxine\n";
      }
      std::cout << "\n";
    }

    std::cout << "AFX features:\n";
    std::cout << "  export NGC_API_KEY=\"<your_api_key>\"\n";
    std::cout << "  cd \"" << afx.string()
              << "/features\" && ./download_features.sh\n";

    return 0;
  }

  Usage(argv[0]);
  return 1;
}
