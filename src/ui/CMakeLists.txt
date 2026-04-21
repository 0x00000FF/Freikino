add_library(freikino_ui STATIC
    src/window.cpp
)
add_library(freikino::ui ALIAS freikino_ui)

target_include_directories(freikino_ui
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(freikino_ui
    PUBLIC
        freikino::common
        freikino::platform
        freikino::compile_options
    PRIVATE
        user32
        gdi32
)
