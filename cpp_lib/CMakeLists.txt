cmake_minimum_required(VERSION 3.14)
project(Http2LibCpp_Core) # Sub-project for the library itself

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Gather all source files for the library
# This assumes .h files are for interface and .cpp for implementation
# For a more robust setup, explicitly list sources or use target_sources with PRIVATE/PUBLIC/INTERFACE specifiers.
file(GLOB LIB_SOURCES "*.cpp")
file(GLOB LIB_HEADERS "*.h")


add_library(http2lib_cpp ${LIB_SOURCES}) # Could be STATIC or SHARED

# Public include directories for consumers of this library
target_include_directories(http2lib_cpp
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR} # To find headers like "http2_parser.h"
    # Add other necessary include paths if any, e.g. if using external deps for the lib itself
)

# Link dependencies if any (e.g., if using a specific Huffman library not part of std)
# target_link_libraries(http2lib_cpp PRIVATE some_dependency)

# Optional: Set properties like CXX_VISIBILITY_PRESET for shared libraries
# if(BUILD_SHARED_LIBS)
#   set_target_properties(http2lib_cpp PROPERTIES CXX_VISIBILITY_PRESET hidden)
#   target_compile_definitions(http2lib_cpp PRIVATE HTTP2LIB_CPP_DLL_EXPORT) # For exporting symbols
# endif()

# Installation rules for the library and its headers (matches root CMakeLists.txt example)
install(TARGETS http2lib_cpp DESTINATION lib)
install(FILES ${LIB_HEADERS} DESTINATION include/cpp_lib)
