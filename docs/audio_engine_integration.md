# TrickRoom audio_engine 集成方案

> 状态：**已确认（2026-08-23）**，等待按 implementation_plan 步骤实施
> 上游：https://github.com/Ryuk17/TrickRoom （`src/audio_engine` 子树）
> 原则：`third_party/` 只放第三方代码且**全部 submodule 管理**；Aura 自身代码只出现在 `src/`、`include/`、`tests/`、`examples/`、`docs/`、`scripts/`、`resources/`

---

## 1. 背景与目标

Aura 上一轮曾以「按需摘取」方式引入 TrickRoom 的 AECM（`third_party/aecm` + `src/audio/` 手写封装），后续添加 AGC/AEC3 时暴露该模式的缺陷：12 个算法共享的 `signal_processing`、`neon-fft`、`audio_processing` 公共代码需要反复重新摘取。该集成已全部删除（见 git 历史），本轮改为**整体引入 audio_engine 子树**，扩充 Aura 的音频算法边界：

| 库 | 算法 | Aura 用途 |
|---|---|---|
| libAE_AECM | 回声消除（移动版） | M1/M4 基础：farend 接 TTS 输出 |
| libAE_AEC | 回声消除 AEC3 | 高算力平台/未来算力卡的升级选项 |
| libAE_NS | 降噪 | ASR 前降噪，嘈杂环境识别率 |
| libAE_SRC | 重采样 | HAL 设备采样率 → 全链路 16 kHz 兜底 |
| libAE_AGC2 | 自适应增益（含 rnn_vad） | AudioFocus 统一音量控制 |
| libAE_AGC | 增益（legacy） | AGC2 的备选/对照 |
| libAE_VAD | WebRTC VAD | silero 的零模型开销预检/降级备选 |
| libAE_BF | 波束形成 | 多麦阵列（储备） |
| libAE_HS | 啸叫抑制 | 扬声器近麦场景（储备） |
| libAE_IE | 可懂度增强 | 储备 |
| libAE_TS | 瞬态抑制 | 储备 |
| libAE_DR | 去混响（需 Eigen） | 储备 |

## 2. 已确认决策（2026-08-23 review 通过）

| 决策点 | 结论 |
|---|---|
| R1 平台验证顺序 | **先 x86_64-linux**（`toolchains/linux-x86_64.cmake`）验证全量编译；aarch64-linux、windows-x86_64 随后 |
| R2 absl 依赖 | **保留**（浅依赖：仅 `absl::string_view` ×30、`absl::StrCat` ×8），submodule 引入，不剥离 |
| R3 golden wav | **放入 `resources/`**（`resources/audio_engine/data/`），供 Aura golden 回归使用 |
| ① 依赖管理 | **全部 submodule**（trickroom 及其依赖），`git clone --recursive` 即可复现环境 |
| ② 引入范围 | **全量引入**，12 个 `libAE_*` 全部编译 |
| ③ DR/Eigen | **不排除**，`BUILD_AE_DR=ON`，Eigen 以 submodule 引入 |

## 3. third_party 布局（submodule）

```
third_party/
├── trickroom/          ← https://github.com/Ryuk17/TrickRoom.git（整仓库；只用其 src/audio_engine）
├── neon-fft/           ← TrickRoom 同款 NE10 FFT（AECM/NS/BF/IE/TS 等依赖）
├── pffft/              ← AGC2 rnn_vad 依赖
├── eigen/              ← DR（去混响 EKF FFT）依赖，header-only
└── abseil-cpp/         ← 仅 absl::strings（接口/内部日志用）
    └── install/        ← 预编译产物（x86_64-linux 一份，aarch64 交叉一份），由 scripts/ 构建
```

说明：

- trickroom 以**整仓库** submodule 引入（而非单独裁 audio_engine），保持上游可同步；CMake 只 `add_subdirectory(third_party/trickroom/src/audio_engine)`。
- trickroom 依赖库**不复用** TrickRoom 仓库内的 submodule 路径，而是作为 Aura 的独立 submodule 直接放在 `third_party/` 根下，因此克隆 Aura 时**不需要** `--recursive` 拉 TrickRoom 的嵌套 submodule。
- 上述路径与 audio_engine 内部 `THIRD_PARTY_ROOT` 假设（`<repo>/third_party`）不一致，由补丁 0001 修正（见 §4）。

## 4. 上游改动与补丁机制

audio_engine 按 TrickRoom CLAUDE.md 约束视为**只读上游**（`interface/` 之外不改代码）。所需改动全部集中在 Aura 侧：

```
patches/
├── 0001-audio-engine-third-party-root.patch   # CMakeLists 层
└── 0002-audio-engine-avx2-platform-guard.patch # CMakeLists 层
scripts/apply_patches.sh                       # 检出 submodule 后自动应用
```

| 补丁 | 改动 | 原因 |
|---|---|---|
| 0001 | `THIRD_PARTY_ROOT` 改为可被父级覆盖（`if(NOT DEFINED ...)`），由 Aura 指向 `third_party/`；库/测试产物输出改为 `${CMAKE_BINARY_DIR}` 下 | 依赖目录布局与 Aura 不同；避免产物污染 submodule 工作区（`git status` 报 dirty） |
| 0002 | 6 个 `*_avx2.cc` 文件的 `set_source_files_properties(... -mavx2 -mfma)` 与编译纳入包进 `if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64\|AMD64")` | **已实测**：AVX2 文件无任何编译保护（直接 `#include <immintrin.h>`），aarch64 上不排除则编译必失败 |

回馈：两个补丁同步提 PR 到上游，上游合并后降级为可选。

## 5. CMake 接入（Aura 根 CMakeLists 草案）

```cmake
# 第三方库（全部 submodule）
set(THIRD_PARTY_ROOT ${PROJECT_SOURCE_DIR}/third_party)
set(CMAKE_PREFIX_PATH "${THIRD_PARTY_ROOT}/abseil-cpp/install" ${CMAKE_PREFIX_PATH})

# audio_engine：12 个 libAE_* 全量编译（含自带 unitest）
set(BUILD_AE_VAD ON)  …  set(BUILD_AE_DR ON)   # 上游默认即 ON，可省略
add_subdirectory(${THIRD_PARTY_ROOT}/trickroom/src/audio_engine)

# Aura 模块
add_subdirectory(src/utils)
add_subdirectory(src/core)
add_subdirectory(src/platform)
add_subdirectory(src/audio)      # 重建，链接 AE_AECM/AE_NS/AE_SRC/AE_AGC2/AE_VAD
```

注意：

- audio_engine 内部 C++20，Aura 侧保持 C++17，target 级属性互不干扰。
- 自带 unitest（`test_ae_*`）经 `add_test` 注册，其 golden 路径为相对 `data/xxx.wav`，工作目录由 Aura 侧设为 audio_engine 源码目录。
- Windows 构建时 `windows-x86_64.cmake` 已含全局 `-mavx2 -mfma`，与补丁 0002 的 x86_64 分支行为一致。

## 6. 资源与测试数据

| 数据 | 位置 | 用途 |
|---|---|---|
| TrickRoom 自带 `data/*.wav` | 保留在 submodule 内（unitest 相对路径依赖） | audio_engine 自带 unitest 输入/参考输出 |
| Aura golden 音频 | `resources/audio_engine/data/`（自 TrickRoom data 拷贝） | Aura 自有算法链测试与回归（`tests/golden/` 用例引用） |

## 7. 平台矩阵

| 平台 | 工具链 | 状态 |
|---|---|---|
| x86_64-linux（开发机） | `toolchains/linux-x86_64.cmake` | **先行验证**：12 库 + 12 unitest 全绿 |
| aarch64-linux（RPi） | `toolchains/aarch64-linux-gnu.toolchain.cmake` | 补丁 0002 后编译；absl 需交叉构建（TrickRoom 自带的 aarch64 工具链面向嵌入式板卡、含 `-nodefaultlibs -nostdinc++`，**不可复用**） |
| x86_64-windows（MinGW） | `toolchains/windows-x86_64.cmake` | 上游已处理 winmm/导出宏，直接验证 |

## 8. 分步实施与验收

| 步骤 | 内容 | 验收 |
|---|---|---|
| 0 | 修复根 CMakeLists（移除对已删 `third_party/aecm`、`src/audio` 的引用），清理 git 状态 | 空工程恢复编译 |
| 1 | `git submodule add` 五个仓库 + `scripts/apply_patches.sh` + 补丁落地；absl x86_64 预构建 | `cmake -B build/linux-x86_64`（`-DCMAKE_TOOLCHAIN_FILE=toolchains/linux-x86_64.cmake`）通过 |
| 2 | 全量编译 12 个 libAE_* + 自带 unitest 全绿；输出 wav 与参考比对脚本化 | 12 库 + 12 测试通过（算法边界扩充的第一个验收点） |
| 3 | aarch64 交叉适配（补丁 0002 生效 + absl 交叉构建 + 交叉 unitest 冒烟） | aarch64 编译通过（真机验证留待 M4.5） |
| 4 | 重建 `src/audio/`：AudioChain 框架 + AECM 节点（libAE_AECM 统一 C API）+ AudioFocus | M1.6/M1.7 验收（回声残余阈值、demo 回环） |
| 5 | NS/SRC/AGC2/VAD 节点接入 + golden 回归脚本化 + 文档同步 | M2 对应步骤验收；回归一键跑通 |

## 9. 风险清单

| 风险 | 说明 | 对策 |
|---|---|---|
| aarch64 是上游未验证路径 | TrickRoom 工具链面向嵌入式板卡，通用 aarch64 构建可能还有隐藏问题 | 步骤 3 提前暴露；AVX2 之外的问题按需追加补丁 |
| absl 交叉编译成本 | aarch64 需在交叉工具链下构建 abseil | 纳入 `scripts/build_deps_aarch64.sh`，一次性成本 |
| 仓库体积 | submodule 不占 Aura 仓库体积；`resources/audio_engine/data/`（约 33 MB 拷贝）入仓库 | 可接受；必要时后续裁剪为 unitest 实际使用的子集 |
| submodule 漂移 | 上游更新可能破坏补丁上下文 | pin commit；升级流程：更新 commit → 重新生成补丁 → 回归 |
