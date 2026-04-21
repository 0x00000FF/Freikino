add_library(freikino_render STATIC
    src/device.cpp
    src/swap_chain.cpp
    src/presenter.cpp
    src/video_pipeline.cpp
    src/wall_clock.cpp
    src/overlay_renderer.cpp
)
add_library(freikino::render ALIAS freikino_render)

target_include_directories(freikino_render
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(freikino_render
    PUBLIC
        freikino::common
        freikino::compile_options
    PRIVATE
        d3d11
        dxgi
        dxguid
        d3dcompiler
        d2d1
        dwrite
)
