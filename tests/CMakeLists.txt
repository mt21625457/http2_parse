cmake_minimum_required(VERSION 3.14)
project(Http2LibCpp_Tests)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Find the library we are testing (http2lib_cpp)
# This should be available as it's added before this subdirectory in the root CMakeLists.txt
# No need to find_package if it's part of the same CMake build tree.

# Gather test source files
# For now, let's assume a main test file, e.g., test_main.cpp
# And specific test files like test_hpack_decoder.cpp, test_http2_parser.cpp etc.
file(GLOB TEST_SOURCES "*.cpp")

# Add the test executable
add_executable(http2_tests ${TEST_SOURCES})

# Link the test executable against GoogleTest and our library
# GTest::gtest for the library, GTest::gtest_main for main() function definition
target_link_libraries(http2_tests
    PRIVATE
        GTest::gtest
        GTest::gtest_main
        http2lib_cpp # Link against our library
)

# Include CTest and add the test executable to it
include(GoogleTest)
gtest_discover_tests(http2_tests) # Discovers tests to be run by CTest

# Optional: If tests need access to header files from the library in a specific way
# target_include_directories(http2_tests PRIVATE ../cpp_lib)
# This is usually handled by http2lib_cpp exporting its include directories correctly.

# If your tests require specific data files (e.g., captured HTTP/2 streams for parser tests),
# you might need to copy them to the build directory or reference them from source.
# Example:
# file(COPY test_data/ DESTINATION ${CMAKE_CURRENT_BINARY_DIR}/test_data)
