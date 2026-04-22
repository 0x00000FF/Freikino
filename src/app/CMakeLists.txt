add_executable(freikino WIN32
    main.cpp
    audio_info_overlay.cpp
    audio_tracks_overlay.cpp
    main_window.cpp
    media_session.cpp
    opening_overlay.cpp
    playback.cpp
    playlist.cpp
    playlist_overlay.cpp
    spectrum_visualizer.cpp
    subtitle_overlay.cpp
    subtitle_setup_overlay.cpp
    title_toast.cpp
    transport_overlay.cpp
    volume_osd.cpp
    debug_overlay.cpp
    ${CMAKE_SOURCE_DIR}/resources/freikino.rc
)
set_target_properties(freikino PROPERTIES
    OUTPUT_NAME "Freikino"
)

target_link_libraries(freikino
    PRIVATE
        freikino::ui
        freikino::render
        freikino::platform
        freikino::common
        freikino::compile_options
        comdlg32
        dwmapi
        ole32
        shell32
        windowscodecs
        shlwapi
)

target_link_libraries(freikino PRIVATE freikino::audio)

if(FREIKINO_WITH_MEDIA)
    target_link_libraries(freikino PRIVATE freikino::media freikino::subtitle)
    target_compile_definitions(freikino PRIVATE FREIKINO_WITH_MEDIA=1)
endif()

# The authoritative manifest is in resources/freikino.manifest and is embedded
# via freikino.rc. Suppress the linker's auto-generated manifest to avoid a
# duplicate RT_MANIFEST resource (CVT1100) at link time.
if(MSVC)
    target_link_options(freikino PRIVATE /MANIFEST:NO)
endif()

# ---------------------------------------------------------------------------
# Post-build verification: reject any build whose PE is missing the expected
# exploit mitigations (ASLR/DEP/CFG/CET/HIGHENTROPYVA) or which has linked
# against a forbidden UI framework (MFC/ATL/Qt/wx/GTK). This runs on every
# link of the exe, so flag regressions surface before the binary ever runs.
# ---------------------------------------------------------------------------
if(MSVC)
    get_filename_component(_msvc_bin_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(_dumpbin "${_msvc_bin_dir}/dumpbin.exe")

    if(NOT EXISTS "${_dumpbin}")
        find_program(FREIKINO_DUMPBIN dumpbin
            PATHS "${_msvc_bin_dir}"
            DOC "Path to MSVC dumpbin.exe")
        if(FREIKINO_DUMPBIN)
            set(_dumpbin "${FREIKINO_DUMPBIN}")
        endif()
    endif()

    if(EXISTS "${_dumpbin}")
        add_custom_command(TARGET freikino POST_BUILD
            COMMAND powershell.exe
                -NoProfile
                -NonInteractive
                -ExecutionPolicy Bypass
                -File "${CMAKE_SOURCE_DIR}/cmake/verify_binary.ps1"
                -ExePath "$<TARGET_FILE:freikino>"
                -DumpBin "${_dumpbin}"
            COMMENT "verify: $<TARGET_FILE_NAME:freikino>"
            VERBATIM
        )
    else()
        message(FATAL_ERROR
            "dumpbin.exe not found; post-build verification cannot run. "
            "Configure this preset from a Developer Command Prompt or set "
            "FREIKINO_DUMPBIN explicitly.")
    endif()
endif()
