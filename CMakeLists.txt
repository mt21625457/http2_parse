cmake_minimum_required(VERSION 3.14) # GoogleTest needs at least 3.13, 3.14 is a good baseline
project(Http2LibCpp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Option to build tests, ON by default
option(HTTP2LIB_BUILD_TESTS "Build the http2lib tests" ON)

# --- GoogleTest ---
# Add GoogleTest using FetchContent
include(FetchContent)
FetchContent_Declare(
  googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip # Example: Use a specific release
  # Or use GitRepository and GIT_TAG for more flexibility if git is available
  # GIT_REPOSITORY https://github.com/google/googletest.git
  # GIT_TAG release-1.14.0 # Or main, or a specific commit hash
)
# For GTest versions < 1.10, you might need to set these variables before FetchContent_MakeAvailable
# set(gtest_force_shared_crt ON CACHE BOOL "" FORCE) # If needed on Windows
FetchContent_MakeAvailable(googletest)


# --- Library ---
# Add the cpp_lib as a library
# We'll create a CMakeLists.txt in cpp_lib for its sources
add_subdirectory(cpp_lib)


# --- Tests ---
if(HTTP2LIB_BUILD_TESTS)
  enable_testing() # Enables CTest
  add_subdirectory(tests)
  # Example of adding a test executable and linking GoogleTest
  # This will be detailed in tests/CMakeLists.txt
  # add_executable(http2_tests ...)
  # target_link_libraries(http2_tests PRIVATE GTest::gtest GTest::gtest_main http2lib_cpp)
endif()

# --- Installation (Optional Example) ---
# install(TARGETS http2lib_cpp DESTINATION lib)
# install(DIRECTORY cpp_lib/ DESTINATION include/cpp_lib FILES_MATCHING PATTERN "*.h")
