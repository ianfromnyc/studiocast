include_guard(GLOBAL)

option(STUDIOCAST_ENABLE_WERROR "Treat warnings as errors" OFF)
option(STUDIOCAST_ENABLE_SANITIZERS "Enable sanitizers (ASan/UBSan) on supported compilers" OFF)
option(STUDIOCAST_ENABLE_LTO "Enable link-time optimization (IPO/LTO) if supported" OFF)
option(STUDIOCAST_ENABLE_CUDA_KERNELS "Build optional CUDA .cu kernels (requires CUDA toolkit)" OFF)
option(STUDIOCAST_BUILD_BENCHMARKS "Build developer benchmark tools" OFF)

set(_studiocast_default_open_cuda OFF)
if(UNIX AND NOT APPLE)
  set(_studiocast_default_open_cuda ON)
endif()
option(STUDIOCAST_ENABLE_OPEN_CUDA "Enable Open CUDA backend (requires ONNX Runtime + CUDA EP)" ${_studiocast_default_open_cuda})

set(_studiocast_default_open_audio OFF)
if(UNIX AND NOT APPLE)
  set(_studiocast_default_open_audio ON)
endif()
option(STUDIOCAST_ENABLE_OPEN_AUDIO "Enable Open Audio backend (requires ONNX Runtime; CPU EP baseline)" ${_studiocast_default_open_audio})

# Optional dependency used for Open Video Eye Contact (face landmarks via dlib).
#
# If disabled or dlib is not found, the open-source Eye Contact backend will be
# unavailable (Maxine AR may still provide Eye Contact when present).
option(STUDIOCAST_ENABLE_DLIB "Enable dlib support (face landmarks)" ON)

# Optional dependency used for an extra, optimized RGB/YUV conversion backend.
#
#   AUTO  Use libyuv when the machine has it, and fall back to the built-in
#         scalar, SSSE3 and AVX2 backends when it does not. This is the default.
#   ON    Require libyuv. A missing libyuv is a configure error. A package
#         build passes this, so the source package decides what the binary gets.
#   OFF   Skip the detection, even on a machine that has libyuv.
set(STUDIOCAST_ENABLE_LIBYUV "AUTO" CACHE STRING
    "Use libyuv for RGB/YUV conversion: AUTO, ON or OFF")
set_property(CACHE STUDIOCAST_ENABLE_LIBYUV PROPERTY STRINGS AUTO ON OFF)

function(studiocast_setup_options)
  # Intentionally light for now; expand later
endfunction()

function(studiocast_global_options)
  set(CMAKE_CXX_STANDARD 20 PARENT_SCOPE)
  set(CMAKE_CXX_STANDARD_REQUIRED ON PARENT_SCOPE)
  set(CMAKE_CXX_EXTENSIONS OFF PARENT_SCOPE)

  # Helpful for clangd/CLion indexing
  set(CMAKE_EXPORT_COMPILE_COMMANDS ON PARENT_SCOPE)

  if(STUDIOCAST_ENABLE_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT _ipo_supported OUTPUT _ipo_error)
    if(_ipo_supported)
      set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE PARENT_SCOPE)
    else()
      message(WARNING "IPO/LTO not supported: ${_ipo_error}")
    endif()
  endif()
endfunction()
