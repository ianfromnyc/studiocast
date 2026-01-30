include_guard(GLOBAL)

function(studiocast_generate_version_header out_header)
  set(STUDIOCAST_VERSION "${PROJECT_VERSION}")
  set(STUDIOCAST_GIT_SHA "unknown")

  find_package(Git QUIET)
  if(GIT_FOUND AND EXISTS "${CMAKE_SOURCE_DIR}/.git")
    execute_process(
      COMMAND "${GIT_EXECUTABLE}" rev-parse --short HEAD
      WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
      RESULT_VARIABLE _git_result
      OUTPUT_VARIABLE _git_output
      OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    if(_git_result EQUAL 0)
      set(STUDIOCAST_GIT_SHA "${_git_output}")
    endif()
  endif()

  get_filename_component(_out_dir "${out_header}" DIRECTORY)
  file(MAKE_DIRECTORY "${_out_dir}")

  configure_file(
    "${CMAKE_SOURCE_DIR}/src/version.h.in"
    "${out_header}"
    @ONLY
  )
endfunction()
