cmake_minimum_required(VERSION 3.22)

project(StudioCast
        VERSION 0.1.0
        DESCRIPTION "Broadcast-style audio/video effects manager for Linux (Maxine-ready)"
        LANGUAGES CXX
)

foreach (var IN ITEMS CPLUS_INCLUDE_PATH CPATH C_INCLUDE_PATH)
    if (DEFINED ENV{${var}} AND NOT "$ENV{${var}}" STREQUAL "")
        message(WARNING
                "${var} is set to '$ENV{${var}}'. This can break the C++ toolchain by forcing mismatched headers. "
                "Consider unsetting it for StudioCast builds.")
    endif ()
endforeach ()


list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(ProjectOptions)
include(CompilerWarnings)
include(Sanitizers)
include(GenerateVersionHeader)

studiocast_setup_options()
studiocast_global_options()

# Qt: prefer Qt6, fall back to Qt5
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_AUTOUIC ON)

# Generate version header at configure time
set(STUDIOCAST_GENERATED_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
studiocast_generate_version_header(
        "${STUDIOCAST_GENERATED_DIR}/studiocast/version.h"
)

# -----------------------------
# Core library (no Qt deps)
# -----------------------------
add_library(studiocast_core
        src/core/probe/probe.h
        src/core/probe/probe.cpp
        src/core/util/exec.h
        src/core/util/exec.cpp
        src/core/util/fs.h
        src/core/util/fs.cpp
        src/core/util/os_release.h
        src/core/util/os_release.cpp
        src/core/util/strings.h
        src/core/util/strings.cpp
        src/core/util/proc.h
        src/core/util/proc.cpp
        src/core/util/xdg.h
        src/core/util/xdg.cpp
        src/core/maxine/gpu.h
        src/core/maxine/gpu.cpp
        src/core/audio/pulse/pactl.h
        src/core/audio/pulse/pactl.cpp
        src/core/audio/virtual_mic_state.h
        src/core/audio/virtual_mic_state.cpp
        src/core/audio/virtual_mic.h
        src/core/audio/virtual_mic.cpp
        src/core/video/v4l2loopback.h
        src/core/video/v4l2loopback.cpp
        src/core/video/v4l2_writer.h
        src/core/video/v4l2_writer.cpp
        src/core/video/pattern.h
        src/core/video/pattern.cpp
        src/core/video/feed.h
        src/core/video/feed.cpp
        src/core/video/convert.h
        src/core/video/convert.cpp
        src/core/video/v4l2_capture.h
        src/core/video/v4l2_capture.cpp
        src/core/video/camera_pipeline.h
        src/core/video/camera_pipeline.cpp
        src/core/video/virtual_camera_service.h
        src/core/video/virtual_camera_service.cpp

)

set_target_properties(studiocast_core PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
)

target_include_directories(studiocast_core PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${STUDIOCAST_GENERATED_DIR}"
)

studiocast_set_project_warnings(studiocast_core)
studiocast_enable_sanitizers(studiocast_core)

# -----------------------------
# GUI executable
# -----------------------------
add_executable(studiocast
        src/main.cpp
        src/gui/theme.h
        src/gui/theme.cpp
        src/gui/main_window.h
        src/gui/main_window.cpp
        src/gui/pages/audio_page.h
        src/gui/pages/audio_page.cpp
        src/gui/pages/video_page.h
        src/gui/pages/video_page.cpp
        src/core/probe/probe.h
        src/core/config/settings.h
        src/core/config/settings.cpp
)

target_link_libraries(studiocast PRIVATE
        Qt${QT_VERSION_MAJOR}::Widgets
        studiocast_core
)

target_include_directories(studiocast PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${STUDIOCAST_GENERATED_DIR}"
)

studiocast_set_project_warnings(studiocast)
studiocast_enable_sanitizers(studiocast)

set_target_properties(studiocast PROPERTIES
        OUTPUT_NAME "studiocast"
)

# -----------------------------
# Probe tool 
# -----------------------------
add_executable(studiocast-probe
        src/tools/studiocast_probe_main.cpp
)

set_target_properties(studiocast-probe PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
)

target_link_libraries(studiocast-probe PRIVATE studiocast_core)

target_include_directories(studiocast-probe PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${STUDIOCAST_GENERATED_DIR}"
)

studiocast_set_project_warnings(studiocast-probe)
studiocast_enable_sanitizers(studiocast-probe)

# -----------------------------
# Maxine helper tool 
# -----------------------------
add_executable(studiocast-maxine
        src/tools/studiocast_maxine_main.cpp
)

set_target_properties(studiocast-maxine PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

target_link_libraries(studiocast-maxine PRIVATE studiocast_core)

target_include_directories(studiocast-maxine PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${STUDIOCAST_GENERATED_DIR}"
)

studiocast_set_project_warnings(studiocast-maxine)
studiocast_enable_sanitizers(studiocast-maxine)

# -----------------------------
# Audio tool 
# -----------------------------
add_executable(studiocast-audio
        src/tools/studiocast_audio_main.cpp
)

target_link_libraries(studiocast-audio PRIVATE studiocast_core)

target_include_directories(studiocast-audio PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${STUDIOCAST_GENERATED_DIR}"
)

studiocast_set_project_warnings(studiocast-audio)
studiocast_enable_sanitizers(studiocast-audio)

set_target_properties(studiocast-audio PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
)

# -----------------------------
# Video tool
# -----------------------------
add_executable(studiocast-video
        src/tools/studiocast_video_main.cpp
)

target_link_libraries(studiocast-video PRIVATE studiocast_core)

target_include_directories(studiocast-video PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${STUDIOCAST_GENERATED_DIR}"
)

studiocast_set_project_warnings(studiocast-video)
studiocast_enable_sanitizers(studiocast-video)

set_target_properties(studiocast-video PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
)


# -----------------------------
# Background daemon
# -----------------------------
add_executable(studiocastd
        src/daemon/studiocastd_main.cpp
)

target_link_libraries(studiocastd PRIVATE studiocast_core)

target_include_directories(studiocastd PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${STUDIOCAST_GENERATED_DIR}"
)

studiocast_set_project_warnings(studiocastd)
studiocast_enable_sanitizers(studiocastd)

set_target_properties(studiocastd PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
        OUTPUT_NAME "studiocastd"
)


# -----------------------------
# Install step
# -----------------------------
include(GNUInstallDirs)
install(TARGETS studiocast studiocast-probe studiocast-maxine studiocast-audio studiocast-video
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

install(TARGETS studiocastd
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
