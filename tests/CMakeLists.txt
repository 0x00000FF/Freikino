find_package(GTest CONFIG REQUIRED)
include(GoogleTest)

add_executable(freikino_tests
    test_placeholder.cpp
)

target_link_libraries(freikino_tests
    PRIVATE
        GTest::gtest
        GTest::gtest_main
        freikino::compile_options
)

# Register each TEST() with CTest so `ctest` reports per-case pass/fail
# rather than a single opaque exe result.
gtest_discover_tests(freikino_tests
    DISCOVERY_TIMEOUT 30
)
