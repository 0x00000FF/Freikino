find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

add_executable(freikino_tests
    test_placeholder.cpp
    test_strings.cpp
    test_error.cpp
    test_log.cpp
    test_dpi.cpp
    test_frame_queue.cpp
)

target_link_libraries(freikino_tests
    PRIVATE
        GTest::gtest
        GTest::gtest_main
        freikino::compile_options
        freikino::common
        freikino::platform
        # `freikino::platform` links user32 PRIVATE, so its Win32 DPI
        # symbols (GetDpiForWindow, GetProcAddress on user32.dll) need
        # user32 resolved at the test binary's final link.
        user32
)

# The SPSC queue is a header-only template inside the media module.
# Testing it doesn't require linking freikino::media (which would drag
# FFmpeg into the test binary), so we pull in just the include path.
target_include_directories(freikino_tests
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/../src/media/include
)

# Register each TEST() with CTest so `ctest` reports per-case pass/fail
# rather than a single opaque exe result.
gtest_discover_tests(freikino_tests
    DISCOVERY_TIMEOUT 30
)
