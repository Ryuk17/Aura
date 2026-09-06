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

FetchContent_Declare(TrickRoom
	URL ${TRICKROOM_URL}
	URL_HASH ${TRICKROOM_URL_HASH}
)
FetchContent_MakeAvailable(TrickRoom)
