# Subtitle rendering: libass for layout/glyph, in-house parsers for
# the non-ASS formats (SRT, SAMI) that libass doesn't load directly.
#
# Lives in its own module so the render + app layers can take a hard
# dependency on libass without dragging FFmpeg in alongside.

# libass comes in via vcpkg. Use find_library on "ass" rather than
# pkg-config so we don't depend on the vcpkg-generated .pc files being
# on the search path.
find_library(LIBASS_LIB ass REQUIRED)
find_path(LIBASS_INCLUDE_DIR ass/ass.h REQUIRED)

add_library(freikino_subtitle STATIC
    src/subtitle_source.cpp
    src/subtitle_renderer.cpp
)
add_library(freikino::subtitle ALIAS freikino_subtitle)

target_include_directories(freikino_subtitle
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        ${LIBASS_INCLUDE_DIR}
)

target_link_libraries(freikino_subtitle
    PUBLIC
        freikino::common
        freikino::compile_options
    PRIVATE
        ${LIBASS_LIB}
)
