include_guard(GLOBAL)

function(studiocast_set_project_warnings target)
  if (MSVC)
    target_compile_options(${target} PRIVATE /W4)
    if (STUDIOCAST_ENABLE_WERROR)
      target_compile_options(${target} PRIVATE /WX)
    endif()
  else()
    target_compile_options(${target} PRIVATE
      -Wall
      -Wextra
      -Wpedantic
      -Wconversion
      -Wsign-conversion
      -Wshadow
      -Wnon-virtual-dtor
    )
    if (STUDIOCAST_ENABLE_WERROR)
      target_compile_options(${target} PRIVATE -Werror)
    endif()
  endif()
endfunction()
