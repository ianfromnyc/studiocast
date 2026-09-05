#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <elf.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "core/maxine/afx_api.h"
#include "core/maxine/ar_api.h"
#include "core/maxine/paths.h"
#include "core/maxine/sdk_runtime.h"
#include "core/maxine/vfx_api.h"

#ifndef STUDIOCAST_CXX_COMPILER
#define STUDIOCAST_CXX_COMPILER "c++"
#endif

namespace {
namespace fs = std::filesystem;

class EnvGuard {
public:
  EnvGuard(const char *name, const std::string &value) : name_(name) {
    if (const char *old = std::getenv(name)) {
      old_value_ = std::string(old);
    }
    ::setenv(name, value.c_str(), 1);
  }

  ~EnvGuard() {
    if (old_value_) {
      ::setenv(name_, old_value_->c_str(), 1);
    } else {
      ::unsetenv(name_);
    }
  }

  EnvGuard(const EnvGuard &) = delete;
  EnvGuard &operator=(const EnvGuard &) = delete;

private:
  const char *name_;
  std::optional<std::string> old_value_;
};

bool Touch(const fs::path &path) {
  std::ofstream out(path, std::ios::out | std::ios::trunc);
  return out.good();
}

bool Require(bool condition, const std::string &message) {
  if (!condition) {
    std::cerr << message << "\n";
    return false;
  }
  return true;
}

std::string ShellQuote(const std::string &value) {
  std::string out = "'";
  for (const char c : value) {
    if (c == '\'') {
      out += "'\\''";
    } else {
      out += c;
    }
  }
  out += "'";
  return out;
}

bool CommandExitedOk(int rc) {
  if (rc == 0)
    return true;
#ifdef WIFEXITED
  return WIFEXITED(rc) && WEXITSTATUS(rc) == 0;
#else
  return false;
#endif
}

bool WriteFakeAfxLibrarySource(const fs::path &path) {
  std::ofstream out(path, std::ios::out | std::ios::trunc);
  if (!out)
    return false;

  out << R"cpp(
extern "C" int NvAFX_CreateEffect(const char *, void **) { return 0; }
extern "C" int NvAFX_DestroyEffect(void *) { return 0; }
extern "C" int NvAFX_SetU32(void *, const char *, unsigned int) { return 0; }
extern "C" int NvAFX_SetFloat(void *, const char *, float) { return 0; }
extern "C" int NvAFX_SetString(void *, const char *, const char *) { return 0; }
extern "C" int NvAFX_GetU32(void *, const char *, unsigned int *) { return 0; }
extern "C" int NvAFX_Load(void *) { return 0; }
extern "C" int NvAFX_Run(void *, const float *, float *, unsigned int) { return 0; }
)cpp";
  return out.good();
}

bool CompileFakeAfxLibrary(const fs::path &library_path) {
  std::error_code ec;
  fs::create_directories(library_path.parent_path(), ec);
  if (ec) {
    std::cerr << "failed to create fake AFX lib directory: " << ec.message()
              << "\n";
    return false;
  }

  const fs::path source_path = library_path.parent_path() / "fake_afx.cpp";
  if (!WriteFakeAfxLibrarySource(source_path)) {
    std::cerr << "failed to write fake AFX source: " << source_path << "\n";
    return false;
  }

  std::ostringstream cmd;
  cmd << ShellQuote(STUDIOCAST_CXX_COMPILER) << " -shared -fPIC "
      << ShellQuote(source_path.string()) << " -o "
      << ShellQuote(library_path.string());
  const int rc = std::system(cmd.str().c_str());
  if (!CommandExitedOk(rc)) {
    std::cerr << "failed to compile fake AFX library with command:\n"
              << cmd.str() << "\n";
    return false;
  }
  return true;
}

bool RunAfxLoaderPriorityChild() {
  const char *sdk_root_env = std::getenv("STUDIOCAST_AFX_TEST_SDK_ROOT");
  const char *expected_env = std::getenv("STUDIOCAST_AFX_TEST_EXPECTED_LIB");
  if (!sdk_root_env || !expected_env) {
    std::cerr << "AFX loader priority child missing environment\n";
    return false;
  }

  const fs::path sdk_root = sdk_root_env;
  const fs::path expected = expected_env;

  studiocast::maxine::afx::AfxApi api;
  std::string err;
  if (!api.Initialize({sdk_root}, &err)) {
    std::cerr << "fake AFX Initialize failed: " << err << "\n";
    return false;
  }

  return Require(api.library_path() == expected,
                 "expected AFX loader to prefer SDK root library " +
                     expected.string() + ", got " +
                     api.library_path().string());
}

bool TestAfxLoaderPrefersExplicitSdkRootBeforeBareLoaderPath(
    const char *argv0) {
  const fs::path root =
      fs::temp_directory_path() /
      ("studiocast-afx-loader-priority-" + std::to_string(::getpid()));
  std::error_code ec;
  fs::remove_all(root, ec);

  const fs::path sdk_root = root / "Audio_Effects_SDK";
  const fs::path sdk_lib = sdk_root / "nvafx" / "lib" / "libnv_audiofx.so";
  const fs::path bare_dir = root / "bare";
  const fs::path bare_lib = bare_dir / "libnv_audiofx.so";

  bool ok = CompileFakeAfxLibrary(sdk_lib) && CompileFakeAfxLibrary(bare_lib);
  if (!ok) {
    fs::remove_all(root, ec);
    return false;
  }

  const std::string old_ld = std::getenv("LD_LIBRARY_PATH")
                                 ? std::getenv("LD_LIBRARY_PATH")
                                 : "";
  const std::string child_ld =
      old_ld.empty() ? bare_dir.string() : (bare_dir.string() + ":" + old_ld);

  ::setenv("STUDIOCAST_AFX_LOADER_PRIORITY_CHILD", "1", 1);
  ::setenv("STUDIOCAST_AFX_TEST_SDK_ROOT", sdk_root.string().c_str(), 1);
  ::setenv("STUDIOCAST_AFX_TEST_EXPECTED_LIB", sdk_lib.string().c_str(), 1);
  ::setenv("LD_LIBRARY_PATH", child_ld.c_str(), 1);

  const fs::path self = fs::absolute(argv0, ec);
  const std::string self_path = ec ? std::string(argv0) : self.string();
  const int rc = std::system(ShellQuote(self_path).c_str());

  ::unsetenv("STUDIOCAST_AFX_LOADER_PRIORITY_CHILD");
  ::unsetenv("STUDIOCAST_AFX_TEST_SDK_ROOT");
  ::unsetenv("STUDIOCAST_AFX_TEST_EXPECTED_LIB");
  if (old_ld.empty()) {
    ::unsetenv("LD_LIBRARY_PATH");
  } else {
    ::setenv("LD_LIBRARY_PATH", old_ld.c_str(), 1);
  }

  fs::remove_all(root, ec);

  return Require(CommandExitedOk(rc),
                 "AFX loader priority child failed; bare loader path may have "
                 "won over explicit SDK root");
}

bool TestCurrentLinuxMaxineLibraryNamesResolve() {
  const fs::path root =
      fs::temp_directory_path() /
      ("studiocast-maxine-paths-" + std::to_string(::getpid()));
  std::error_code ec;
  fs::remove_all(root, ec);

  const fs::path vfx = root / "VideoFX";
  const fs::path ar = root / "ARSDK";
  const fs::path afx = root / "Audio_Effects_SDK";

  fs::create_directories(vfx / "lib", ec);
  fs::create_directories(vfx / "models", ec);
  fs::create_directories(vfx / "features", ec);
  fs::create_directories(ar / "lib", ec);
  fs::create_directories(ar / "models", ec);
  fs::create_directories(ar / "features", ec);
  fs::create_directories(afx / "nvafx" / "lib", ec);
  fs::create_directories(afx / "features", ec);
  if (ec) {
    std::cerr << "failed to create test SDK layout: " << ec.message() << "\n";
    return false;
  }

  if (!Touch(vfx / "lib" / "libVideoFX.so") ||
      !Touch(ar / "lib" / "libnvARPose.so") ||
      !Touch(afx / "nvafx" / "lib" / "libnv_audiofx.so")) {
    std::cerr << "failed to create fake Maxine libraries\n";
    fs::remove_all(root, ec);
    return false;
  }

  EnvGuard vfx_env("STUDIOCAST_VFX_SDK_ROOT", vfx.string());
  EnvGuard ar_env("STUDIOCAST_AR_SDK_ROOT", ar.string());
  EnvGuard afx_env("STUDIOCAST_AFX_SDK_ROOT", afx.string());

  const auto rep = studiocast::maxine::ResolveMaxinePaths();

  bool ok = true;
  ok &= Require(rep.vfx.ok, "expected VFX component to resolve");
  ok &= Require(rep.vfx.library == vfx / "lib" / "libVideoFX.so",
                "expected VFX to resolve libVideoFX.so, got " +
                    rep.vfx.library.string());
  ok &= Require(rep.ar.ok, "expected AR component to resolve");
  ok &= Require(rep.ar.library == ar / "lib" / "libnvARPose.so",
                "expected AR to resolve libnvARPose.so, got " +
                    rep.ar.library.string());
  ok &= Require(rep.afx.ok, "expected AFX component to resolve");
  ok &= Require(rep.afx.library == afx / "nvafx" / "lib" / "libnv_audiofx.so",
                "expected AFX to resolve libnv_audiofx.so, got " +
                    rep.afx.library.string());

  fs::remove_all(root, ec);
  return ok;
}

// The Maxine "SDK Core" 1.x tree keeps the model engines in `<root>/lib/models`
// and has no `<root>/models` directory.
bool TestSdkCore1xModelsDirResolves() {
  const fs::path root =
      fs::temp_directory_path() /
      ("studiocast-maxine-paths-1x-" + std::to_string(::getpid()));
  std::error_code ec;
  fs::remove_all(root, ec);

  const fs::path vfx = root / "VideoFX";
  const fs::path ar = root / "ARSDK";

  fs::create_directories(vfx / "lib" / "models", ec);
  fs::create_directories(vfx / "features", ec);
  fs::create_directories(ar / "lib" / "models", ec);
  fs::create_directories(ar / "features", ec);
  if (ec) {
    std::cerr << "failed to create SDK Core 1.x layout: " << ec.message()
              << "\n";
    return false;
  }

  if (!Touch(vfx / "lib" / "libVideoFX.so") ||
      !Touch(ar / "lib" / "libnvARPose.so")) {
    std::cerr << "failed to create fake Maxine libraries\n";
    fs::remove_all(root, ec);
    return false;
  }

  EnvGuard vfx_env("STUDIOCAST_VFX_SDK_ROOT", vfx.string());
  EnvGuard ar_env("STUDIOCAST_AR_SDK_ROOT", ar.string());

  const auto rep = studiocast::maxine::ResolveMaxinePaths();

  bool ok = true;
  ok &= Require(rep.vfx.models_dir_exists,
                "expected VFX models dir to exist at <root>/lib/models");
  ok &= Require(rep.vfx.models_dir == vfx / "lib" / "models",
                "expected VFX models dir <root>/lib/models, got " +
                    rep.vfx.models_dir.string());
  ok &= Require(rep.vfx.models_dir_source == "lib/models",
                "expected VFX models dir source 'lib/models', got '" +
                    rep.vfx.models_dir_source + "'");
  ok &= Require(rep.vfx.ok, "expected VFX component to resolve");
  ok &= Require(rep.ar.models_dir == ar / "lib" / "models",
                "expected AR models dir <root>/lib/models, got " +
                    rep.ar.models_dir.string());
  ok &= Require(rep.ar.ok, "expected AR component to resolve");

  fs::remove_all(root, ec);
  return ok;
}

// A legacy 0.7/0.8 tree keeps `<root>/models`. Keep preferring it so existing
// installs do not change behaviour.
bool TestLegacyModelsDirStillWins() {
  const fs::path root =
      fs::temp_directory_path() /
      ("studiocast-maxine-paths-legacy-" + std::to_string(::getpid()));
  std::error_code ec;
  fs::remove_all(root, ec);

  const fs::path vfx = root / "VideoFX";
  fs::create_directories(vfx / "lib" / "models", ec);
  fs::create_directories(vfx / "models", ec);
  fs::create_directories(vfx / "features", ec);
  if (ec || !Touch(vfx / "lib" / "libVideoFX.so")) {
    std::cerr << "failed to create mixed SDK layout\n";
    fs::remove_all(root, ec);
    return false;
  }

  EnvGuard vfx_env("STUDIOCAST_VFX_SDK_ROOT", vfx.string());

  const auto rep = studiocast::maxine::ResolveMaxinePaths();

  bool ok = true;
  ok &= Require(rep.vfx.models_dir == vfx / "models",
                "expected legacy <root>/models to win, got " +
                    rep.vfx.models_dir.string());
  ok &= Require(rep.vfx.models_dir_source == "models",
                "expected VFX models dir source 'models', got '" +
                    rep.vfx.models_dir_source + "'");

  // The effects do not see the report; they only know the library they
  // loaded. Both ways of asking must give one answer on a tree that holds
  // both layouts, or `doctor` names one directory and the effects use
  // another.
  ok &= Require(studiocast::maxine::ModelsDirForLibrary(
                    vfx / "lib" / "libVideoFX.so") == rep.vfx.models_dir,
                "expected the library walk to agree with the resolved paths, "
                "got " +
                    studiocast::maxine::ModelsDirForLibrary(
                        vfx / "lib" / "libVideoFX.so")
                        .string());

  fs::remove_all(root, ec);
  return ok;
}

bool RunShellCommand(const std::string &cmd) {
  const int rc = std::system(cmd.c_str());
  if (!CommandExitedOk(rc)) {
    std::cerr << "command failed:\n" << cmd << "\n";
    return false;
  }
  return true;
}

// Builds a shared library that exports `symbol` and, optionally, links against
// `link_against` and calls the `dep_symbol` that this library exports.
//
// The call is necessary, not decoration. Debian and its derivatives give the
// linker `--as-needed` by default, and that option removes the DT_NEEDED entry
// of a library from which the object uses no symbol. A library that only links
// against the dependency therefore carries no dependency at all on those
// distributions, and the pre-load correctly finds nothing to load. The library
// must use the dependency to depend on it.
bool BuildSharedLib(const fs::path &out, const std::string &soname,
                    const std::string &symbol, const fs::path &link_against,
                    const std::string &dep_symbol = {}) {
  // A dependency the library never calls is not a dependency. Refuse it here
  // instead of building a fixture that only works where `--as-needed` is off.
  if (link_against.empty() != dep_symbol.empty()) {
    std::cerr << "BuildSharedLib needs the symbol of the library it links "
                 "against\n";
    return false;
  }

  std::error_code ec;
  fs::create_directories(out.parent_path(), ec);

  const fs::path src = out.parent_path() / (symbol + ".cpp");
  {
    std::ofstream f(src, std::ios::out | std::ios::trunc);
    if (!f)
      return false;
    if (!link_against.empty()) {
      f << "extern \"C\" int " << dep_symbol << "();\n";
      f << "extern \"C\" int " << symbol << "() { return " << dep_symbol
        << "(); }\n";
    } else {
      // `--as-needed` drops the C library as well, so use it. A library with
      // no dependency at all would make the system-library tests say nothing.
      f << "#include <cstring>\n";
      f << "extern \"C\" int " << symbol
        << "() { return static_cast<int>(std::strlen(\"42\")); }\n";
    }
    if (!f.good())
      return false;
  }

  std::ostringstream cmd;
  cmd << ShellQuote(STUDIOCAST_CXX_COMPILER) << " -shared -fPIC "
      << ShellQuote(src.string()) << " -Wl,-soname," << ShellQuote(soname);
  if (!link_against.empty()) {
    cmd << " " << ShellQuote(link_against.string());
  }
  cmd << " -o " << ShellQuote(out.string());
  return RunShellCommand(cmd.str());
}

// The SDK Core 1.x ships its own CUDA and TensorRT runtime under
// `<root>/external/*/lib`. Those directories are not on the loader path, so
// StudioCast must pre-load them before it opens the SDK library.
bool TestSdkRuntimePreloadMakesBundledDepsResolvable() {
  const fs::path root =
      fs::temp_directory_path() /
      ("studiocast-maxine-preload-" + std::to_string(::getpid()));
  std::error_code ec;
  fs::remove_all(root, ec);

  const fs::path dep = root / "external" / "cuda" / "lib" / "libsc_fake_dep.so";
  const fs::path target = root / "lib" / "libsc_fake_target.so";

  if (!BuildSharedLib(dep, "libsc_fake_dep.so", "sc_fake_dep_value", {}) ||
      !BuildSharedLib(target, "libsc_fake_target.so", "sc_fake_target_value",
                      dep, "sc_fake_dep_value")) {
    fs::remove_all(root, ec);
    return false;
  }

  bool ok = true;

  // Without help the bundled dependency is not on the loader path.
  void *plain = dlopen(target.c_str(), RTLD_NOW | RTLD_LOCAL);
  ok &= Require(plain == nullptr,
                "expected a plain dlopen of the fake SDK library to fail");
  if (plain)
    dlclose(plain);

  const auto report = studiocast::maxine::PreloadSdkRuntime(target);
  ok &= Require(report.problems.empty(),
                "expected the pre-load to report no problems");

  bool saw_dep = false;
  for (const auto &d : report.dependencies) {
    if (d.soname == "libsc_fake_dep.so") {
      saw_dep = true;
      ok &= Require(d.loaded, "expected the bundled dependency to be loaded");
      ok &= Require(d.resolved == dep,
                    "expected the dependency to resolve from external/cuda/lib,"
                    " got " +
                        d.resolved.string());
    }
  }
  ok &= Require(saw_dep, "expected the pre-load to name the bundled soname");

  // A pre-loaded dependency must stay out of the process-wide namespace. The
  // SDK ships its own TensorRT and CUDA, and the same process also loads ONNX
  // Runtime, which was built against other versions of the same sonames. A
  // global pre-load would make ONNX Runtime bind to the SDK copies.
  ok &= Require(::dlsym(RTLD_DEFAULT, "sc_fake_dep_value") == nullptr,
                "expected the pre-loaded dependency to export no symbol into "
                "the global namespace");

  void *after = dlopen(target.c_str(), RTLD_NOW | RTLD_LOCAL);
  ok &= Require(after != nullptr,
                "expected dlopen to succeed after the pre-load");
  if (after)
    dlclose(after);

  fs::remove_all(root, ec);
  return ok;
}

// The loader asks for a library without a root; `doctor` asks for the same
// library with the root it resolved. When that root is the one the library
// implies, both must land on the same cache entry, or the work is done twice
// and the second report says it pre-loaded nothing.
bool TestSdkRuntimePreloadSharesOneEntryForTheInferredRoot() {
  const fs::path root =
      fs::temp_directory_path() /
      ("studiocast-maxine-preload-same-" + std::to_string(::getpid()));
  std::error_code ec;
  fs::remove_all(root, ec);

  const fs::path dep = root / "external" / "cuda" / "lib" / "libsc_same_dep.so";
  const fs::path target = root / "lib" / "libsc_same_target.so";
  if (!BuildSharedLib(dep, "libsc_same_dep.so", "sc_same_dep_value", {}) ||
      !BuildSharedLib(target, "libsc_same_target.so", "sc_same_target_value",
                      dep, "sc_same_dep_value")) {
    fs::remove_all(root, ec);
    return false;
  }

  // The loader's call: no root, so the root comes from the library path.
  const fs::path no_root;
  const auto &implied = studiocast::maxine::PreloadSdkRuntime(target, no_root);
  // The doctor's call: the same root, named.
  const auto &named = studiocast::maxine::PreloadSdkRuntime(target, root);

  bool ok = true;
  ok &= Require(&implied == &named,
                "expected one cache entry for the implied and the named root");
  ok &= Require(named.Summary().find("pre-loaded 1 bundled library") !=
                    std::string::npos,
                "expected the second call to report the pre-load of the first,"
                " got: " +
                    named.Summary());

  fs::remove_all(root, ec);
  return ok;
}

// The report depends on the SDK root, not only on the library. Two roots for
// the same library must each get their own report.
bool TestSdkRuntimePreloadCachesPerSdkRoot() {
  const fs::path base =
      fs::temp_directory_path() /
      ("studiocast-maxine-preload-roots-" + std::to_string(::getpid()));
  std::error_code ec;
  fs::remove_all(base, ec);

  // The target sits outside both roots, so its own directory never holds the
  // dependency.
  const fs::path target = base / "target" / "lib" / "libsc_root_target.so";
  const fs::path empty_root = base / "root-without";
  const fs::path full_root = base / "root-with";
  const fs::path dep =
      full_root / "external" / "cuda" / "lib" / "libsc_root_dep.so";

  fs::create_directories(empty_root / "external" / "cuda" / "lib", ec);
  if (ec ||
      !BuildSharedLib(dep, "libsc_root_dep.so", "sc_root_dep_value", {}) ||
      !BuildSharedLib(target, "libsc_root_target.so", "sc_root_target_value",
                      dep, "sc_root_dep_value")) {
    fs::remove_all(base, ec);
    return false;
  }

  const auto &without =
      studiocast::maxine::PreloadSdkRuntime(target, empty_root);
  bool ok = Require(without.sdk_root == empty_root,
                    "expected the first report to keep the root it was given, "
                    "got " +
                        without.sdk_root.string());
  for (const auto &d : without.dependencies) {
    if (d.soname == "libsc_root_dep.so") {
      ok &= Require(!d.loaded,
                    "expected a root without the dependency not to load it");
    }
  }

  const auto &with = studiocast::maxine::PreloadSdkRuntime(target, full_root);
  ok &= Require(with.sdk_root == full_root,
                "expected the second root to get its own report, got " +
                    with.sdk_root.string());

  bool saw_dep = false;
  for (const auto &d : with.dependencies) {
    if (d.soname == "libsc_root_dep.so") {
      saw_dep = true;
      ok &= Require(d.loaded,
                    "expected the second root to pre-load the dependency");
    }
  }
  ok &= Require(saw_dep, "expected the second report to name the dependency");

  fs::remove_all(base, ec);
  return ok;
}

// Two threads that pre-load the same library and root must end with one
// report. The pre-load computes outside the cache lock, so the first writer
// wins and every caller gets that entry.
bool TestSdkRuntimePreloadIsSharedAcrossThreads() {
  const fs::path base =
      fs::temp_directory_path() /
      ("studiocast-maxine-preload-threads-" + std::to_string(::getpid()));
  std::error_code ec;
  fs::remove_all(base, ec);

  const fs::path root = base / "root";
  const fs::path target = base / "target" / "lib" / "libsc_thread_target.so";
  const fs::path dep =
      root / "external" / "cuda" / "lib" / "libsc_thread_dep.so";

  if (!BuildSharedLib(dep, "libsc_thread_dep.so", "sc_thread_dep_value", {}) ||
      !BuildSharedLib(target, "libsc_thread_target.so",
                      "sc_thread_target_value", dep, "sc_thread_dep_value")) {
    fs::remove_all(base, ec);
    return false;
  }

  const studiocast::maxine::SdkRuntimeReport *first = nullptr;
  const studiocast::maxine::SdkRuntimeReport *second = nullptr;
  std::thread a(
      [&] { first = &studiocast::maxine::PreloadSdkRuntime(target, root); });
  std::thread b(
      [&] { second = &studiocast::maxine::PreloadSdkRuntime(target, root); });
  a.join();
  b.join();

  bool ok = Require(first != nullptr && first == second,
                    "expected both threads to get the same cached report");
  ok &= Require(first != nullptr && first->sdk_root == root,
                "expected the shared report to keep the root it was given");

  bool saw_dep = false;
  if (first != nullptr) {
    for (const auto &d : first->dependencies) {
      if (d.soname == "libsc_thread_dep.so") {
        saw_dep = true;
        ok &= Require(d.loaded, "expected the shared report to pre-load the "
                                "bundled dependency");
      }
    }
  }
  ok &= Require(saw_dep, "expected the shared report to name the dependency");

  fs::remove_all(base, ec);
  return ok;
}

// Core system libraries must stay with the system loader.
bool TestSdkRuntimePreloadSkipsSystemLibraries() {
  const fs::path root =
      fs::temp_directory_path() /
      ("studiocast-maxine-preload-skip-" + std::to_string(::getpid()));
  std::error_code ec;
  fs::remove_all(root, ec);

  const fs::path target = root / "lib" / "libsc_plain_target.so";
  if (!BuildSharedLib(target, "libsc_plain_target.so", "sc_plain_value", {})) {
    fs::remove_all(root, ec);
    return false;
  }

  const auto report = studiocast::maxine::PreloadSdkRuntime(target);

  bool ok = true;
  for (const auto &d : report.dependencies) {
    ok &= Require(!d.loaded, "expected no system library to be pre-loaded, but "
                             "the pre-load took " +
                                 d.soname);
  }
  ok &= Require(report.problems.empty(),
                "expected no problems for a library with system deps only");

  fs::remove_all(root, ec);
  return ok;
}

// The effect and parameter selector strings are part of the Maxine ABI. These
// are the values in `nvVideoEffects.h` and in the per-feature headers of the
// VFX SDK Core 1.x (they are the same in the 0.7/0.8 SDK).
bool TestVfxSelectorsMatchTheSdkHeaders() {
  namespace vfx = studiocast::maxine::vfx;

  auto same = [](const char *got, const char *want, const char *what) {
    return Require(std::string(got) == want,
                   std::string("expected ") + what + " to be '" + want +
                       "', got '" + got + "'");
  };

  bool ok = true;
  ok &= same(vfx::NVVFX_FX_GREEN_SCREEN, "GreenScreen", "the green screen id");
  ok &= same(vfx::NVVFX_FX_BGBLUR, "BackgroundBlur", "the background blur id");
  ok &= same(vfx::NVVFX_FX_DENOISING, "Denoising", "the denoising id");
  ok &= same(vfx::NVVFX_FX_TRANSFER, "Transfer", "the transfer id");
  ok &= same(vfx::NVVFX_FX_RELIGHTING, "Relighting", "the relighting id");
  ok &= same(vfx::NVVFX_FX_AIGS_RELIGHTING, "AIGSRelighting",
             "the AIGS relighting id");

  ok &= same(vfx::NVVFX_MODEL_DIRECTORY, "ModelDir", "the model dir selector");
  ok &= same(vfx::NVVFX_CUDA_STREAM, "CudaStream", "the CUDA stream selector");
  ok &= same(vfx::NVVFX_STRENGTH, "Strength", "the strength selector");
  ok &= same(vfx::NVVFX_MODE, "Mode", "the mode selector");
  ok &= same(vfx::NVVFX_TEMPORAL, "Temporal", "the temporal selector");
  ok &= same(vfx::NVVFX_STATE, "State", "the state selector");
  ok &= same(vfx::NVVFX_STATE_SIZE, "StateSize", "the state size selector");
  ok &= same(vfx::NVVFX_STATE_COUNT, "NumStateObjects",
             "the state count selector");
  ok &= same(vfx::NVVFX_INPUT_IMAGE, "SrcImage0", "the input image selector");
  ok &= same(vfx::NVVFX_INPUT_IMAGE_1, "SrcImage1",
             "the second input image selector");
  ok &= same(vfx::NVVFX_OUTPUT_IMAGE, "DstImage0", "the output image selector");
  return ok;
}

// Effects only know the library they loaded, so they must be able to derive
// the models directory from it in both layouts.
bool TestModelsDirForLibrary() {
  const fs::path root =
      fs::temp_directory_path() /
      ("studiocast-maxine-modeldir-" + std::to_string(::getpid()));
  std::error_code ec;
  fs::remove_all(root, ec);

  const fs::path new_root = root / "ARSDK";
  const fs::path old_root = root / "VideoFX";
  fs::create_directories(new_root / "lib" / "models", ec);
  fs::create_directories(old_root / "models", ec);
  fs::create_directories(old_root / "lib", ec);
  if (ec || !Touch(new_root / "lib" / "libnvARPose.so") ||
      !Touch(old_root / "lib" / "libVideoFX.so")) {
    std::cerr << "failed to create models dir layout\n";
    fs::remove_all(root, ec);
    return false;
  }

  bool ok = true;
  ok &= Require(studiocast::maxine::ModelsDirForLibrary(
                    new_root / "lib" / "libnvARPose.so") ==
                    new_root / "lib" / "models",
                "expected the SDK Core 1.x models dir next to the library");
  ok &= Require(studiocast::maxine::ModelsDirForLibrary(
                    old_root / "lib" / "libVideoFX.so") == old_root / "models",
                "expected the legacy models dir one level up");
  ok &= Require(studiocast::maxine::ModelsDirForLibrary({}).empty(),
                "expected an empty result for an empty library path");

  fs::remove_all(root, ec);
  return ok;
}

// The AR feature ids come from the per-feature headers of the AR SDK.
bool TestArFeatureIdsMatchTheSdkHeaders() {
  namespace ar = studiocast::maxine::ar;

  auto same = [](const char *got, const char *want, const char *what) {
    return Require(std::string(got) == want,
                   std::string("expected ") + what + " to be '" + want +
                       "', got '" + got + "'");
  };

  bool ok = true;
  ok &= same(ar::NVAR_FEATURE_GAZE_REDIRECTION, "GazeRedirection",
             "the gaze redirection id");
  ok &= same(ar::NVAR_FEATURE_FACE_BOX_DETECTION, "FaceBoxDetection",
             "the face box detection id");
  ok &= same(ar::NVAR_FEATURE_BODY_DETECTION, "BodyDetection",
             "the body detection id");
  return ok;
}

// ---------------------------------------------------------------------------
// Crafted ELF images for the DT_NEEDED reader in sdk_runtime.cpp.
//
// The reader is pointed at every file that looks like an SDK library, and one
// of those can be truncated, half written or plain wrong. No linker makes the
// shapes below, so they are written by hand.
// ---------------------------------------------------------------------------

// Where each part of the image sits. The single PT_LOAD starts at file offset
// 0 with virtual address 0, so an address is also an offset.
constexpr std::size_t kElfPhOff = 64;
constexpr std::size_t kElfDynOff = 512;
constexpr std::size_t kElfStrOff = 1024;
constexpr std::size_t kElfImageSize = 4096;

// A well-formed ELF64 shared object that names one dependency.
std::vector<char> BuildElfImage(const std::string &soname) {
  std::vector<char> buf(kElfImageSize, 0);
  auto put = [&buf](std::size_t at, const void *src, std::size_t n) {
    std::memcpy(buf.data() + at, src, n);
  };

  // A string table starts with a NUL, so offset 0 is the empty name.
  std::string strtab(1, '\0');
  const std::size_t name_off = strtab.size();
  strtab += soname;
  strtab.push_back('\0');
  put(kElfStrOff, strtab.data(), strtab.size());

  std::vector<Elf64_Dyn> dyns(3);
  dyns[0].d_tag = DT_STRTAB;
  dyns[0].d_un.d_ptr = kElfStrOff;
  dyns[1].d_tag = DT_NEEDED;
  dyns[1].d_un.d_val = name_off;
  dyns[2].d_tag = DT_NULL;
  put(kElfDynOff, dyns.data(), dyns.size() * sizeof(Elf64_Dyn));

  std::vector<Elf64_Phdr> phdrs(2);
  phdrs[0].p_type = PT_LOAD;
  phdrs[0].p_filesz = kElfImageSize;
  phdrs[0].p_memsz = kElfImageSize;
  phdrs[1].p_type = PT_DYNAMIC;
  phdrs[1].p_offset = kElfDynOff;
  phdrs[1].p_vaddr = kElfDynOff;
  phdrs[1].p_filesz = dyns.size() * sizeof(Elf64_Dyn);
  phdrs[1].p_memsz = phdrs[1].p_filesz;
  put(kElfPhOff, phdrs.data(), phdrs.size() * sizeof(Elf64_Phdr));

  Elf64_Ehdr eh{};
  std::memcpy(eh.e_ident, ELFMAG, SELFMAG);
  eh.e_ident[EI_CLASS] = ELFCLASS64;
  eh.e_ident[EI_DATA] = ELFDATA2LSB;
  eh.e_ident[EI_VERSION] = EV_CURRENT;
  eh.e_type = ET_DYN;
  eh.e_machine = EM_X86_64;
  eh.e_version = EV_CURRENT;
  eh.e_phoff = kElfPhOff;
  eh.e_phentsize = sizeof(Elf64_Phdr);
  eh.e_phnum = static_cast<Elf64_Half>(phdrs.size());
  put(0, &eh, sizeof(eh));
  return buf;
}

Elf64_Ehdr ElfHeaderOf(const std::vector<char> &buf) {
  Elf64_Ehdr eh{};
  std::memcpy(&eh, buf.data(), sizeof(eh));
  return eh;
}

void SetElfHeader(std::vector<char> *buf, const Elf64_Ehdr &eh) {
  std::memcpy(buf->data(), &eh, sizeof(eh));
}

Elf64_Phdr ElfProgramHeaderOf(const std::vector<char> &buf, std::size_t index) {
  Elf64_Phdr ph{};
  std::memcpy(&ph, buf.data() + kElfPhOff + index * sizeof(Elf64_Phdr),
              sizeof(ph));
  return ph;
}

void SetElfProgramHeader(std::vector<char> *buf, std::size_t index,
                         const Elf64_Phdr &ph) {
  std::memcpy(buf->data() + kElfPhOff + index * sizeof(Elf64_Phdr), &ph,
              sizeof(ph));
}

// A broken ELF file must give no dependencies, and must not make the reader
// touch memory outside the buffer it read the file into. The out-of-bounds
// read itself is caught by an AddressSanitizer build; what this test pins is
// that every one of these shapes is refused.
bool TestMalformedElfIsRejected() {
  const fs::path root =
      fs::temp_directory_path() /
      ("studiocast-maxine-elf-" + std::to_string(::getpid()));
  std::error_code ec;
  fs::remove_all(root, ec);
  fs::create_directories(root, ec);
  if (ec) {
    std::cerr << "failed to create the ELF test directory: " << ec.message()
              << "\n";
    return false;
  }

  const std::string kSoname = "libstudiocast-not-a-real-library.so.1";
  auto deps_of = [&root, &kSoname](const std::string &name,
                                   const std::vector<char> &image) -> size_t {
    const fs::path p = root / name;
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    out.write(image.data(), static_cast<std::streamsize>(image.size()));
    out.close();
    return studiocast::maxine::PreloadSdkRuntime(p).dependencies.size();
  };

  bool ok = true;

  // Control: the hand-built image must still parse, or the checks below would
  // pass for the wrong reason.
  ok &= Require(deps_of("good.so", BuildElfImage(kSoname)) == 1,
                "expected the well-formed ELF image to name one dependency");

  {
    // e_phoff near the end of the 64-bit range: `offset + sizeof(T)` used to
    // wrap and pass the bounds check.
    auto image = BuildElfImage(kSoname);
    auto eh = ElfHeaderOf(image);
    eh.e_phoff = 0xFFFFFFFFFFFFFFF0ULL;
    eh.e_phnum = 1;
    SetElfHeader(&image, eh);
    ok &= Require(deps_of("phoff_overflow.so", image) == 0,
                  "expected a wrapped e_phoff to give no dependencies");
  }
  {
    // The file stops in the middle of the program header table.
    auto image = BuildElfImage(kSoname);
    image.resize(100);
    ok &= Require(deps_of("truncated.so", image) == 0,
                  "expected a truncated ELF file to give no dependencies");
  }
  {
    // The dynamic segment starts near the end of the 64-bit range.
    auto image = BuildElfImage(kSoname);
    auto ph = ElfProgramHeaderOf(image, 1);
    ph.p_offset = 0xFFFFFFFFFFFFFFF0ULL;
    ph.p_filesz = 64;
    SetElfProgramHeader(&image, 1, ph);
    ok &= Require(deps_of("dyn_offset_overflow.so", image) == 0,
                  "expected a wrapped p_offset to give no dependencies");
  }
  {
    // The dynamic segment claims far more bytes than the file holds.
    auto image = BuildElfImage(kSoname);
    auto ph = ElfProgramHeaderOf(image, 1);
    ph.p_filesz = 0xFFFFFFFFFFFFFF00ULL;
    SetElfProgramHeader(&image, 1, ph);
    ok &= Require(deps_of("dyn_huge_filesz.so", image) == 0,
                  "expected an oversized p_filesz to give no dependencies");
  }
  {
    // A big-endian file: every field this reader takes would be byte-swapped.
    auto image = BuildElfImage(kSoname);
    image[EI_DATA] = ELFDATA2MSB;
    ok &= Require(deps_of("big_endian.so", image) == 0,
                  "expected a big-endian ELF file to be refused");
  }
  {
    // PN_XNUM keeps the real count in a section header. The table below is
    // large enough that the reader would otherwise walk it and succeed.
    auto image = BuildElfImage(kSoname);
    const std::size_t big = kElfPhOff + 65535u * sizeof(Elf64_Phdr);
    image.resize(big, 0);
    auto load = ElfProgramHeaderOf(image, 0);
    load.p_filesz = big;
    load.p_memsz = big;
    SetElfProgramHeader(&image, 0, load);
    auto eh = ElfHeaderOf(image);
    eh.e_phnum = PN_XNUM;
    SetElfHeader(&image, eh);
    ok &= Require(deps_of("pn_xnum.so", image) == 0,
                  "expected PN_XNUM to be refused");
  }
  {
    // A DT_NEEDED name offset that wraps past the end of the string table.
    auto image = BuildElfImage(kSoname);
    Elf64_Dyn d{};
    const std::size_t at = kElfDynOff + sizeof(Elf64_Dyn);
    std::memcpy(&d, image.data() + at, sizeof(d));
    d.d_un.d_val = 0xFFFFFFFFFFFFFFF0ULL;
    std::memcpy(image.data() + at, &d, sizeof(d));
    ok &= Require(deps_of("strtab_overflow.so", image) == 0,
                  "expected a wrapped string table offset to name nothing");
  }

  fs::remove_all(root, ec);
  return ok;
}

} // namespace

int main(int argc, char **argv) {
  if (std::getenv("STUDIOCAST_AFX_LOADER_PRIORITY_CHILD")) {
    return RunAfxLoaderPriorityChild() ? 0 : 1;
  }

  if (!TestCurrentLinuxMaxineLibraryNamesResolve()) {
    std::cout << "[FAIL] current Linux Maxine library names resolve\n";
    return 1;
  }

  std::cout << "[PASS] current Linux Maxine library names resolve\n";

  if (!TestSdkCore1xModelsDirResolves()) {
    std::cout << "[FAIL] SDK Core 1.x models dir resolves\n";
    return 1;
  }
  std::cout << "[PASS] SDK Core 1.x models dir resolves\n";

  if (!TestLegacyModelsDirStillWins()) {
    std::cout << "[FAIL] legacy models dir still wins\n";
    return 1;
  }
  std::cout << "[PASS] legacy models dir still wins\n";

  if (!TestSdkRuntimePreloadMakesBundledDepsResolvable()) {
    std::cout << "[FAIL] SDK runtime pre-load makes bundled deps resolvable\n";
    return 1;
  }
  std::cout << "[PASS] SDK runtime pre-load makes bundled deps resolvable\n";

  if (!TestSdkRuntimePreloadSharesOneEntryForTheInferredRoot()) {
    std::cout << "[FAIL] SDK runtime pre-load shares one entry for the "
                 "inferred root\n";
    return 1;
  }
  std::cout << "[PASS] SDK runtime pre-load shares one entry for the inferred "
               "root\n";

  if (!TestSdkRuntimePreloadCachesPerSdkRoot()) {
    std::cout << "[FAIL] SDK runtime pre-load caches per SDK root\n";
    return 1;
  }
  std::cout << "[PASS] SDK runtime pre-load caches per SDK root\n";

  if (!TestSdkRuntimePreloadIsSharedAcrossThreads()) {
    std::cout << "[FAIL] SDK runtime pre-load is shared across threads\n";
    return 1;
  }
  std::cout << "[PASS] SDK runtime pre-load is shared across threads\n";

  if (!TestSdkRuntimePreloadSkipsSystemLibraries()) {
    std::cout << "[FAIL] SDK runtime pre-load skips system libraries\n";
    return 1;
  }
  std::cout << "[PASS] SDK runtime pre-load skips system libraries\n";

  if (!TestVfxSelectorsMatchTheSdkHeaders()) {
    std::cout << "[FAIL] VFX selectors match the SDK headers\n";
    return 1;
  }
  std::cout << "[PASS] VFX selectors match the SDK headers\n";

  if (!TestArFeatureIdsMatchTheSdkHeaders()) {
    std::cout << "[FAIL] AR feature ids match the SDK headers\n";
    return 1;
  }
  std::cout << "[PASS] AR feature ids match the SDK headers\n";

  if (!TestModelsDirForLibrary()) {
    std::cout << "[FAIL] models dir resolves from the library path\n";
    return 1;
  }
  std::cout << "[PASS] models dir resolves from the library path\n";

  if (!TestMalformedElfIsRejected()) {
    std::cout << "[FAIL] malformed ELF files are rejected\n";
    return 1;
  }
  std::cout << "[PASS] malformed ELF files are rejected\n";

  if (argc <= 0 || !argv || !argv[0] ||
      !TestAfxLoaderPrefersExplicitSdkRootBeforeBareLoaderPath(argv[0])) {
    std::cout << "[FAIL] AFX loader prefers explicit SDK root before bare "
                 "loader path\n";
    return 1;
  }

  std::cout << "[PASS] AFX loader prefers explicit SDK root before bare "
               "loader path\n";
  return 0;
}
