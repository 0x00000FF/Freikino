# Central compile/link flags. Every target that builds Freikino code must link
# against `freikino::compile_options` so that security flags are applied
# uniformly and cannot be forgotten on a per-target basis.

add_library(freikino_compile_options INTERFACE)
add_library(freikino::compile_options ALIAS freikino_compile_options)

target_compile_definitions(freikino_compile_options INTERFACE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    STRICT
    _WIN32_WINNT=0x0A00
    WINVER=0x0A00
)

if(MSVC)
    target_compile_options(freikino_compile_options INTERFACE
        /W4
        /WX
        /permissive-
        /utf-8
        /Zc:__cplusplus
        /Zc:preprocessor
        /Zc:inline
        /Zc:throwingNew
        /Zc:referenceBinding
        /Zc:rvalueCast
        /Zc:strictStrings
        /Zc:ternary
        /EHsc
        /GS
        /Gy
        /Gw
        /guard:cf
        /sdl
        /volatile:iso
        /we4640   # construction of local static is not thread-safe -> error
    )

    target_compile_options(freikino_compile_options INTERFACE
        $<$<CONFIG:Debug>:/Od /RTC1 /Zi>
        $<$<CONFIG:Release>:/O2 /Oi /GL /Zi>
        $<$<CONFIG:RelWithDebInfo>:/O2 /Oi /Zi>
        $<$<CONFIG:MinSizeRel>:/O1 /Oi /GL>
    )

    target_link_options(freikino_compile_options INTERFACE
        /DYNAMICBASE
        /HIGHENTROPYVA
        /NXCOMPAT
        /GUARD:CF
        /CETCOMPAT
        /DEBUG
        /LARGEADDRESSAWARE
        $<$<CONFIG:Release>:/LTCG /OPT:REF /OPT:ICF>
        $<$<CONFIG:MinSizeRel>:/LTCG /OPT:REF /OPT:ICF>
    )
endif()

# MSVC runtime: use the dynamic CRT so exception unwinding across DLL boundaries
# (FFmpeg, libass from vcpkg) is well-defined. vcpkg static triplets still use
# /MT by default; we override for correctness with mixed runtimes.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL"
    CACHE STRING "MSVC runtime" FORCE)
