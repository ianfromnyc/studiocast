include_guard(GLOBAL)

include(CheckCXXSourceCompiles)

function(studiocast_configure_onnxruntime out_found out_target)
  set(_found FALSE)
  set(_target "")

  # 1) Prefer a CMake package config if available.
  find_package(onnxruntime CONFIG QUIET)
  if (onnxruntime_FOUND)
    if (TARGET onnxruntime::onnxruntime)
      set(_found TRUE)
      set(_target onnxruntime::onnxruntime)
    elseif (TARGET onnxruntime::onnxruntime_shared)
      set(_found TRUE)
      set(_target onnxruntime::onnxruntime_shared)
    elseif (TARGET onnxruntime::onnxruntime_static)
      set(_found TRUE)
      set(_target onnxruntime::onnxruntime_static)
    elseif (TARGET onnxruntime)
      set(_found TRUE)
      set(_target onnxruntime)
    elseif (TARGET onnxruntime_shared)
      set(_found TRUE)
      set(_target onnxruntime_shared)
    elseif (TARGET onnxruntime_static)
      set(_found TRUE)
      set(_target onnxruntime_static)
    endif()
  endif()

  # 2) Fall back to pkg-config (common for distro packages).
  if (NOT _found)
    if (PkgConfig_FOUND)
      pkg_check_modules(ONNXRUNTIME QUIET IMPORTED_TARGET onnxruntime)
      if (ONNXRUNTIME_FOUND)
        set(_found TRUE)
        set(_target PkgConfig::ONNXRUNTIME)
      endif()
    endif()
  endif()

  # 3) Last resort: user-provided root path.
  if (NOT _found AND DEFINED ONNXRUNTIME_ROOT)
    find_path(ONNXRUNTIME_INCLUDE_DIR
      NAMES onnxruntime_cxx_api.h
      HINTS "${ONNXRUNTIME_ROOT}"
      PATH_SUFFIXES include include/onnxruntime
    )

    find_library(ONNXRUNTIME_LIBRARY
      NAMES onnxruntime
      HINTS "${ONNXRUNTIME_ROOT}"
      PATH_SUFFIXES lib lib64
    )

    if (ONNXRUNTIME_INCLUDE_DIR AND ONNXRUNTIME_LIBRARY)
      if (NOT TARGET studiocast_onnxruntime)
        add_library(studiocast_onnxruntime UNKNOWN IMPORTED)
        set_target_properties(studiocast_onnxruntime PROPERTIES
          IMPORTED_LOCATION "${ONNXRUNTIME_LIBRARY}"
          INTERFACE_INCLUDE_DIRECTORIES "${ONNXRUNTIME_INCLUDE_DIR}"
        )
      endif()
      if (NOT TARGET studiocast::onnxruntime)
        add_library(studiocast::onnxruntime ALIAS studiocast_onnxruntime)
      endif()

      set(_found TRUE)
      set(_target studiocast::onnxruntime)
    endif()
  endif()

  # Feature probe: CUDA EP V2 provider options.
  # We want to use OrtApi::CreateCUDAProviderOptions + SessionOptionsAppendExecutionProvider_CUDA_V2
  # where headers support it, and keep compatibility where they don't.
  set(_has_cuda_ep_v2 FALSE)
  if (_found)
    set(_probe_src [==[
#include <onnxruntime_cxx_api.h>

int main() {
  auto& api = Ort::GetApi();

  OrtCUDAProviderOptionsV2* opts = nullptr;
  api.CreateCUDAProviderOptions(&opts);

  Ort::SessionOptions so;
  api.SessionOptionsAppendExecutionProvider_CUDA_V2(so, opts);
  api.ReleaseCUDAProviderOptions(opts);
  return 0;
}
]==])

    # Best-effort propagation of include dirs / compile defs from the chosen target.
    # check_cxx_source_compiles does not automatically add target usage requirements.
    get_target_property(_ort_includes "${_target}" INTERFACE_INCLUDE_DIRECTORIES)
    if (NOT _ort_includes)
      set(_ort_includes "")
    endif()
    get_target_property(_ort_compile_defs "${_target}" INTERFACE_COMPILE_DEFINITIONS)
    if (NOT _ort_compile_defs)
      set(_ort_compile_defs "")
    endif()

    set(_ort_includes_sanitized "")
    foreach (d IN LISTS _ort_includes)
      if (NOT d MATCHES "\\$<")
        list(APPEND _ort_includes_sanitized "${d}")
      endif()
    endforeach()

    set(_ort_compile_defs_sanitized "")
    foreach (d IN LISTS _ort_compile_defs)
      if (NOT d MATCHES "\\$<")
        list(APPEND _ort_compile_defs_sanitized "${d}")
      endif()
    endforeach()

    set(_old_required_includes "${CMAKE_REQUIRED_INCLUDES}")
    set(_old_required_definitions "${CMAKE_REQUIRED_DEFINITIONS}")
    set(_old_try_compile_target_type "${CMAKE_TRY_COMPILE_TARGET_TYPE}")

    set(CMAKE_REQUIRED_INCLUDES "${_ort_includes_sanitized}")
    set(CMAKE_REQUIRED_DEFINITIONS "${_ort_compile_defs_sanitized}")
    # Compile-only feature probe: avoid link failures when only headers are present.
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

    check_cxx_source_compiles("${_probe_src}" _studiocast_ort_cuda_ep_v2_probe_ok)

    set(CMAKE_REQUIRED_INCLUDES "${_old_required_includes}")
    set(CMAKE_REQUIRED_DEFINITIONS "${_old_required_definitions}")
    set(CMAKE_TRY_COMPILE_TARGET_TYPE "${_old_try_compile_target_type}")

    if (_studiocast_ort_cuda_ep_v2_probe_ok)
      set(_has_cuda_ep_v2 TRUE)
    endif()
  endif()

  if (_has_cuda_ep_v2)
    add_compile_definitions(STUDIOCAST_ORT_HAS_CUDA_EP_V2=1)
    message(STATUS "ONNX Runtime CUDA EP V2 provider options: available (STUDIOCAST_ORT_HAS_CUDA_EP_V2=1)")
  else()
    add_compile_definitions(STUDIOCAST_ORT_HAS_CUDA_EP_V2=0)
    if (_found)
      message(STATUS "ONNX Runtime CUDA EP V2 provider options: unavailable (STUDIOCAST_ORT_HAS_CUDA_EP_V2=0)")
    else()
      message(STATUS "ONNX Runtime CUDA EP V2 provider options: unavailable (onnxruntime not found; STUDIOCAST_ORT_HAS_CUDA_EP_V2=0)")
    endif()
  endif()

  set(${out_found} ${_found} PARENT_SCOPE)
  set(${out_target} "${_target}" PARENT_SCOPE)
endfunction()
