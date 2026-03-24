#include "core/maxine/ar_api.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "core/util/xdg.h"

namespace studiocast::maxine::ar {

namespace fs = std::filesystem;

namespace {

struct SharedLibLoadResult {
  util::DynLib lib;
  fs::path path;
};

std::vector<fs::path>
CandidateRoots(const std::vector<fs::path> &explicit_roots) {
  std::vector<fs::path> roots;
  roots.reserve(explicit_roots.size() + 4);

  for (const auto &r : explicit_roots) {
    if (!r.empty()) {
      roots.push_back(r);
    }
  }

  roots.push_back(util::DefaultArRoot());
  roots.emplace_back("/usr/local/ARSDK");

  // Dedup while preserving order.
  std::vector<fs::path> dedup;
  dedup.reserve(roots.size());
  for (const auto &r : roots) {
    if (r.empty())
      continue;
    if (std::find(dedup.begin(), dedup.end(), r) == dedup.end()) {
      dedup.push_back(r);
    }
  }
  return dedup;
}

std::vector<fs::path> CandidateLibDirs(const std::vector<fs::path> &roots) {
  std::vector<fs::path> dirs;
  dirs.reserve(roots.size() * 6);
  for (const auto &root : roots) {
    dirs.push_back(root);
    dirs.push_back(root / "lib");
    dirs.push_back(root / "lib64");
    dirs.push_back(root / "bin");
    dirs.push_back(root / "lib" / "x86_64-linux-gnu");
    dirs.push_back(root / "lib64" / "x86_64-linux-gnu");
  }

  // Dedup while preserving order.
  std::vector<fs::path> dedup;
  dedup.reserve(dirs.size());
  for (const auto &d : dirs) {
    if (d.empty())
      continue;
    if (std::find(dedup.begin(), dedup.end(), d) == dedup.end()) {
      dedup.push_back(d);
    }
  }
  return dedup;
}

bool LooksLikeArLibName(const fs::path &p) {
  const auto s = p.filename().string();
  return s.rfind("libnvARPose.so", 0) == 0 || s.rfind("libnvar.so", 0) == 0 ||
         s.rfind("libNvAR.so", 0) == 0;
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

  // 2) Scan for versioned variants of the known AR library names.
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
      if (!LooksLikeArLibName(p)) {
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

ArApi::ArApi() = default;

ArApi::~ArApi() = default;

ArApi::ArApi(ArApi &&) noexcept = default;

ArApi &ArApi::operator=(ArApi &&) noexcept = default;

bool ArApi::Initialize(std::string *error_out) {
  const std::vector<fs::path> roots = {
      util::DefaultArRoot(),
      fs::path("/usr/local/ARSDK"),
  };
  return Initialize(roots, error_out);
}

bool ArApi::Initialize(const std::vector<fs::path> &sdk_roots,
                       std::string *error_out) {
  initialized_ = false;
  library_path_.clear();
  lib_.Close();
  f_ = Functions{};
  error_.clear();

  return InitializeImpl(sdk_roots, error_out);
}

bool ArApi::InitializeFromLibraryPath(const fs::path &library_path,
                                      std::string *error_out) {
  initialized_ = false;
  library_path_.clear();
  lib_.Close();
  f_ = Functions{};
  error_.clear();

  return InitializeFromLibraryPathImpl(library_path, error_out);
}

bool ArApi::InitializeImpl(const std::vector<fs::path> &sdk_roots,
                           std::string *error_out) {
  const auto roots = CandidateRoots(sdk_roots);
  const auto dirs = CandidateLibDirs(roots);

  std::string last;
  const std::vector<std::string> preferred = {
      "libnvARPose.so",   "libnvar.so",   "libNvAR.so",
      "libnvARPose.so.1", "libnvar.so.1", "libNvAR.so.1",
  };

  auto found = FindLibWithSymbol(dirs, preferred, "NvAR_Create",
                                 util::DynLib::Scope::Global, &last);
  if (!found) {
    std::ostringstream oss;
    oss << "Could not load NvAR library (missing NvAR_Create).";
    if (!last.empty()) {
      oss << " loader error: " << last;
    }
    error_ = oss.str();
    if (error_out) {
      *error_out = error_;
    }
    return false;
  }

  lib_ = std::move(found->lib);
  library_path_ = found->path;

  std::string err;
  if (!LoadSymbols(&err)) {
    error_ = err;
    lib_.Close();
    library_path_.clear();
    if (error_out) {
      *error_out = error_;
    }
    return false;
  }

  initialized_ = true;
  return true;
}

bool ArApi::InitializeFromLibraryPathImpl(const fs::path &library_path,
                                          std::string *error_out) {
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

bool ArApi::LoadSymbols(std::string *error_out) {
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

  if (!load_required("NvAR_Create", &f_.NvAR_Create) ||
      !load_required("NvAR_Load", &f_.NvAR_Load) ||
      !load_required("NvAR_Run", &f_.NvAR_Run) ||
      !load_required("NvAR_Destroy", &f_.NvAR_Destroy) ||
      !load_required("NvAR_SetU32", &f_.NvAR_SetU32) ||
      !load_required("NvAR_SetS32", &f_.NvAR_SetS32) ||
      !load_required("NvAR_SetF32", &f_.NvAR_SetF32) ||
      !load_required("NvAR_SetF64", &f_.NvAR_SetF64) ||
      !load_required("NvAR_SetU64", &f_.NvAR_SetU64) ||
      !load_required("NvAR_SetObject", &f_.NvAR_SetObject) ||
      !load_required("NvAR_SetString", &f_.NvAR_SetString) ||
      !load_required("NvAR_SetF32Array", &f_.NvAR_SetF32Array) ||
      !load_required("NvAR_GetU32", &f_.NvAR_GetU32) ||
      !load_required("NvAR_GetS32", &f_.NvAR_GetS32) ||
      !load_required("NvAR_GetF32", &f_.NvAR_GetF32) ||
      !load_required("NvAR_GetF64", &f_.NvAR_GetF64) ||
      !load_required("NvAR_GetU64", &f_.NvAR_GetU64) ||
      !load_required("NvAR_GetObject", &f_.NvAR_GetObject) ||
      !load_required("NvAR_GetString", &f_.NvAR_GetString) ||
      !load_required("NvAR_GetF32Array", &f_.NvAR_GetF32Array)) {
    return false;
  }

  load_optional("NvAR_CudaStreamCreate", &f_.NvAR_CudaStreamCreate);
  load_optional("NvAR_CudaStreamDestroy", &f_.NvAR_CudaStreamDestroy);
  load_optional("NvAR_SetCudaStream", &f_.NvAR_SetCudaStream);
  load_optional("NvAR_GetCudaStream", &f_.NvAR_GetCudaStream);
  load_optional("NvAR_GetVersion", &f_.NvAR_GetVersion);
  load_optional("NvCV_GetErrorStringFromCode", &f_.NvCV_GetErrorStringFromCode);

  return true;
}

std::string ArApi::StatusToString(NvCV_Status code) const {
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

} // namespace studiocast::maxine::ar
