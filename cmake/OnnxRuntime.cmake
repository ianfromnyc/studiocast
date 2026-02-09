include_guard(GLOBAL)

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

  set(${out_found} ${_found} PARENT_SCOPE)
  set(${out_target} "${_target}" PARENT_SCOPE)
endfunction()
