add_library(freikino_platform STATIC
    src/dpi.cpp
)
add_library(freikino::platform ALIAS freikino_platform)

target_include_directories(freikino_platform
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(freikino_platform
    PUBLIC
        freikino::common
        freikino::compile_options
    PRIVATE
        user32
)
