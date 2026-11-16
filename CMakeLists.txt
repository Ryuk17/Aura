# Aura — 板端 Voice Agent 运行时
#
# 边界（详见 docs/architecture.md）：
#   Aura 只负责 voice agent：音频 I/O、pipeline、事件总线、状态机、Agent 数据流、
#   会话/编排/记忆/工具/打断。
#   算法全部由 soundproof 提供（FetchContent），Aura 只做 C ABI 适配。
#
# 快速开始（PC 开发，见 README）：
#   cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_TOOLCHAIN_FILE=toolchains/windows-x86_64.cmake
#   cmake --build build -j
#   ctest --test-dir build --output-on-failure
#
# 离线开发（不拉 GitHub，直接指向本地 soundproof 源码）：
#   -DFETCHCONTENT_SOURCE_DIR_SOUNDPROOF=F:/share/soundproof

cmake_minimum_required(VERSION 3.20)
project(Aura VERSION 0.2.0 LANGUAGES C CXX)

include(FetchContent)
list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------- 全局语言标准
# Aura 自身以 C11 为主；soundproof 是 C++20（它自己的目录作用域内设置）。
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# ------------------------------------------------------------------ 模块裁剪
# 与运行期配置的分工：这里决定"编不编进去"，configs/ 决定"怎么调"。
option(AURA_BUILD_KWS        "编入 KWS 适配节点"           ON)
option(AURA_BUILD_VOICEPRINT "编入声纹（SR）适配节点"       ON)
option(AURA_BUILD_ASR        "编入 ASR 适配节点"           ON)
option(AURA_BUILD_LLM        "编入 LLM 适配节点（需 soundproof LLM）" OFF)
option(AURA_BUILD_TTS        "编入 TTS 适配节点（需要 soundproof MNN）" ON)
option(AURA_BUILD_BARGE_IN   "编入打断裁决模块"             ON)
option(AURA_BUILD_NET        "编入网络传输模块"             OFF)

# soundproof：算法引擎。MNN 开关决定 KWS/ASR/SR/TTS/TURN/silero 是否可用；
# LLM 额外需要 MNN-LLM + OpenCV，默认关闭（Phase C 再打开）。
option(AURA_WITH_SOUNDPROOF     "启用 soundproof 算法引擎（FetchContent）" ON)
option(AURA_WITH_SOUNDPROOF_MNN "soundproof 启用 MNN（KWS/ASR/SR/TTS/TURN/silero）" ON)
option(AURA_WITH_SOUNDPROOF_LLM "soundproof 启用 MNN-LLM/VLM（重型：会拉 OpenCV）" OFF)
option(AURA_SOUNDPROOF_TESTS    "构建/运行 soundproof 自带的 test_ae_*" OFF)

option(AURA_BUILD_TESTS      "构建单元测试"       ON)
option(AURA_BUILD_HOST_SIM   "构建 PC 全链路仿真" ON)

if(AURA_WITH_SOUNDPROOF_LLM AND NOT AURA_WITH_SOUNDPROOF_MNN)
	message(WARNING "Aura: AURA_WITH_SOUNDPROOF_LLM 需要 MNN，已自动关闭")
	set(AURA_WITH_SOUNDPROOF_LLM OFF CACHE BOOL "" FORCE)
endif()
if(AURA_BUILD_LLM AND NOT AURA_WITH_SOUNDPROOF_LLM)
	message(STATUS "Aura: AURA_BUILD_LLM=ON 但 soundproof LLM 未启用 — LLM 适配节点不编译")
	set(AURA_BUILD_LLM OFF CACHE BOOL "" FORCE)
endif()

# ------------------------------------------------------------------ 平台选择
# OSAL 只有一个后端：POSIX（Linux / Windows + MinGW-w64）。不做 RTOS 后端。
set(AURA_OS_NAME "posix" CACHE STRING "OSAL 后端（当前仅 posix）" FORCE)
message(STATUS "Aura: os=${AURA_OS_NAME} platform=${CMAKE_SYSTEM_NAME}-${CMAKE_SYSTEM_PROCESSOR}")

# ------------------------------------------------------- 编译期配置头（生成）
configure_file(
	${CMAKE_CURRENT_SOURCE_DIR}/cmake/aura_config.h.in
	${CMAKE_CURRENT_BINARY_DIR}/generated/aura_config.h
	@ONLY
)

# ------------------------------------------------------------------ 第三方依赖
if(AURA_WITH_SOUNDPROOF)
	include(cmake/soundproof.cmake)
else()
	message(WARNING "Aura: AURA_WITH_SOUNDPROOF=OFF — 没有任何算法节点，只适合编译框架单测")
endif()

# -------------------------------------------------------------------- 目标
add_subdirectory(src)
add_subdirectory(apps)
if(AURA_BUILD_TESTS)
	enable_testing()
	add_subdirectory(tests)
endif()

message(STATUS "Aura ${PROJECT_VERSION} configured (build type: ${CMAKE_BUILD_TYPE})")
