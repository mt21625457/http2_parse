cmake_minimum_required(VERSION 3.14) # GoogleTest needs at least 3.13, 3.14 is a good baseline
project(Http2LibCpp LANGUAGES CXX)

SET(CMAKE_C_COMPILER ${CC})
SET(CMAKE_CXX_COMPILER ${CXX})

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  

set(TRAGE_BUILD http2_parse)

# Option to build tests, ON by default
option(HTTP2LIB_BUILD_TESTS "Build the http2lib tests" ON)


if(CMAKE_SYSTEM_NAME MATCHES "Darwin")

    # 设置LLVM C++包含路径
    set(LLVM_INCLUDE_DIR "${LLVM_PATH}/include")
    set(CMAKE_THREAD_LIBS_INIT "-lpthread")
    set(CMAKE_HAVE_THREADS_LIBRARY 1)


    execute_process(COMMAND
        "xcrun" --show-sdk-path
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
        OUTPUT_VARIABLE MACOS_SDK_PATH
        ERROR_QUIET OUTPUT_STRIP_TRAILING_WHITESPACE)

    # 显式设置OSX系统特定参数，避免使用"macosx"作为sysroot
    # 明确设置SDK路径，避免使用macosx字符串
    set(LLVM_CXX_INCLUDE_DIR "${LLVM_INCLUDE_DIR}/c++/v1")
    set(CMAKE_OSX_SYSROOT "${MACOS_SDK_PATH}")
    set(CMAKE_OSX_DEPLOYMENT_TARGET "11.5")
 
    message(STATUS "Setting macOS SDK path: ${MACOS_SDK_PATH}")

    SET(CMAKE_C_FLAGS ${CMAKE_C_FLAGS} " -O3 -g -ffunction-sections -fdata-sections -isysroot ${MACOS_SDK_PATH}")
    SET(CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS} " -O3 -g -std=c++23 -nostdinc++ -stdlib=libc++ -isysroot ${MACOS_SDK_PATH}  -D_HAS_STD_BYTE=0 -I${LLVM_CXX_INCLUDE_DIR}")
    SET(CMAKE_EXE_LINKER_FLAGS ${CMAKE_EXE_LINKER_FLAGS} " -nostdinc++ -stdlib=libc++ -Wl,-dead_strip -ffunction-sections -Wl,-rpath,'@loader_path/../Frameworks' -framework Foundation -framework DiskArbitration -framework SystemConfiguration -framework CoreServices -framework IOKit")
    SET(CMAKE_SHARED_LINKER_FLAGS ${CMAKE_SHARED_LINKER_FLAGS} " -nostdinc++ -stdlib=libc++ -Wl,-dead_strip -ffunction-sections")
    
elseif(CMAKE_SYSTEM_NAME MATCHES "Linux")

    SET(CMAKE_C_FLAGS "-O3 -g -ffunction-sections -fdata-sections")
    SET(CMAKE_CXX_FLAGS "-O3 -g -std=c++23 -stdlib=libc++ -ffunction-sections -fdata-sections -fno-strict-aliasing -fvisibility=hidden -fno-omit-frame-pointer -Wno-unused-command-line-argument -Wnan-infinity-disabled")
    # SET(CMAKE_CXX_FLAGS "-O3 -g -stdlib=libc++ -fno-strict-aliasing -fno-omit-frame-pointer") 
    execute_process (COMMAND ${CMAKE_CXX_COMPILER} --print-libgcc-file-name --rtlib=compiler-rt OUTPUT_VARIABLE BUILTINS_LIBRARY OUTPUT_STRIP_TRAILING_WHITESPACE)
    add_link_options("-fuse-ld=lld")
    SET(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--exclude-libs,ALL -Wl,-rpath=../lib:./lib -Wl,--gc-sections -ffunction-sections -fdata-sections")
    SET(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--exclude-libs,ALL -Wl,-rpath=$ORIGIN/../lib:./lib -Wl,--gc-sections -ffunction-sections -fdata-sections")

endif()

set(CMAKE_PREFIX_PATH ${LIBBASE_PATH}/lib/cmake)


find_package(GTest CONFIG REQUIRED)


file(GLOB_RECURSE HTTP2LIB_SOURCES 
    "src/*.cpp"
    "src/*.h"
    "src/*.hpp"
)
# --- Library ---
# Add the cpp_lib as a library
# We'll create a CMakeLists.txt in cpp_lib for its sources
add_library(http2_parse ${HTTP2LIB_SOURCES})


# --- Test Sources ---
file(GLOB_RECURSE TEST_SOURCES 
    "tests/*.cpp"
    "tests/*.h"
    "tests/*.hpp"
)

# --- Test Executable ---
add_executable(http2_parse_test ${TEST_SOURCES})

# --- Test Dependencies ---
target_link_libraries(http2_parse_test 
    PUBLIC
    http2_parse
    GTest::gtest
    GTest::gtest_main
    http2_parse
)

# --- Test Include Directories ---
target_include_directories(http2_parse_test 
    PRIVATE 
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/tests
)



# --- Enable Testing ---
enable_testing()
add_test(NAME http2_parse_tests COMMAND http2_parse_test)


