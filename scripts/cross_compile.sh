#!/usr/bin/env bash
# Aura — 交叉编译脚本（PC → ARM Linux SBC / 树莓派）
#
# 用法:
#   ./scripts/cross_compile.sh aarch64            # 64 位 ARM（树莓派 64 位系统）
#   ./scripts/cross_compile.sh armhf              # 32 位 ARM
#   ./scripts/cross_compile.sh aarch64 build-arm  # 指定构建目录
#
# 环境变量:
#   JOBS       并行编译数（默认 nproc）
#   AURA_OPTS  附加 cmake 参数（如 -DAURA_BUILD_TESTS=OFF）
#
# 前置条件: 安装了对应交叉工具链
#   Debian/Ubuntu: sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu
#   （armhf 对应 gcc-arm-linux-gnueabihf）
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TARGET="${1:-aarch64}"
BUILD_DIR="${2:-$ROOT/build-$TARGET}"
JOBS="${JOBS:-$(nproc 2>/dev/null || echo 4)}"

case "$TARGET" in
	aarch64)
		TC_FILE="$ROOT/toolchains/aarch64-linux-gnu.toolchain.cmake"
		;;
	armhf)
		TC_FILE="$ROOT/toolchains/arm-linux-gnueabihf.toolchain.cmake"
		;;
	*)
		echo "usage: $0 aarch64|armhf [build_dir]" >&2
		exit 1
		;;
esac

# 板端不跑 PC 单测/host_sim；推理引擎在板端必须保留（MNN 是主路径）。
cmake -S "$ROOT" -B "$BUILD_DIR" \
	-DCMAKE_TOOLCHAIN_FILE="$TC_FILE" \
	-DCMAKE_BUILD_TYPE=Release \
	-DAURA_BUILD_TESTS=OFF \
	-DAURA_BUILD_HOST_SIM=OFF \
	-DAURA_WITH_MNN=ON \
	"${AURA_OPTS:-}"

cmake --build "$BUILD_DIR" -j "$JOBS"

echo "完成。产物在 $BUILD_DIR（库: libaura_*.a）"
echo "推送到板子: scp -r $BUILD_DIR/* pi@<board>:/path/  （或 scripts/flash_board.sh, Phase 后续）"
