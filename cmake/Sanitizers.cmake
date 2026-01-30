include_guard(GLOBAL)

function(studiocast_enable_sanitizers target)
  if(NOT STUDIOCAST_ENABLE_SANITIZERS)
    return()
  endif()

  if (MSVC)
    message(WARNING "Sanitizers are not configured for MSVC in this repo yet.")
    return()
  endif()

  # Address + Undefined is a solid default for early development.
  target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(${target} PRIVATE -fsanitize=address,undefined)
endfunction()
