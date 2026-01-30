include_guard(GLOBAL)

option(STUDIOCAST_ENABLE_WERROR "Treat warnings as errors" OFF)
option(STUDIOCAST_ENABLE_SANITIZERS "Enable sanitizers (ASan/UBSan) on supported compilers" OFF)
option(STUDIOCAST_ENABLE_LTO "Enable link-time optimization (IPO/LTO) if supported" OFF)

function(studiocast_setup_options)
  # Intentionally light in Phase 0; expand later.
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
