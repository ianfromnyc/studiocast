#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace studiocast::maxine {

// One dependency of a Maxine SDK library.
struct SdkDependencyLoad {
  std::string soname; // e.g. "libnvinfer.so.10"
  // Where the soname came from. Empty when the system loader keeps it.
  std::filesystem::path resolved;
  bool loaded = false; // true when StudioCast pre-loaded the SDK copy
  std::string error;   // set when the SDK copy could not be loaded
};

// What `PreloadSdkRuntime` did for one SDK library.
struct SdkRuntimeReport {
  std::filesystem::path library;
  std::filesystem::path sdk_root;
  std::vector<std::filesystem::path> search_dirs;
  std::vector<SdkDependencyLoad> dependencies;
  std::vector<std::string> problems;

  // Short, one-line summary for `studiocast-maxine doctor`.
  std::string Summary() const;
};

// Makes the runtime that the Maxine SDK ships loadable without
// LD_LIBRARY_PATH.
//
// The SDK Core 1.x puts its CUDA 12 and TensorRT 10 runtime under
// `<sdk_root>/external/cuda/lib` and `<sdk_root>/external/tensorrt/lib`, and
// its own support libraries next to the target library. None of those
// directories is on the loader path, and the SDK libraries carry no usable
// RPATH, so a plain dlopen fails. This function reads the DT_NEEDED list of
// `library`, resolves each soname against the SDK directories, and dlopens the
// SDK copies with RTLD_NOW | RTLD_LOCAL in dependency order. Local is enough:
// the loader still satisfies a later DT_NEEDED from the link map by soname,
// and the SDK's own TensorRT and CUDA stay out of the process-wide namespace,
// where ONNX Runtime would otherwise bind to them.
//
// Core system libraries (libc, libstdc++, ...) and the NVIDIA driver library
// (libcuda.so.1) always stay with the system loader. So does any soname the
// SDK does not ship, and any library the process already has open.
//
// Handles stay open for the life of the process. Call this before you dlopen
// `library`. Results are cached per library path and SDK root, so repeated
// calls are cheap and two roots for the same library keep their own report.
//
// `sdk_root` may be empty, in which case it is taken from the library path
// before the cache is read, so a call without a root and a call that names
// that same root share one report.
const SdkRuntimeReport &
PreloadSdkRuntime(const std::filesystem::path &library,
                  const std::filesystem::path &sdk_root = {});

// Same as above, but does nothing when `library` is a bare soname rather than a
// path to a file. Loaders use it because they also try bare names through the
// system loader path.
void PreloadSdkRuntimeIfLocal(const std::filesystem::path &library);

} // namespace studiocast::maxine
