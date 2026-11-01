# NE10（Ryuk17/neon-fft v0.1.0）— 语音前端共用的实数 FFT
#
# 为什么语音前端需要它：fbank 每 10ms 要对一帧 512 点实数信号做 FFT（功率谱），
# 全流程里最热的一步。TrickRoom 的 AEC3 已经在用它，我们沿用同一个库、同一套
# 调用姿势（见 src/dsp/aura_fft.cpp），避免"两个 FFT 实现算出的特征不一致"。
#
# 与 TrickRoom 的关系：**同一份源码，不重复下载**。
# TrickRoom 自己在 super-build 里 FetchContent 了 neon-fft（名字也是 neon-fft），
# 而 FetchContent 的 populate 状态是**全局**的 —— 所以本文件必须在
# cmake/trickroom.cmake **之前** include：我们先声明并 populate，TrickRoom 随后
# 的 FetchContent_GetProperties 看到已 populate 就直接复用，第二次不下载。
#
# 只用它的**源码**，不用它的 CMakeLists：那个会建一个 libNeFFT 共享库，
# 而我们只要其中 4 个 .c（TrickRoom 也是这么干的）。.neon.c / .neonintrinsic.c
# 明确排除 —— 它们是 ARM NEON 专用，排除后 x86 与 ARM 走同一份通用 C 实现，
# 特征数值完全一致（跨平台对齐比单端那点性能重要）。
#
# 顺带一提：URL 用 codeload 直链而非 github.com/archive —— 后者会 302 到
# codeload，而 github.com 在某些网络下连不上。两份 tarball 字节相同，
# 所以 hash 与 TrickRoom 钉的那个直接对齐。

set(NEON_FFT_VERSION "v0.1.0")
set(NEON_FFT_CLEAN_URL "https://codeload.github.com/Ryuk17/neon-fft/tar.gz/refs/tags/${NEON_FFT_VERSION}")
set(NEON_FFT_CLEAN_HASH "SHA256=4b557d080a09c9eb5d1852b32daa8f022816772c58a9addc19c032f5b89a30d4")

FetchContent_Declare(neon-fft
	URL ${NEON_FFT_CLEAN_URL}
	URL_HASH ${NEON_FFT_CLEAN_HASH}
	DOWNLOAD_EXTRACT_TIMESTAMP ON
)

# 只 populate（取源码），不 add_subdirectory。CMP0169 是 CMake 3.30+ 对
# FetchContent_Populate 的弃用策略 —— 与 TrickRoom 里同样置 OLD。
if(POLICY CMP0169)
	cmake_policy(SET CMP0169 OLD)
endif()
FetchContent_GetProperties(neon-fft)
if(NOT neon-fft_POPULATED)
	FetchContent_Populate(neon-fft)
endif()

add_library(aura_neon_fft STATIC
	${neon-fft_SOURCE_DIR}/src/NE10_fft.c
	${neon-fft_SOURCE_DIR}/src/NE10_fft_float32.c
	${neon-fft_SOURCE_DIR}/src/NE10_fft_generic_float32.c
	${neon-fft_SOURCE_DIR}/src/NE10_rfft_float32.c
)
target_include_directories(aura_neon_fft PUBLIC ${neon-fft_SOURCE_DIR}/include)

message(STATUS "NE10 ${NEON_FFT_VERSION} ready (source: ${neon-fft_SOURCE_DIR})")
