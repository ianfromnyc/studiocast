#include "core/maxine/vfx/vfx_runtime.h"

#include "core/util/xdg.h"

#include <filesystem>
#include <sstream>
#include <vector>

namespace studiocast::maxine::vfx {

namespace fs = std::filesystem;

namespace {

using cudaSetDevice_t = int (*)(int device);

struct SharedLibLoadResult {
  util::DynLib lib;
  fs::path path;
};

std::optional<fs::path> FindSdkRoot() {
  const std::vector<fs::path> candidates = {
      util::DefaultVfxRoot(),
      fs::path("/usr/local/VideoFX"),
  };

  for (const auto &root : candidates) {
    std::error_code ec;
    if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) {
      continue;
    }
    const auto models = root / "models";
    if (fs::exists(models, ec) && fs::is_directory(models, ec)) {
      return root;
    }
  }
  return std::nullopt;
}

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

  // 1) Try preferred names first.
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

  // 2) Scan directory for versioned variants of the known VFX libraries.
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

VfxRuntime::VfxRuntime() = default;

VfxRuntime::~VfxRuntime() {
  if (stream_ && api_.NvVFX_CudaStreamDestroy) {
    api_.NvVFX_CudaStreamDestroy(stream_);
    stream_ = nullptr;
  }
}

bool VfxRuntime::Initialize(const config::GpuSelection &gpu_policy,
                            std::string *error_out) {
  diag_ = VfxDiagnostics{};
  api_ = VfxApi{};

  auto load_required = [&](const util::DynLib &lib, const char *symbol,
                           auto *out) -> bool {
    std::string err;
    if (!lib.GetSymbol(symbol, out, &err)) {
      diag_.error = err;
      if (error_out) {
        *error_out = diag_.error;
      }
      return false;
    }
    return true;
  };

  auto load_optional = [&](const util::DynLib &lib, const char *symbol,
                           auto *out) { lib.GetSymbol(symbol, out, nullptr); };

  // GPU selection
  {
    auto sel = studiocast::maxine::SelectGpu(gpu_policy);
    diag_.selected_gpu = sel.selected;
    if (!sel.ok()) {
      diag_.error = sel.error;
      if (error_out) {
        *error_out = diag_.error;
      }
      return false;
    }
  }

  // SDK root
  const auto root = FindSdkRoot();
  if (!root) {
    diag_.error = "Maxine VideoFX SDK not found (missing models directory).";
    if (error_out) {
      *error_out = diag_.error;
    }
    return false;
  }

  diag_.sdk_root = *root;
  diag_.models_dir = *root / "models";

  const auto lib_dirs = CandidateLibDirs(*root);

  // Load VFX library.
  {
    std::string last;
    const std::vector<std::string> preferred = {
        "libVideoFX.so",
        "libnvVideoEffects.so",
        "libNVVideoEffects.so",
        "libnvvfx.so",
        "libNvVFX.so",
    };

    auto found = FindLibWithSymbol(lib_dirs, preferred, "NvVFX_CreateEffect",
                                   util::DynLib::Scope::Global, &last);
    if (!found) {
      std::ostringstream oss;
      oss << "Could not load Maxine VFX library (missing NvVFX_CreateEffect).";
      if (!last.empty()) {
        oss << " loader error: " << last;
      }
      diag_.error = oss.str();
      if (error_out) {
        *error_out = diag_.error;
      }
      return false;
    }

    vfx_lib_ = std::move(found->lib);
    diag_.vfx_library = found->path;
  }

  // Load symbols (VFX)
  if (!load_required(vfx_lib_, "NvVFX_CreateEffect",
                     &api_.NvVFX_CreateEffect) ||
      !load_required(vfx_lib_, "NvVFX_DestroyEffect",
                     &api_.NvVFX_DestroyEffect) ||
      !load_required(vfx_lib_, "NvVFX_CudaStreamCreate",
                     &api_.NvVFX_CudaStreamCreate) ||
      !load_required(vfx_lib_, "NvVFX_CudaStreamDestroy",
                     &api_.NvVFX_CudaStreamDestroy) ||
      !load_required(vfx_lib_, "NvVFX_SetCudaStream",
                     &api_.NvVFX_SetCudaStream) ||
      !load_required(vfx_lib_, "NvVFX_Load", &api_.NvVFX_Load) ||
      !load_required(vfx_lib_, "NvVFX_Run", &api_.NvVFX_Run) ||
      !load_required(vfx_lib_, "NvVFX_SetImage", &api_.NvVFX_SetImage) ||
      !load_required(vfx_lib_, "NvVFX_SetString", &api_.NvVFX_SetString) ||
      !load_required(vfx_lib_, "NvVFX_SetF32", &api_.NvVFX_SetF32) ||
      !load_required(vfx_lib_, "NvVFX_SetU32", &api_.NvVFX_SetU32) ||
      !load_required(vfx_lib_, "NvVFX_SetS32", &api_.NvVFX_SetS32) ||
      !load_required(vfx_lib_, "NvVFX_AllocateState",
                     &api_.NvVFX_AllocateState) ||
      !load_required(vfx_lib_, "NvVFX_DeallocateState",
                     &api_.NvVFX_DeallocateState) ||
      !load_required(vfx_lib_, "NvVFX_ResetState", &api_.NvVFX_ResetState) ||
      !load_required(vfx_lib_, "NvVFX_SetStateObjectHandleArray",
                     &api_.NvVFX_SetStateObjectHandleArray)) {
    return false;
  }

  // Optional symbols
  load_optional(vfx_lib_, "NvVFX_GetCudaStream", &api_.NvVFX_GetCudaStream);
  load_optional(vfx_lib_, "NvVFX_GetString", &api_.NvVFX_GetString);
  load_optional(vfx_lib_, "NvVFX_GetU32", &api_.NvVFX_GetU32);
  load_optional(vfx_lib_, "NvVFX_GetS32", &api_.NvVFX_GetS32);
  load_optional(vfx_lib_, "NvVFX_GetF32", &api_.NvVFX_GetF32);
  load_optional(vfx_lib_, "NvVFX_SetObject", &api_.NvVFX_SetObject);
  load_optional(vfx_lib_, "NvVFX_GetObject", &api_.NvVFX_GetObject);
  load_optional(vfx_lib_, "NvCV_GetErrorStringFromCode",
                &api_.NvCV_GetErrorStringFromCode);

  // NvCVImage symbols might live in the VFX library or in a separate library.
  {
    std::string err;

    // 1) Prefer symbols from the already-resolved VFX library.
    const bool loaded_from_vfx = nvcv_api_.InitializeFromLibraryPath(
        NvcvApi::Requirement::VfxCompat, diag_.vfx_library, &err);

    // 2) Otherwise, locate the canonical NvCVImage library (commonly
    // libnvcvimage.so).
    if (!loaded_from_vfx) {
      if (!nvcv_api_.Initialize(NvcvApi::Requirement::VfxCompat,
                                {diag_.sdk_root}, &err)) {
        diag_.error = err;
        if (error_out) {
          *error_out = diag_.error;
        }
        return false;
      }
    }

    diag_.nvcv_library = nvcv_api_.library_path();

    api_.NvCVImage_Init = nvcv_api_.f().NvCVImage_Init;
    api_.NvCVImage_Alloc = nvcv_api_.f().NvCVImage_Alloc;
    api_.NvCVImage_Realloc = nvcv_api_.f().NvCVImage_Realloc;
    api_.NvCVImage_Dealloc = nvcv_api_.f().NvCVImage_Dealloc;
    api_.NvCVImage_Transfer = nvcv_api_.f().NvCVImage_Transfer;
    api_.NvCVImage_CompositeOverConstant =
        nvcv_api_.f().NvCVImage_CompositeOverConstant;
    api_.NvCVImage_Composite = nvcv_api_.f().NvCVImage_Composite;

    if (!api_.NvCV_GetErrorStringFromCode) {
      api_.NvCV_GetErrorStringFromCode =
          nvcv_api_.f().NvCV_GetErrorStringFromCode;
    }
  }

  // CUDA runtime (optional but recommended for multi-GPU correctness).
  cudaSetDevice_t cudaSetDevice = nullptr;
  {
    const std::vector<std::string> cuda_candidates = {
        "libcudart.so",
        "libcudart.so.12",
        "libcudart.so.11.0",
    };

    for (const auto &name : cuda_candidates) {
      util::DynLib lib;
      std::string ignore;
      if (!lib.Open(fs::path(name), util::DynLib::Scope::Local, &ignore)) {
        continue;
      }
      if (lib.GetSymbol("cudaSetDevice", &cudaSetDevice, nullptr)) {
        diag_.cuda_runtime_loaded = true;
        cuda_lib_ = std::move(lib);
        break;
      }
    }
  }

  const int gpu_index = diag_.selected_gpu->index;

  if (cudaSetDevice) {
    const int rc = cudaSetDevice(gpu_index);
    if (rc != 0) {
      std::ostringstream oss;
      oss << "cudaSetDevice(" << gpu_index << ") failed (cudaError=" << rc
          << ").";
      diag_.error = oss.str();
      if (error_out) {
        *error_out = diag_.error;
      }
      return false;
    }
  } else {
    // If user selects GPU 0, we can often still work.
    if (gpu_index != 0) {
      diag_.error = "CUDA runtime (libcudart) not found; cannot select a "
                    "non-zero GPU index reliably.";
      if (error_out) {
        *error_out = diag_.error;
      }
      return false;
    }
  }

  // Set global GPU selection in the VFX SDK.
  {
    const NvCV_Status st = api_.NvVFX_SetS32(nullptr, "GPU", gpu_index);
    if (st != NVCV_SUCCESS) {
      std::ostringstream oss;
      oss << "NvVFX_SetS32(NULL, GPU, " << gpu_index << ") failed (" << st
          << ").";
      if (api_.NvCV_GetErrorStringFromCode) {
        oss << " " << api_.NvCV_GetErrorStringFromCode(st);
      }
      diag_.error = oss.str();
      if (error_out) {
        *error_out = diag_.error;
      }
      return false;
    }
  }

  // Create a CUDA stream for SDK work.
  {
    const NvCV_Status st = api_.NvVFX_CudaStreamCreate(&stream_);
    if (st != NVCV_SUCCESS || !stream_) {
      std::ostringstream oss;
      oss << "NvVFX_CudaStreamCreate failed (" << st << ").";
      if (api_.NvCV_GetErrorStringFromCode) {
        oss << " " << api_.NvCV_GetErrorStringFromCode(st);
      }
      diag_.error = oss.str();
      if (error_out) {
        *error_out = diag_.error;
      }
      return false;
    }
  }

  diag_.initialized = true;
  if (error_out) {
    *error_out = {};
  }
  return true;
}

} // namespace studiocast::maxine::vfx
