# 设置最低版本要求
cmake_minimum_required(VERSION 3.10.0)

# 设置项目名称
project(AuarProject)

if(CMAKE_TOOLCHAIN_FILE)
    get_filename_component(CMAKE_TOOLCHAIN_FILE_NAME ${CMAKE_TOOLCHAIN_FILE} NAME)
    find_file(CMAKE_TOOLCHAIN_FILE ${CMAKE_TOOLCHAIN_FILE_NAME} PATHS ${CMAKE_SOURCE_DIR} NO_DEFAULT_PATH)
    message(STATUS "CMAKE_TOOLCHAIN_FILE = ${CMAKE_TOOLCHAIN_FILE}")
endif()

# 设置 C++ 标准
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED True)

set(LIBRARY_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/lib)
set(EXECUTABLE_OUTPUT_PATH ${PROJECT_SOURCE_DIR}/bin)

set(CMAKE_VERBOSE_MAKEFILE ON)
set(CMAKE_PREFIX_PATH 
    "${PROJECT_SOURCE_DIR}/third_libs/abseil-cpp/install" 
    ${CMAKE_PREFIX_PATH}
)

find_package(absl REQUIRED)

option(BUILD_TEST_WAVE_IO "Whether to build wav io unitest" OFF)
option(BUILD_TEST_ANR "Whether to build anr unitest" ON)


if(BUILD_TEST_WAVE_IO)
    include(cmake/test_wav_io.cmake)
    add_wav_io_test_module()
endif()

if(BUILD_TEST_ANR)
    include(cmake/test_anr.cmake)
    add_anr_test_module()
endif()


