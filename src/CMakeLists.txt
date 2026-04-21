option(FREIKINO_WITH_MEDIA
    "Build the media module (requires FFmpeg + libass via vcpkg)" ON)

add_subdirectory(common)
add_subdirectory(platform)
add_subdirectory(render)
add_subdirectory(audio)
add_subdirectory(ui)
if(FREIKINO_WITH_MEDIA)
    add_subdirectory(media)
    add_subdirectory(subtitle)
endif()
add_subdirectory(app)
