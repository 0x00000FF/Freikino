# The media module is the only part of Freikino that pulls in third-party
# code (FFmpeg, libass). Every other module stays dependency-free. That lets
# anyone who just wants to hack on the shell build the skeleton quickly;
# enabling this module commits to the vcpkg setup.

find_package(FFMPEG REQUIRED)

if(NOT FFMPEG_INCLUDE_DIRS OR NOT FFMPEG_LIBRARIES)
    message(FATAL_ERROR
        "FFmpeg was located but FFMPEG_INCLUDE_DIRS / FFMPEG_LIBRARIES are "
        "empty. Ensure vcpkg's toolchain is active (VCPKG_ROOT set, "
        "CMAKE_TOOLCHAIN_FILE pointed at vcpkg.cmake) before configuring.")
endif()

add_library(freikino_media STATIC
    src/ffmpeg_source.cpp
    src/matroska_subs.cpp
    src/probe.cpp
    src/thumbnail_source.cpp
)
add_library(freikino::media ALIAS freikino_media)

target_include_directories(freikino_media
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${FFMPEG_INCLUDE_DIRS}
)

target_link_libraries(freikino_media
    PUBLIC
        freikino::common
        freikino::render
        freikino::audio
        freikino::compile_options
    PRIVATE
        ${FFMPEG_LIBRARIES}
        d3d11
        dxgi
)

# FFmpeg's public headers trigger specific /W4 warnings that we cannot fix
# from our side. Scope the suppression narrowly to this TU only; the rest of
# the codebase stays under /W4 /WX unchanged.
if(MSVC)
    target_compile_options(freikino_media PRIVATE
        /wd4244   # conversion, possible loss of data (FFmpeg macros)
        /wd4267   # conversion from size_t, possible loss of data
    )
endif()
