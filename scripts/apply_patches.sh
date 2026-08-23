#!/usr/bin/env bash
# 应用 third_party 子模块的上游补丁（详见 docs/audio_engine_integration.md §4）
# 幂等：已应用的补丁自动跳过（git apply --check 失败即视为已应用）
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TR_REPO="$ROOT/third_party/trickroom"

PATCHES=(
    "$ROOT/patches/0001-audio-engine-third-party-root.patch"
    "$ROOT/patches/0002-audio-engine-avx2-platform-guard.patch"
    "$ROOT/patches/0003-audio-engine-sanitizer-stddef.patch"
    "$ROOT/patches/0004-audio-engine-pffft-include.patch"
    "$ROOT/patches/0005-audio-engine-aligned-malloc.patch"
    "$ROOT/patches/0006-audio-engine-ctest-workdir.patch"
)

for p in "${PATCHES[@]}"; do
    name="$(basename "$p")"
    if git -C "$TR_REPO" apply --check "$p" 2>/dev/null; then
        git -C "$TR_REPO" apply "$p"
        echo "applied: $name"
    else
        echo "skip (already applied or conflicts): $name"
    fi
done
