# Aura — 板端全链路语音交互框架
#
# 分层（自上而下，禁止反向依赖，见 docs/todo.md 第 3 节）：
#   apps → agent/net → kws/asr/llm/tts/voiceprint → audio/dsp/inference → core/utils → platform
#
# 本文件负责：模块裁剪选项、第三方依赖、编译期配置生成，然后分发到 src/apps/tests。
#
# 快速开始（PC 开发，见 README）：
#   cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_TOOLCHAIN_FILE=toolchains/windows-x86_64.cmake
#   cmake --build build -j
#   ./build/tests/aura_unit_tests
#   ./build/tests/host_sim/host_sim

cmake_minimum_required(VERSION 3.20)
project(Aura VERSION 0.1.0 LANGUAGES C CXX)

include(FetchContent)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------- 全局语言标准
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ------------------------------------------------------------------ 模块裁剪
# 与运行期配置的分工：这里决定"编不编进去"，configs/ 决定"怎么调"。
option(AURA_BUILD_KWS        "编译关键词唤醒模块"          ON)
option(AURA_BUILD_VOICEPRINT "编译声纹模块"                ON)
option(AURA_BUILD_ASR        "编译语音识别模块"            ON)
option(AURA_BUILD_LLM        "编译大语言模型模块"          ON)
option(AURA_BUILD_TTS        "编译语音合成模块"            ON)
option(AURA_BUILD_BARGE_IN   "编译打断裁决模块"            ON)
option(AURA_BUILD_NET        "编译网络传输模块"            OFF)

option(AURA_WITH_MNN         "启用 MNN 推理引擎（FetchContent）" ON)
option(AURA_WITH_MNN_LLM     "启用 MNN-LLM 管线（Phase 4，构建耗时大）" OFF)
option(AURA_WITH_TRICKROOM   "启用 TrickRoom 3A 引擎（Phase 2，super-build）" OFF)

option(AURA_BUILD_TESTS      "构建单元测试"       ON)
option(AURA_BUILD_HOST_SIM   "构建 PC 全链路仿真" ON)

# ------------------------------------------------------------------ 平台选择
# OSAL 只有一个后端：POSIX（Linux / Windows + MinGW-w64）。不做 RTOS 后端，
# 也不留 AURA_PLATFORM 覆盖口子 —— 多一个后端就多一条永远不会被 CI 跑到的
# 代码路径，而它的腐坏要到真正上板那天才暴露。
set(AURA_OS_NAME "posix" CACHE STRING "OSAL 后端（当前仅 posix）" FORCE)
message(STATUS "Aura: os=${AURA_OS_NAME} platform=${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")

# ------------------------------------------------------- 编译期配置头（生成）
configure_file(
	${CMAKE_CURRENT_SOURCE_DIR}/cmake/aura_config.h.in
	${CMAKE_CURRENT_BINARY_DIR}/generated/aura_config.h
	@ONLY
)

# ------------------------------------------------------------------ 第三方依赖
if(AURA_WITH_MNN)
	include(cmake/mnn.cmake)
else()
	message(STATUS "Aura: MNN disabled (AURA_WITH_MNN=OFF) — inference 层将不可用")
endif()

if(AURA_WITH_TRICKROOM)
	include(cmake/trickroom.cmake)
endif()

# -------------------------------------------------------------------- 目标
add_subdirectory(src)
add_subdirectory(apps)
if(AURA_BUILD_TESTS)
	enable_testing()
	add_subdirectory(tests)
endif()

message(STATUS "Aura ${PROJECT_VERSION} configured (build type: ${CMAKE_BUILD_TYPE})")
