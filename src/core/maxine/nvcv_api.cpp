#include "core/maxine/nvcv_api.h"

#include <algorithm>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "core/util/dynlib.h"
#include "core/util/xdg.h"

namespace studiocast::maxine {

namespace fs = std::filesystem;

class NvcvApi::Impl {
public:
  util::DynLib lib;
};

namespace {

std::vector<fs::path>
CandidateRoots(const std::vector<fs::path> &explicit_roots) {
  std::vector<fs::path> roots;
  roots.reserve(explicit_roots.size() + 8);

  for (const auto &r : explicit_roots) {
    if (!r.empty()) {
      roots.push_back(r);
    }
  }

  // User-local canonical layout
  roots.push_back(util::DefaultVfxRoot());
  roots.push_back(util::DefaultArRoot());

  // Common system-wide install locations
  roots.emplace_back("/usr/local/VideoFX");
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

bool LooksLikeSharedObject(const fs::path &p) {
  const auto s = p.filename().string();
  return s.find(".so") != std::string::npos;
}

bool LooksLikeNvcvImageLibName(const fs::path &p) {
  const auto s = p.filename().string();
  if (s.find("nvcv") == std::string::npos &&
      s.find("NVCV") == std::string::npos) {
    return false;
  }
  if (s.find("image") == std::string::npos &&
      s.find("Image") == std::string::npos) {
    return false;
  }
  return LooksLikeSharedObject(p);
}

struct SharedLibLoadResult {
  util::DynLib lib;
  fs::path path;
};

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

  // 2) Scan for likely matches (versioned .so names).
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
      if (!LooksLikeNvcvImageLibName(p)) {
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

NvcvApi::NvcvApi() : impl_(new Impl) {}

NvcvApi::~NvcvApi() {
  delete impl_;
  impl_ = nullptr;
}

NvcvApi::NvcvApi(NvcvApi &&other) noexcept { *this = std::move(other); }

NvcvApi &NvcvApi::operator=(NvcvApi &&other) noexcept {
  if (this == &other) {
    return *this;
  }
  std::swap(initialized_, other.initialized_);
  std::swap(library_path_, other.library_path_);
  std::swap(f_, other.f_);
  std::swap(error_, other.error_);
  std::swap(impl_, other.impl_);
  return *this;
}

bool NvcvApi::Initialize(Requirement req, std::string *error_out) {
  return InitializeImpl(req, {}, error_out);
}

bool NvcvApi::Initialize(Requirement req,
                         const std::vector<fs::path> &sdk_roots,
                         std::string *error_out) {
  return InitializeImpl(req, sdk_roots, error_out);
}

bool NvcvApi::InitializeFromLibraryPath(Requirement req,
                                        const fs::path &library_path,
                                        std::string *error_out) {
  return InitializeFromLibraryPathImpl(req, library_path, error_out);
}

bool NvcvApi::InitializeImpl(Requirement req,
                             const std::vector<fs::path> &sdk_roots,
                             std::string *error_out) {
  initialized_ = false;
  library_path_.clear();
  f_ = Functions{};
  error_.clear();
  if (error_out) {
    error_out->clear();
  }

  if (!impl_) {
    error_ = "Internal error: NvcvApi not constructed.";
    if (error_out)
      *error_out = error_;
    return false;
  }

  const auto roots = CandidateRoots(sdk_roots);
  const auto dirs = CandidateLibDirs(roots);

  const std::vector<std::string> preferred_names = {
      "libnvcvimage.so",
      "libNVCVImage.so",
      "libnvcvimage.so.1",
      "libNVCVImage.so.1",
  };

  const char *probe_symbol =
      (req == Requirement::Minimal) ? "NvCVImage_Alloc" : "NvCVImage_Init";

  std::string last;
  auto found = FindLibWithSymbol(dirs, preferred_names, probe_symbol,
                                 util::DynLib::Scope::Global, &last);
  if (!found) {
    std::ostringstream oss;
    oss << "Could not load NvCVImage library (missing " << probe_symbol << ").";
    if (!last.empty()) {
      oss << " loader error: " << last;
    }
    error_ = oss.str();
    if (error_out) {
      *error_out = error_;
    }
    return false;
  }

  impl_->lib = std::move(found->lib);
  library_path_ = found->path;
  return LoadSymbols(req, error_out);
}

bool NvcvApi::InitializeFromLibraryPathImpl(Requirement req,
                                            const fs::path &library_path,
                                            std::string *error_out) {
  initialized_ = false;
  library_path_.clear();
  f_ = Functions{};
  error_.clear();
  if (error_out) {
    error_out->clear();
  }

  if (!impl_) {
    error_ = "Internal error: NvcvApi not constructed.";
    if (error_out)
      *error_out = error_;
    return false;
  }

  std::string err;
  if (!impl_->lib.Open(library_path, util::DynLib::Scope::Global, &err)) {
    error_ = err;
    if (error_out)
      *error_out = error_;
    return false;
  }

  const char *probe_symbol =
      (req == Requirement::Minimal) ? "NvCVImage_Alloc" : "NvCVImage_Init";
  if (!impl_->lib.GetSymbolRaw(probe_symbol, &err)) {
    error_ = err;
    if (error_out)
      *error_out = error_;
    return false;
  }

  library_path_ = library_path;
  return LoadSymbols(req, error_out);
}

bool NvcvApi::LoadSymbols(Requirement req, std::string *error_out) {
  auto load_required = [&](const char *symbol, auto *out) -> bool {
    std::string err;
    if (!impl_->lib.GetSymbol(symbol, out, &err)) {
      error_ = err;
      if (error_out) {
        *error_out = error_;
      }
      return false;
    }
    return true;
  };

  auto load_optional = [&](const char *symbol, auto *out) {
    impl_->lib.GetSymbol(symbol, out, nullptr);
  };

  // Minimal required set.
  if (!load_required("NvCVImage_Alloc", &f_.NvCVImage_Alloc) ||
      !load_required("NvCVImage_Dealloc", &f_.NvCVImage_Dealloc) ||
      !load_required("NvCVImage_Transfer", &f_.NvCVImage_Transfer) ||
      !load_required("NvCVImage_Composite", &f_.NvCVImage_Composite)) {
    return false;
  }

  // VFX compatibility extras.
  if (req == Requirement::VfxCompat) {
    if (!load_required("NvCVImage_Init", &f_.NvCVImage_Init) ||
        !load_required("NvCVImage_Realloc", &f_.NvCVImage_Realloc) ||
        !load_required("NvCVImage_CompositeOverConstant",
                       &f_.NvCVImage_CompositeOverConstant)) {
      return false;
    }
  } else {
    load_optional("NvCVImage_Init", &f_.NvCVImage_Init);
    load_optional("NvCVImage_Realloc", &f_.NvCVImage_Realloc);
    load_optional("NvCVImage_CompositeOverConstant",
                  &f_.NvCVImage_CompositeOverConstant);
  }

  // Optional error strings (may live here or in a parent SDK library; best
  // effort).
  load_optional("NvCV_GetErrorStringFromCode", &f_.NvCV_GetErrorStringFromCode);

  initialized_ = true;
  return true;
}

} // namespace studiocast::maxine
