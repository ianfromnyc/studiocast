#include "core/maxine/vfx_api.h"

#include "core/util/xdg.h"

#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

namespace studiocast::maxine::vfx {

namespace fs = std::filesystem;

namespace {

struct SharedLibLoadResult {
  util::DynLib lib;
  fs::path path;
};

std::vector<fs::path> CandidateLibDirs(const fs::path &root) {
  std::vector<fs::path> dirs;
  dirs.push_back(root);
  dirs.push_back(root / "lib");
  dirs.push_back(root / "lib64");
  dirs.push_back(root / "bin");
  // Some SDK extractions place libs under lib/x86_64-linux-gnu
  dirs.push_back(root / "lib" / "x86_64-linux-gnu");
  dirs.push_back(root / "lib64" / "x86_64-linux-gnu");
  return dirs;
}

bool LooksLikeVfxLibName(const fs::path &p) {
  const auto s = p.filename().string();
  return s.rfind("libVideoFX.so", 0) == 0 ||
         s.rfind("libnvVideoEffects.so", 0) == 0 ||
         s.rfind("libNVVideoEffects.so", 0) == 0 ||
         s.rfind("libnvvfx.so", 0) == 0 ||
         s.rfind("libNvVFX.so", 0) == 0;
}

std::optional<SharedLibLoadResult>
FindLibWithSymbol(const std::vector<fs::path> &lib_dirs,
                  const std::vector<std::string> &preferred_names,
                  const char *required_symbol, util::DynLib::Scope scope,
                  std::string *last_error) {
  std::error_code ec;

  auto try_file =
      [&](const fs::path &full) -> std::optional<SharedLibLoadResult> {
    SharedLibLoadResult res;
    res.path = full;

    std::string err;
    if (!res.lib.Open(full, scope, &err)) {
      if (last_error) {
        *last_error = err;
      }
      return std::nullopt;
    }

    if (!res.lib.GetSymbolRaw(required_symbol, &err)) {
      if (last_error) {
        *last_error = err;
      }
      return std::nullopt;
    }

    return res;
  };

  // 0) Try bare names via the system loader path.
  for (const auto &name : preferred_names) {
    if (auto r = try_file(fs::path(name))) {
      return r;
    }
  }

  // 1) Try preferred names under candidate directories.
  for (const auto &dir : lib_dirs) {
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) {
      continue;
    }
    for (const auto &name : preferred_names) {
      const auto full = dir / name;
      if (fs::exists(full, ec)) {
        if (auto r = try_file(full)) {
          return r;
        }
      }
    }
  }

  // 2) Scan for versioned variants of the known VFX library names.
  for (const auto &dir : lib_dirs) {
    if (!fs::exists(dir, ec) || !fs::is_directory(dir, ec)) {
      continue;
    }
    for (const auto &entry : fs::directory_iterator(dir, ec)) {
      if (ec) {
        break;
      }
      if (!entry.is_regular_file(ec)) {
        continue;
      }
      const auto p = entry.path();
      if (!LooksLikeVfxLibName(p)) {
        continue;
      }
      if (auto r = try_file(p)) {
        return r;
      }
    }
  }

  return std::nullopt;
}

} // namespace

VfxApi::VfxApi() = default;

VfxApi::~VfxApi() = default;

VfxApi::VfxApi(VfxApi &&) noexcept = default;

VfxApi &VfxApi::operator=(VfxApi &&) noexcept = default;

bool VfxApi::Initialize(std::string *error_out) {
  const std::vector<fs::path> roots = {
      util::DefaultVfxRoot(),
      fs::path("/usr/local/VideoFX"),
  };
  return Initialize(roots, error_out);
}

bool VfxApi::Initialize(const std::vector<std::filesystem::path> &sdk_roots,
                        std::string *error_out) {
  initialized_ = false;
  library_path_.clear();
  lib_.Close();
  cuda_lib_.Close();
  f_ = Functions{};
  cuda_ = CudaFunctions{};
  cuda_loaded_ = false;
  error_.clear();

  return InitializeImpl(sdk_roots, error_out);
}

bool VfxApi::InitializeFromLibraryPath(
    const std::filesystem::path &library_path, std::string *error_out) {
  initialized_ = false;
  library_path_.clear();
  lib_.Close();
  cuda_lib_.Close();
  f_ = Functions{};
  cuda_ = CudaFunctions{};
  cuda_loaded_ = false;
  error_.clear();

  return InitializeFromLibraryPathImpl(library_path, error_out);
}

bool VfxApi::InitializeImpl(const std::vector<std::filesystem::path> &sdk_roots,
                            std::string *error_out) {
  std::error_code ec;

  for (const auto &root : sdk_roots) {
    if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) {
      continue;
    }

    const auto models = root / "models";
    if (!fs::exists(models, ec) || !fs::is_directory(models, ec)) {
      continue;
    }

    std::string last;
    const std::vector<std::string> preferred = {
        "libVideoFX.so",
        "libnvVideoEffects.so",
        "libNVVideoEffects.so",
        "libnvvfx.so",
        "libNvVFX.so",
    };

    const auto dirs = CandidateLibDirs(root);
    auto found = FindLibWithSymbol(dirs, preferred, "NvVFX_CreateEffect",
                                   util::DynLib::Scope::Global, &last);
    if (!found) {
      std::ostringstream oss;
      oss << "Maxine VFX library not found under SDK root: " << root.string()
          << ".";
      if (!last.empty()) {
        oss << " loader error: " << last;
      }
      error_ = oss.str();
      continue;
    }

    lib_ = std::move(found->lib);
    library_path_ = found->path;

    std::string err;
    if (!LoadSymbols(&err)) {
      error_ = err;
      lib_.Close();
      library_path_.clear();
      continue;
    }

    initialized_ = true;
    return true;
  }

  if (error_.empty()) {
    error_ = "Maxine VideoFX SDK not found (missing models directory).";
  }

  if (error_out) {
    *error_out = error_;
  }
  return false;
}

bool VfxApi::InitializeFromLibraryPathImpl(
    const std::filesystem::path &library_path, std::string *error_out) {
  std::string err;
  if (!lib_.Open(library_path, util::DynLib::Scope::Global, &err)) {
    error_ = err;
    if (error_out) {
      *error_out = error_;
    }
    return false;
  }

  library_path_ = library_path;
  if (!LoadSymbols(&err)) {
    error_ = err;
    if (error_out) {
      *error_out = error_;
    }
    return false;
  }

  initialized_ = true;
  return true;
}

bool VfxApi::LoadSymbols(std::string *error_out) {
  auto load_required = [&](const char *symbol, auto *out) -> bool {
    std::string err;
    if (!lib_.GetSymbol(symbol, out, &err)) {
      if (error_out) {
        *error_out = err;
      }
      return false;
    }
    return true;
  };

  auto load_optional = [&](const char *symbol, auto *out) {
    lib_.GetSymbol(symbol, out, nullptr);
  };

  if (!load_required("NvVFX_CreateEffect", &f_.NvVFX_CreateEffect) ||
      !load_required("NvVFX_DestroyEffect", &f_.NvVFX_DestroyEffect) ||
      !load_required("NvVFX_Load", &f_.NvVFX_Load) ||
      !load_required("NvVFX_Run", &f_.NvVFX_Run) ||
      !load_required("NvVFX_SetImage", &f_.NvVFX_SetImage) ||
      !load_required("NvVFX_SetString", &f_.NvVFX_SetString) ||
      !load_required("NvVFX_GetString", &f_.NvVFX_GetString) ||
      !load_required("NvVFX_SetF32", &f_.NvVFX_SetF32) ||
      !load_required("NvVFX_SetU32", &f_.NvVFX_SetU32) ||
      !load_required("NvVFX_SetS32", &f_.NvVFX_SetS32) ||
      !load_required("NvVFX_GetU32", &f_.NvVFX_GetU32) ||
      !load_required("NvVFX_GetS32", &f_.NvVFX_GetS32) ||
      !load_required("NvVFX_GetF32", &f_.NvVFX_GetF32) ||
      !load_required("NvVFX_SetObject", &f_.NvVFX_SetObject) ||
      !load_required("NvVFX_GetObject", &f_.NvVFX_GetObject) ||
      !load_required("NvVFX_AllocateState", &f_.NvVFX_AllocateState) ||
      !load_required("NvVFX_DeallocateState", &f_.NvVFX_DeallocateState) ||
      !load_required("NvVFX_ResetState", &f_.NvVFX_ResetState) ||
      !load_required("NvVFX_SetStateObjectHandleArray",
                     &f_.NvVFX_SetStateObjectHandleArray) ||
      !load_required("NvVFX_CudaStreamCreate", &f_.NvVFX_CudaStreamCreate) ||
      !load_required("NvVFX_CudaStreamDestroy", &f_.NvVFX_CudaStreamDestroy) ||
      !load_required("NvVFX_CudaStreamSynchronize",
                     &f_.NvVFX_CudaStreamSynchronize) ||
      !load_required("NvVFX_SetCudaStream", &f_.NvVFX_SetCudaStream)) {
    return false;
  }

  load_optional("NvVFX_GetCudaStream", &f_.NvVFX_GetCudaStream);
  load_optional("NvCV_GetErrorStringFromCode", &f_.NvCV_GetErrorStringFromCode);

  TryLoadCudaRuntime();

  return true;
}

void VfxApi::TryLoadCudaRuntime() {
  cuda_loaded_ = false;
  cuda_ = CudaFunctions{};
  cuda_lib_.Close();

  const std::vector<std::string> candidates = {
      "libcudart.so",
      "libcudart.so.12",
      "libcudart.so.11.0",
  };

  for (const auto &name : candidates) {
    util::DynLib lib;
    std::string ignore;
    if (!lib.Open(fs::path(name), util::DynLib::Scope::Local, &ignore)) {
      continue;
    }

    // Require at least cudaMalloc/cudaFree/cudaMemset to consider usable.
    if (!lib.GetSymbol("cudaMalloc", &cuda_.cudaMalloc, nullptr) ||
        !lib.GetSymbol("cudaFree", &cuda_.cudaFree, nullptr) ||
        !lib.GetSymbol("cudaMemset", &cuda_.cudaMemset, nullptr)) {
      continue;
    }

    // Optional helpers.
    lib.GetSymbol("cudaMemsetAsync", &cuda_.cudaMemsetAsync, nullptr);
    lib.GetSymbol("cudaGetErrorString", &cuda_.cudaGetErrorString, nullptr);

    cuda_lib_ = std::move(lib);
    cuda_loaded_ = true;
    return;
  }
}

std::string VfxApi::StatusToString(NvCV_Status code) const {
  if (code == NVCV_SUCCESS) {
    return "NVCV_SUCCESS";
  }
  if (f_.NvCV_GetErrorStringFromCode) {
    const char *s = f_.NvCV_GetErrorStringFromCode(code);
    if (s && *s) {
      return std::string(s);
    }
  }

  std::ostringstream oss;
  oss << "NvCV_Status(" << code << ")";
  return oss.str();
}

} // namespace studiocast::maxine::vfx
