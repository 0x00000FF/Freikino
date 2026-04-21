add_library(freikino_common STATIC
    src/error.cpp
    src/strings.cpp
    src/log.cpp
)
add_library(freikino::common ALIAS freikino_common)

target_include_directories(freikino_common
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(freikino_common
    PUBLIC
        freikino::compile_options
)
