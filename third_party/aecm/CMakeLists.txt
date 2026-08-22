# AECM（Acoustic Echo Canceller Mobile）— WebRTC 移植
# 来源：TrickRoom 仓库 src/audio_engine/audio_processing/acoustic_echo_cancellation_mobile/
#       保留原 BSD 版权头；算法实现为 WebRTC 源码，FFT 使用 NE10（仅 C 通用版本）。
# 对外暴露 C API（echo_control_mobile.h: WebRtcAecm_Create/Init/BufferFarend/Process）。

# 目录内所有源文件均为 AECM 必需的最小依赖集（信号处理公共函数 + AECM 算法 + NE10 FFT）
file(GLOB AECM_SRC
    ${CMAKE_CURRENT_SOURCE_DIR}/audio_processing/acoustic_echo_cancellation_mobile/*.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/signal_processing/*.c
    ${CMAKE_CURRENT_SOURCE_DIR}/signal_processing/*.cc
    ${CMAKE_CURRENT_SOURCE_DIR}/utils/ring_buffer.c
    ${CMAKE_CURRENT_SOURCE_DIR}/neon-fft/src/*.c)
# 排除平台专用实现（MIPS/NEON 汇编），仅用 C 通用版本
list(FILTER AECM_SRC EXCLUDE REGEX "_(mips|neon)\\.(c|cc)$")

add_library(aura_aecm STATIC ${AECM_SRC})
target_include_directories(aura_aecm PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/neon-fft/include)
