# Aura Voice Agent — 端侧语音助手（Raspberry Pi / Windows 开发机）
# 构建方式：
#   Windows (MinGW):  cmake -B build -G "MinGW Makefiles"
#   RPi (aarch64):    cmake -B build -G "Unix Makefiles" -DCMAKE_TOOLCHAIN_FILE=toolchains/aarch64-linux-gnu.toolchain.cmake

cmake_minimum_required(VERSION 3.16)
project(Aura VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# 测试注册需在 add_subdirectory 之前启用（audio_engine 自带 unitest）
enable_testing()

# 对外公共头文件目录（include/aura/...）
add_library(aura_interfaces INTERFACE)
target_include_directories(aura_interfaces INTERFACE ${PROJECT_SOURCE_DIR}/include)
# 全链路统一音频参数：16 kHz / 16 bit / mono
target_compile_definitions(aura_interfaces INTERFACE
    AURA_SAMPLE_RATE=16000
    AURA_CHANNELS=1
    AURA_BITS_PER_SAMPLE=16)

# 第三方库（全部 submodule，见 docs/audio_engine_integration.md）
set(THIRD_PARTY_ROOT ${PROJECT_SOURCE_DIR}/third_party)
set(CMAKE_PREFIX_PATH "${THIRD_PARTY_ROOT}/abseil-cpp/install" ${CMAKE_PREFIX_PATH})
add_subdirectory(${THIRD_PARTY_ROOT}/trickroom/src/audio_engine)

# 核心模块
add_subdirectory(src/utils)
add_subdirectory(src/core)
add_subdirectory(src/platform)
add_subdirectory(src/audio)

# 测试与示例
add_subdirectory(tests)
add_subdirectory(examples)
