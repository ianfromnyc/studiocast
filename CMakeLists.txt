cmake_minimum_required(VERSION 3.22)

project(StudioCast
        VERSION 0.1.0
        DESCRIPTION "Broadcast-style audio/video effects manager for Linux (Maxine-ready)"
        LANGUAGES CXX
)

include(CTest)

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
include(OnnxRuntime)

studiocast_setup_options()
studiocast_global_options()

# Qt: prefer Qt6, fall back to Qt5
find_package(QT NAMES Qt6 Qt5 REQUIRED COMPONENTS Widgets)
find_package(Qt${QT_VERSION_MAJOR} REQUIRED COMPONENTS Widgets)

# PulseAudio (libpulse-simple) for real-time audio capture/playback.
# Keep optional so the repo can still build on minimal systems.
find_package(PkgConfig REQUIRED)
pkg_check_modules(PULSE_SIMPLE QUIET IMPORTED_TARGET libpulse-simple)

# MJPEG decode (libjpeg-turbo recommended; provided as `JPEG`).
find_package(JPEG QUIET)
if (NOT JPEG_FOUND)
    message(FATAL_ERROR
            "JPEG (libjpeg) development files not found. MJPEG decode requires libjpeg-turbo (recommended) or libjpeg. "
            "On Ubuntu 22.04: sudo apt install libjpeg-turbo8 libjpeg-turbo8-dev")
endif ()

# PNG decode (libpng) for virtual background replace images.
find_package(PNG QUIET)
if (NOT PNG_FOUND)
    message(FATAL_ERROR
            "PNG (libpng) development files not found. Virtual background replace images require PNG support. "
            "On Ubuntu 22.04: sudo apt install libpng-dev")
endif ()

# dlib (optional) for face landmark detection used by Open Video Eye Contact.
set(_studiocast_have_dlib FALSE)
if (STUDIOCAST_ENABLE_DLIB)
    # Prefer CMake config packages (common on modern distros), but allow module lookup.
    find_package(dlib CONFIG QUIET)
    if (dlib_FOUND)
        set(_studiocast_have_dlib TRUE)
    else()
        find_package(dlib QUIET)
        if (dlib_FOUND)
            set(_studiocast_have_dlib TRUE)
        endif()
    endif()

    if (_studiocast_have_dlib)
        message(STATUS "dlib found; Open Video Eye Contact (dlib landmarks) enabled.")
    else()
        message(STATUS "dlib not found; Open Video Eye Contact will be unavailable. Install dlib (e.g., libdlib-dev) or set dlib_DIR.")
    endif()
endif()

set(_studiocast_onnxruntime_found FALSE)
set(_studiocast_onnxruntime_target "")
studiocast_configure_onnxruntime(_studiocast_onnxruntime_found _studiocast_onnxruntime_target)
if (STUDIOCAST_ENABLE_OPEN_CUDA)
    if (_studiocast_onnxruntime_found)
        message(STATUS "ONNX Runtime found (${_studiocast_onnxruntime_target}); Open CUDA backend enabled.")
    else()
        message(STATUS "ONNX Runtime not found; disabling Open CUDA backend. Install onnxruntime or set ONNXRUNTIME_ROOT.")
        set(STUDIOCAST_ENABLE_OPEN_CUDA OFF CACHE BOOL "Enable Open CUDA backend (requires ONNX Runtime + CUDA EP)" FORCE)
    endif()
endif()

if (STUDIOCAST_ENABLE_OPEN_AUDIO)
    if (_studiocast_onnxruntime_found)
        message(STATUS "ONNX Runtime found (${_studiocast_onnxruntime_target}); Open Audio backend enabled.")
    else()
        message(STATUS "ONNX Runtime not found; Open Audio backend will be unavailable. Install onnxruntime or set ONNXRUNTIME_ROOT.")
    endif()
endif()

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
        src/core/onnx/ort_session.h
        src/core/onnx/ort_session.cpp
        src/core/open_video/model_pack_registry.h
        src/core/open_video/model_pack_registry.cpp
        src/core/open_video/frame_analysis_cache.h
        src/core/open_video/yunet_face_detector.h
        src/core/open_video/yunet_face_detector.cpp
        src/core/open_video/fastdvdnet_denoiser.h
        src/core/open_video/fastdvdnet_denoiser.cpp
        src/core/open_video/dlib_face_landmarks.h
        src/core/open_video/dlib_face_landmarks.cpp
        src/core/open_video/gaze_correction_eye_contact.h
        src/core/open_video/gaze_correction_eye_contact.cpp
        src/core/open_video/diagnostics.h
        src/core/open_video/diagnostics.cpp
        src/core/open_video/diagnose.h
        src/core/open_video/diagnose.cpp
        src/core/open_video/matting_session.h
        src/core/open_video/matting_session.cpp
        src/core/open_audio/open_audio_diagnostics.h
        src/core/open_audio/open_audio_diagnostics.cpp
        src/core/open_audio/open_audio_diagnose.h
        src/core/open_audio/open_audio_diagnose.cpp
        src/core/open_audio/model_pack_registry.h
        src/core/open_audio/model_pack_registry.cpp
        src/core/open_audio/open_audio_audio_processor.h
        src/core/open_audio/open_audio_audio_processor.cpp
        src/core/open_audio/open_audio_onnx_session.h
        src/core/open_audio/open_audio_onnx_session.cpp
        src/core/audio/effects/broadcast_audio_effect_contract.h
        src/core/util/dynlib.h
        src/core/util/dynlib.cpp
        src/core/util/json.h
        src/core/util/json.cpp
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
        src/core/config/daemon_config.h
        src/core/config/daemon_config.cpp
        src/core/config/settings.h
        src/core/config/settings.cpp
        src/core/maxine/gpu.h
        src/core/maxine/gpu.cpp
        src/core/maxine/gpu_selection.h
        src/core/maxine/gpu_selection.cpp
        src/core/maxine/availability.h
        src/core/maxine/availability.cpp
        src/core/maxine/reason_codes.h
        src/core/maxine/reason_codes.cpp
        src/core/maxine/paths.h
        src/core/maxine/paths.cpp
        src/core/maxine/maxine_manager.h
        src/core/maxine/maxine_manager.cpp
        src/core/maxine/nvcv_types.h
        src/core/maxine/nvcv_api.h
        src/core/maxine/nvcv_api.cpp
        src/core/maxine/vfx_api.h
        src/core/maxine/vfx_api.cpp
        src/core/maxine/afx_api.h
        src/core/maxine/afx_api.cpp
        src/core/maxine/afx/afx_effect.h
        src/core/maxine/afx/afx_effect.cpp
        src/core/maxine/afx/afx_audio_processor.h
        src/core/maxine/ar_api.h
        src/core/maxine/ar_api.cpp
        src/core/maxine/cuda_driver_api.h
        src/core/maxine/cuda_driver_api.cpp
        src/core/cuda/cuda_image.h
        src/core/cuda/cuda_image.cpp
        src/core/cuda/cuda_tensor.h
        src/core/cuda/cuda_tensor.cpp
        src/core/cuda/kernels/resize_bilinear.h
        src/core/cuda/kernels/resize_bilinear_ptx.cpp
        src/core/cuda/kernels/preprocess_to_nchw.h
        src/core/cuda/kernels/preprocess_to_nchw_ptx.cpp
        src/core/cuda/kernels/open_cuda_vb_kernels.h
        src/core/cuda/kernels/open_cuda_vb_kernels_ptx.cpp
        src/core/maxine/cuda_crop_scale.h
        src/core/maxine/cuda_crop_scale.cpp
        src/core/maxine/cuda_vignette.h
        src/core/maxine/cuda_vignette.cpp
        src/core/maxine/effects/maxine_effect.h
        src/core/maxine/effects/maxine_effect.cpp
        src/core/maxine/effects/vfx_green_screen_effect.h
        src/core/maxine/effects/vfx_green_screen_effect.cpp
        src/core/maxine/effects/vfx_background_blur_effect.h
        src/core/maxine/effects/vfx_background_blur_effect.cpp
        src/core/maxine/effects/vfx_transfer_effect.h
        src/core/maxine/effects/vfx_transfer_effect.cpp
        src/core/maxine/effects/vfx_denoise_effect.h
        src/core/maxine/effects/vfx_denoise_effect.cpp
        src/core/maxine/effects/vfx_relighting_effect.h
        src/core/maxine/effects/vfx_relighting_effect.cpp
        src/core/maxine/effects/ar_eye_contact_effect.h
        src/core/maxine/effects/ar_eye_contact_effect.cpp
        src/core/maxine/effects/ar_auto_frame_tracker.h
        src/core/maxine/effects/ar_auto_frame_tracker.cpp
        src/core/audio/pulse/pactl.h
        src/core/audio/pulse/pactl.cpp
        src/core/audio/audio_consumer_detector.h
        src/core/audio/audio_consumer_detector.cpp
        src/core/audio/audio_device_safety.h
        src/core/audio/audio_device_safety.cpp
        src/core/audio/audio_backend_resolver.h
        src/core/audio/audio_backend_resolver.cpp
        src/core/audio/audio_processor.h
        src/core/audio/audio_processor.cpp
        src/core/audio/dsp/post_dsp_chain.h
        src/core/audio/dsp/post_dsp_chain.cpp
        src/core/audio/effects/broadcast_audio_effects.h
        src/core/audio/effects/broadcast_audio_effects_plan.h
        src/core/audio/effects/broadcast_audio_effects_plan.cpp
        src/core/audio/effects/broadcast_audio_effects_json.h
        src/core/audio/effects/broadcast_audio_effects_json.cpp
        src/core/audio/virtual_mic_state.h
        src/core/audio/virtual_mic_state.cpp
        src/core/audio/virtual_mic.h
        src/core/audio/virtual_mic.cpp
        src/core/audio/virtual_speaker_state.h
        src/core/audio/virtual_speaker_state.cpp
        src/core/audio/virtual_speaker.h
        src/core/audio/virtual_speaker.cpp
        src/core/audio/virtual_audio_service.h
        src/core/audio/virtual_audio_service.cpp
        src/core/video/v4l2loopback.h
        src/core/video/v4l2loopback.cpp
        src/core/video/v4l2_writer.h
        src/core/video/v4l2_writer.cpp
        src/core/video/gpu_frame.h
        src/core/video/pattern.h
        src/core/video/pattern.cpp
        src/core/video/image_ppm.h
        src/core/video/image_ppm.cpp
        src/core/video/feed.h
        src/core/video/feed.cpp
        src/core/video/convert.h
        src/core/video/convert.cpp
        src/core/video/scaling_policy.h
        src/core/video/scaling_policy.cpp
        src/core/video/mjpeg_decode.h
        src/core/video/mjpeg_decode.cpp
        src/core/video/effects/effect_types.h
        src/core/video/effects/effect_types.cpp
        src/core/video/effects/broadcast_effect_contract.h
        src/core/video/effects/broadcast_effect_maxine_gate.h
        src/core/video/effects/broadcast_effect_rules.h
        src/core/video/effects/broadcast_effect_rules.cpp
        src/core/video/effects/broadcast_effects.h
        src/core/video/effects/broadcast_effects.cpp
        src/core/video/effects/broadcast_effects_json.h
        src/core/video/effects/broadcast_effects_json.cpp
        src/core/video/effects/effect.h
        src/core/video/effects/effect_chain.h
        src/core/video/effects/effect_chain.cpp
        src/core/video/effects/effect_descriptors.h
        src/core/video/effects/effect_descriptors.cpp
        src/core/video/effects/mirror_effect.h
        src/core/video/effects/mirror_effect.cpp
        src/core/video/effects/box_blur.h
        src/core/video/effects/box_blur.cpp
        src/core/video/effects/background_blur_cpu.h
        src/core/video/effects/background_blur_cpu.cpp
        src/core/video/effects/background_remove_cpu.h
        src/core/video/effects/background_remove_cpu.cpp
        src/core/video/v4l2_capture.h
        src/core/video/v4l2_capture.cpp
        src/core/video/camera_pipeline.h
        src/core/video/camera_pipeline.cpp
        src/core/video/camera_effects_json.h
        src/core/video/camera_effects_json.cpp
        src/core/video/broadcast_camera_effects_json.h
        src/core/video/broadcast_camera_effects_json.cpp
        src/core/video/virtual_camera_service.h
        src/core/video/virtual_camera_service.cpp

        src/core/ipc/daemon_socket.h
        src/core/ipc/daemon_socket.cpp
        src/core/ipc/daemon_client.h
        src/core/ipc/daemon_client.cpp
        src/core/ipc/daemon_server.h
        src/core/ipc/daemon_server.cpp

)

set_target_properties(studiocast_core PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
)

# Video color conversion is extremely hot (runs every frame). In Debug builds the lack of
# optimization can make the virtual camera feel laggy. Force optimization for this TU.
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        set_source_files_properties(src/core/video/convert.cpp PROPERTIES COMPILE_OPTIONS "-O3")
endif()

target_include_directories(studiocast_core PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${STUDIOCAST_GENERATED_DIR}"
)

target_link_libraries(studiocast_core PUBLIC ${CMAKE_DL_LIBS})

if (TARGET JPEG::JPEG)
    target_link_libraries(studiocast_core PUBLIC JPEG::JPEG)
else()
    target_include_directories(studiocast_core PUBLIC ${JPEG_INCLUDE_DIRS})
    target_link_libraries(studiocast_core PUBLIC ${JPEG_LIBRARIES})
endif()

if (TARGET PNG::PNG)
    target_link_libraries(studiocast_core PUBLIC PNG::PNG)
else()
    target_include_directories(studiocast_core PUBLIC ${PNG_INCLUDE_DIRS})
    target_link_libraries(studiocast_core PUBLIC ${PNG_LIBRARIES})
endif()

if (PULSE_SIMPLE_FOUND)
    target_sources(studiocast_core PRIVATE
            src/core/audio/audio_pipeline.h
            src/core/audio/audio_pipeline.cpp
    )
    target_link_libraries(studiocast_core PUBLIC PkgConfig::PULSE_SIMPLE)
    target_compile_definitions(studiocast_core PUBLIC STUDIOCAST_HAVE_PULSE_SIMPLE=1)
else()
    target_compile_definitions(studiocast_core PUBLIC STUDIOCAST_HAVE_PULSE_SIMPLE=0)
    message(STATUS "libpulse-simple not found via pkg-config; audio pipeline is disabled. Install libpulse-dev to enable.")
endif()

target_compile_definitions(studiocast_core PUBLIC
    STUDIOCAST_ENABLE_OPEN_CUDA=$<BOOL:${STUDIOCAST_ENABLE_OPEN_CUDA}>
    STUDIOCAST_ENABLE_OPEN_AUDIO=$<BOOL:${STUDIOCAST_ENABLE_OPEN_AUDIO}>
    STUDIOCAST_HAVE_DLIB=$<BOOL:${_studiocast_have_dlib}>
)

if (_studiocast_have_dlib)
    if (TARGET dlib::dlib)
        target_link_libraries(studiocast_core PUBLIC dlib::dlib)
    elseif (TARGET dlib)
        target_link_libraries(studiocast_core PUBLIC dlib)
    else()
        message(WARNING "dlib found but no CMake target was provided; linking may fail.")
    endif()
endif()

if ((_studiocast_onnxruntime_found) AND (STUDIOCAST_ENABLE_OPEN_CUDA OR STUDIOCAST_ENABLE_OPEN_AUDIO))
    target_link_libraries(studiocast_core PUBLIC ${_studiocast_onnxruntime_target})
    target_compile_definitions(studiocast_core PUBLIC STUDIOCAST_HAVE_ONNXRUNTIME=1)
else()
    target_compile_definitions(studiocast_core PUBLIC STUDIOCAST_HAVE_ONNXRUNTIME=0)
endif()

studiocast_set_project_warnings(studiocast_core)
studiocast_enable_sanitizers(studiocast_core)

# -----------------------------
# GUI executable
# -----------------------------
add_executable(studiocast
        src/main.cpp
        src/gui/theme.h
        src/gui/theme.cpp
        src/gui/resources/gui_resources.qrc
        src/gui/main_window.h
        src/gui/main_window.cpp
        src/gui/status/daemon_status_snapshot.h
        src/gui/status/daemon_status_snapshot.cpp
        src/gui/status/status_poller.h
        src/gui/status/status_poller.cpp
        src/gui/pages/home_page.h
        src/gui/pages/home_page.cpp
        src/gui/pages/audio_page.h
        src/gui/pages/audio_page.cpp
        src/gui/pages/video_page.h
        src/gui/pages/video_page.cpp
        src/core/probe/probe.h
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
# Open CUDA helper tool
# -----------------------------
add_executable(studiocast-open
        src/tools/studiocast_open_main.cpp
)

set_target_properties(studiocast-open PROPERTIES AUTOMOC OFF AUTOUIC OFF AUTORCC OFF)

target_link_libraries(studiocast-open PRIVATE studiocast_core)

target_include_directories(studiocast-open PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${STUDIOCAST_GENERATED_DIR}"
)

studiocast_set_project_warnings(studiocast-open)
studiocast_enable_sanitizers(studiocast-open)

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
# Daemon control CLI (studiocastctl)
# -----------------------------
add_executable(studiocastctl
        src/tools/studiocastctl_main.cpp
)

target_link_libraries(studiocastctl PRIVATE studiocast_core)

target_include_directories(studiocastctl PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${STUDIOCAST_GENERATED_DIR}"
)

studiocast_set_project_warnings(studiocastctl)
studiocast_enable_sanitizers(studiocastctl)

set_target_properties(studiocastctl PROPERTIES
        AUTOMOC OFF
        AUTOUIC OFF
        AUTORCC OFF
        OUTPUT_NAME "studiocastctl"
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
install(TARGETS studiocast studiocast-probe studiocast-maxine studiocast-open studiocast-audio studiocast-video studiocastctl
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

install(TARGETS studiocastd
        RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

if (BUILD_TESTING AND PULSE_SIMPLE_FOUND)
    add_executable(studiocast-audio-tests
            tests/audio_service_tests.cpp
    )

    target_link_libraries(studiocast-audio-tests PRIVATE studiocast_core)

    target_include_directories(studiocast-audio-tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
            "${STUDIOCAST_GENERATED_DIR}"
    )

    studiocast_set_project_warnings(studiocast-audio-tests)
    studiocast_enable_sanitizers(studiocast-audio-tests)

    set_target_properties(studiocast-audio-tests PROPERTIES
            AUTOMOC OFF
            AUTOUIC OFF
            AUTORCC OFF
    )

    add_test(NAME studiocast-audio-tests COMMAND studiocast-audio-tests)
elseif (BUILD_TESTING)
    message(STATUS "Skipping studiocast-audio-tests because libpulse-simple was not found.")
endif()

if (BUILD_TESTING)
    add_executable(studiocast-maxine-paths-tests
            tests/maxine_paths_tests.cpp
    )

    target_link_libraries(studiocast-maxine-paths-tests PRIVATE studiocast_core)

    target_include_directories(studiocast-maxine-paths-tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
            "${STUDIOCAST_GENERATED_DIR}"
    )

    studiocast_set_project_warnings(studiocast-maxine-paths-tests)
    studiocast_enable_sanitizers(studiocast-maxine-paths-tests)

    set_target_properties(studiocast-maxine-paths-tests PROPERTIES
            AUTOMOC OFF
            AUTOUIC OFF
            AUTORCC OFF
    )

    add_test(NAME studiocast-maxine-paths-tests COMMAND studiocast-maxine-paths-tests)

    add_executable(studiocast-ipc-tests
            tests/ipc_client_tests.cpp
    )

    target_link_libraries(studiocast-ipc-tests PRIVATE studiocast_core)

    target_include_directories(studiocast-ipc-tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
            "${STUDIOCAST_GENERATED_DIR}"
    )

    studiocast_set_project_warnings(studiocast-ipc-tests)
    studiocast_enable_sanitizers(studiocast-ipc-tests)

    set_target_properties(studiocast-ipc-tests PROPERTIES
            AUTOMOC OFF
            AUTOUIC OFF
            AUTORCC OFF
    )

    add_test(NAME studiocast-ipc-tests COMMAND studiocast-ipc-tests)

    add_executable(studiocast-gui-status-tests
            tests/gui_status_snapshot_tests.cpp
            src/gui/status/daemon_status_snapshot.h
            src/gui/status/daemon_status_snapshot.cpp
    )

    target_link_libraries(studiocast-gui-status-tests PRIVATE
            Qt${QT_VERSION_MAJOR}::Widgets
    )

    target_include_directories(studiocast-gui-status-tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
            "${STUDIOCAST_GENERATED_DIR}"
    )

    studiocast_set_project_warnings(studiocast-gui-status-tests)
    studiocast_enable_sanitizers(studiocast-gui-status-tests)

    set_target_properties(studiocast-gui-status-tests PROPERTIES
            AUTOMOC OFF
            AUTOUIC OFF
            AUTORCC OFF
    )

    add_test(NAME studiocast-gui-status-tests COMMAND studiocast-gui-status-tests)

    add_executable(studiocast-video-tests
            tests/video_service_tests.cpp
    )

    target_link_libraries(studiocast-video-tests PRIVATE studiocast_core)

    target_include_directories(studiocast-video-tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src"
            "${STUDIOCAST_GENERATED_DIR}"
    )

    studiocast_set_project_warnings(studiocast-video-tests)
    studiocast_enable_sanitizers(studiocast-video-tests)

    set_target_properties(studiocast-video-tests PROPERTIES
            AUTOMOC OFF
            AUTOUIC OFF
            AUTORCC OFF
    )

    add_test(NAME studiocast-video-tests COMMAND studiocast-video-tests)
endif()
