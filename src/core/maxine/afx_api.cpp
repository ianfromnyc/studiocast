#include "core/maxine/afx_api.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "core/util/xdg.h"

namespace studiocast::maxine::afx {

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

  roots.push_back(util::DefaultAfxRoot());
  roots.emplace_back("/usr/local/Audio_Effects_SDK");

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
  dirs.reserve(roots.size() * 12);
  for (const auto &root : roots) {
    // AFX extraction layout typically nests the core library under nvafx/lib.
    dirs.push_back(root / "nvafx" / "lib");
    dirs.push_back(root / "nvafx" / "lib64");
    dirs.push_back(root / "nvafx" / "lib" / "x86_64-linux-gnu");
    dirs.push_back(root / "nvafx" / "lib64" / "x86_64-linux-gnu");

    // Some layouts put shared libraries under the root directly or standard lib
    // dirs.
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

  // 0) Try preferred names under candidate directories. Explicit SDK roots
  // must win over unrelated libraries found through the process loader path.
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

  // 1) Fall back to bare names via the system loader path for users who
  // intentionally expose Maxine through ldconfig / LD_LIBRARY_PATH.
  for (const auto &name : preferred_names) {
    if (auto r = try_file(fs::path(name))) {
      return r;
    }
  }

  return std::nullopt;
}

} // namespace

AfxApi::AfxApi() = default;

AfxApi::~AfxApi() = default;

AfxApi::AfxApi(AfxApi &&) noexcept = default;

AfxApi &AfxApi::operator=(AfxApi &&) noexcept = default;

bool AfxApi::Initialize(std::string *error_out) {
  const std::vector<fs::path> roots = {
      util::DefaultAfxRoot(),
      fs::path("/usr/local/Audio_Effects_SDK"),
  };
  return Initialize(roots, error_out);
}

bool AfxApi::Initialize(const std::vector<std::filesystem::path> &sdk_roots,
                        std::string *error_out) {
  initialized_ = false;
  library_path_.clear();
  lib_.Close();
  f_ = Functions{};
  error_.clear();

  return InitializeImpl(sdk_roots, error_out);
}

bool AfxApi::InitializeFromLibraryPath(
    const std::filesystem::path &library_path, std::string *error_out) {
  initialized_ = false;
  library_path_.clear();
  lib_.Close();
  f_ = Functions{};
  error_.clear();

  return InitializeFromLibraryPathImpl(library_path, error_out);
}

bool AfxApi::InitializeImpl(const std::vector<fs::path> &sdk_roots,
                            std::string *error_out) {
  const auto roots = CandidateRoots(sdk_roots);
  const auto dirs = CandidateLibDirs(roots);

  std::string last;
  const std::vector<std::string> preferred = {
      "libnv_audiofx.so",
      "libnv_audiofx.so.1",
  };

  auto found = FindLibWithSymbol(dirs, preferred, "NvAFX_CreateEffect",
                                 util::DynLib::Scope::Global, &last);
  if (!found) {
    std::ostringstream oss;
    oss << "Could not load NvAFX library (missing NvAFX_CreateEffect).";
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

bool AfxApi::InitializeFromLibraryPathImpl(const fs::path &library_path,
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

bool AfxApi::LoadSymbols(std::string *error_out) {
  std::string err;
  auto req = [&](const char *sym, auto *out) -> bool {
    if (!lib_.GetSymbol(sym, out, &err)) {
      if (error_out) {
        *error_out = err;
      }
      return false;
    }
    return true;
  };

  if (!req("NvAFX_CreateEffect", &f_.NvAFX_CreateEffect))
    return false;
  if (!req("NvAFX_DestroyEffect", &f_.NvAFX_DestroyEffect))
    return false;

  if (!req("NvAFX_SetU32", &f_.NvAFX_SetU32))
    return false;
  // Optional list setter.
  (void)lib_.GetSymbol("NvAFX_SetU32List", &f_.NvAFX_SetU32List, nullptr);
  if (!req("NvAFX_SetFloat", &f_.NvAFX_SetFloat))
    return false;
  if (!req("NvAFX_SetString", &f_.NvAFX_SetString))
    return false;

  if (!req("NvAFX_GetU32", &f_.NvAFX_GetU32))
    return false;
  // Optional list getter.
  (void)lib_.GetSymbol("NvAFX_GetU32List", &f_.NvAFX_GetU32List, nullptr);

  if (!req("NvAFX_Load", &f_.NvAFX_Load))
    return false;
  if (!req("NvAFX_Run", &f_.NvAFX_Run))
    return false;

  return true;
}

} // namespace studiocast::maxine::afx
