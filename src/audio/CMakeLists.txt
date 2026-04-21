add_library(freikino_audio STATIC
    src/wasapi_renderer.cpp
)
add_library(freikino::audio ALIAS freikino_audio)

target_include_directories(freikino_audio
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(freikino_audio
    PUBLIC
        freikino::common
        freikino::render
        freikino::compile_options
    PRIVATE
        ole32
        avrt
        mmdevapi
)
