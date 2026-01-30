cmake_minimum_required(VERSION 3.22)

project(StudioCast
  VERSION 0.1.0
  DESCRIPTION "Broadcast-style audio/video effects manager for Linux (Maxine-ready)"
  LANGUAGES CXX
)

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

# Generate version header at build time
studiocast_generate_version_header(
  "${CMAKE_CURRENT_BINARY_DIR}/generated/studiocast/version.h"
)

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
)

target_link_libraries(studiocast PRIVATE Qt${QT_VERSION_MAJOR}::Widgets)

target_include_directories(studiocast PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/src"
  "${CMAKE_CURRENT_BINARY_DIR}/generated"
)

studiocast_set_project_warnings(studiocast)
studiocast_enable_sanitizers(studiocast)

set_target_properties(studiocast PROPERTIES
  OUTPUT_NAME "studiocast"
)

include(GNUInstallDirs)
install(TARGETS studiocast RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
