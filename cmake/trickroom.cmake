# TrickRoom — 3A 音频引擎（WebRTC 系：AEC3/AECM/NS/AGC/VAD/BF 等）
# 下载并构建其 libAE_* 静态库目标，直接 target_link_libraries 使用。
#
# 注意：
# - TrickRoom 是 CMake super-build：顶层自带 FetchContent 拉取 abseil/neon-fft/
#   pffft/eigen（均 URL+SHA256，构建时需要联网）。
# - 产物为 AE_AEC / AE_AECM / AE_NS / AE_AGC / AE_AGC2 / AE_BF / AE_VAD /
#   AE_SRC / AE_HS / AE_IE / AE_TS / AE_DR 等静态库目标（各带 PUBLIC include 目录）。
# - 没有全局 include/ lib/ 目录，不要用 include_directories/link_directories 指向源码树。

set(TRICKROOM_VERSION "v0.1.0")
set(TRICKROOM_URL "https://github.com/Ryuk17/TrickRoom/archive/refs/tags/${TRICKROOM_VERSION}.tar.gz")
set(TRICKROOM_URL_HASH "SHA256=c55c696ca232a222b0c194c5be94fa51ab396b3f9e1317cf212ce1e858dce2a2")

# 只构建音频引擎库；客户端（Windows only）保持关闭
set(BUILD_TRICKROOM OFF CACHE BOOL "Build trickroom client" FORCE)

# 只保留 Aura 用得到的 6 个算法：AEC/NS/VAD/AGC/BF/SRC（链 aec3,bf,ns,agc,src）。
# 其余 6 个（AECM/AGC2/HS/IE/TS/DR）连库带测试一起关掉 —— 每多一个库就多一份
# 编译时间与磁盘占用，而它们不会出现在任何链上。各 AE_* 库互相独立（AGC2 的
# rnn_vad 权重编在 AE_AGC2 内部），关掉不会影响保留的库。
# 这些 option 定义在 TrickRoom 的 src/audio_engine/CMakeLists.txt 里，
# 但 option() 不覆盖已存在的 cache 项 —— 在这里先设好即可生效。
foreach(_ae_unused AECM AGC2 HS IE TS DR)
	set(BUILD_AE_${_ae_unused} OFF CACHE BOOL "TrickRoom: ${_ae_unused} 未使用" FORCE)
endforeach()

FetchContent_Declare(TrickRoom
	URL ${TRICKROOM_URL}
	URL_HASH ${TRICKROOM_URL_HASH}
)
FetchContent_MakeAvailable(TrickRoom)

# abseil 由 TrickRoom 在自身 CMakeLists 里 FetchContent 进来（BUILD_AUDIO_ENGINE
# 开关之内，AE_* 都 link absl::strings）。其中 absl_time_zone 在 MinGW 下编不过：
# cctz 的 time_zone_lookup.cc 走 WinRT 路径去取系统时区，而 MinGW 的 WinRT 头
# 既没有全局的 ::WindowsCreateStringReference 一族（它只提供
# ABI::Windows::Foundation 里的蛇形命名版本），又有 IReference<boolean> 与
# IReference<BYTE> 的重复定义。abseil 本来用 `!defined(__MINGW32__)` 把 MinGW
# 挡在这条路外，但同一个 #if 的另一个分支是 `NTDDI_VERSION >= NTDDI_WIN10_NI`，
# 而新版 MinGW-w64 的 sdkddkver.h 默认就满足它 —— 于是又被拉了回去。
#
# 把 NTDDI_VERSION 钉在 NTDDI_WIN10（0x0A000000 < NTDDI_WIN10_NI）即可落回
# 注册表路径，那本来就是 Windows 上取时区的传统做法，不是降级。只作用于这一个
# 目标，不污染全局编译选项。
if(WIN32 AND NOT MSVC AND TARGET absl_time_zone)
	target_compile_definitions(absl_time_zone PRIVATE NTDDI_VERSION=0x0A000000)
	message(STATUS "Aura: MinGW 下把 absl_time_zone 钉到 NTDDI_WIN10（绕开 WinRT 时区路径）")
endif()
