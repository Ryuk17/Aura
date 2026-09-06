# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概况

Aura — 板端（树莓派 / Linux SBC，远期 RTOS/MCU）全链路语音交互框架：
`mic → AFE(3A/BSS) → VAD → KWS → 声纹 → ASR → LLM/Agent → TTS → playback`。
端侧全离线为主路径，云端仅增强/兜底；推理统一走 MNN。项目文档用中文撰写。

**⚠️ 仓库正处于重构中**：旧骨架（src/ include/ tests/ examples/ resources/ 及旧 docs）已从工作区删除，顶层 CMakeLists.txt 正在重写（当前为空）。新架构的唯一权威文档是 [docs/todo.md](docs/todo.md)（含目标目录结构、分层规则、节点契约、状态机、模型内存策略、Phase 0–5 计划、验收指标）。开工前先读它；规划中的设计文档（architecture.md / pipeline.md / model_requirement.md / memory_budget.md / barge_in.md / risk.md / porting_guide.md）按该文档落位，勿按旧结构行事。

## 构建

- 构建系统：CMake + FetchContent（第三方依赖不进 git，构建时拉取；.gitmodules 已删除）：
  - MNN 3.3.0 — [cmake/mnn.cmake](cmake/mnn.cmake)
  - TrickRoom（3A 音频引擎 v0.1.0）— [cmake/trickroom.cmake](cmake/trickroom.cmake)（URL+HASH 已于 2026-09-06 修复并编译验证）
- TrickRoom 是 CMake super-build（自带 FetchContent 拉 abseil/neon-fft/pffft/eigen），产物为 `libAE_*` 静态库目标（AE_AEC/AE_AECM/AE_NS/AE_AGC/AE_AGC2/AE_BF/AE_VAD/AE_SRC/…），直接 `target_link_libraries` 使用；无全局 include/lib 目录。
- 工具链文件在 [toolchains/](toolchains/)：`linux-x86_64.cmake`（当前主开发平台）、`aarch64-linux-gnu.toolchain.cmake`、`arm-linux-gnueabihf.toolchain.cmake`、`windows-x86_64.cmake`
- 配置与编译（PC 开发）：
  ```bash
  cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=toolchains/linux-x86_64.cmake
  cmake --build build -j
  ```
- 模型权重不入库：下载方式见 [models/download.sh](models/download.sh)（hfd.sh + `HF_ENDPOINT=https://hf-mirror.com`）。清单：ASR `sherpa-mnn-streaming-zipformer-bilingual-zh-en-2023-02-20`、TTS `Kokoro-82M`、LLM `Qwen3.5-0.8B-MNN`、Embedding `Qwen3-Embedding-0.6B-MNN`、轮次检测 `smart-turn-v3.2-gpu.mnn`、VAD `silero_vad.mnn`
- 当前无测试代码（旧 tests/ 已删）。按计划：单测在 `tests/unit`（PC 上跑），`tests/host_sim` 是 PC 仿真（文件喂音频跑全链路）——Phase 1 出口条件即 host_sim 无硬件跑通 pipeline+event_bus+状态机。

## 架构要点（详见 docs/todo.md 第 3–4 节）

### 分层依赖（自上而下，禁止反向依赖）

```
apps → agent/net → kws/asr/llm/tts/voiceprint（各含 router）→ audio/dsp/inference → core/utils → platform(HAL)/git-commit
```

- 跨模块通信只走 core 的 pipeline 消息与 event_bus，不直接 include 别的算法模块
- 算法模块只依赖 inference（MNN 封装）接口，不直接碰 MNN/NPU SDK 头
- 只有 platform/ 允许 include 板级 SDK 头；移植只改这一层
- agent 层不做信号处理、不跑推理，只消费事件/文本并下发启停指令；net 层只做传输与网络状态事件上报，端云路由切换在 asr/llm 的 router 内

### Pipeline 节点契约

- 三种流：音频帧流（PCM）、事件流（event_bus）、文本 chunk 流；一个 node 可同时消费音频+事件
- 推模式；`process()` 内禁止长阻塞 —— ASR/LLM/TTS 大推理内部抛子 task，process 只做入队
- 所有音频帧必须携带 `pts`（pipeline 层统一维护）：AEC 参考/多麦对齐、BSS、barge-in 裁决都依赖它
- 错误处理：node 不得 exit；异常上抛 event_bus 错误事件 → 状态机迁入 Error，可配置自动复位回 Idle

### 状态机

`Idle → Listening → Thinking → Speaking → Error`；`Interrupted` 是迁移事件不是可停留状态。
Speaking 期间 AEC/VAD/KWS 继续运行；有效打断 = 停 TTS 播放 + **中止 LLM 推理 task**（推理须支持外部中止）+ 丢弃未播内容，迁回 Listening。
barge-in 多因子裁决：AEC 近端能量 + 播放标志 + NN-VAD 概率 + KWS 命中（优先）+ DoA（可选）；播放期间抬高 VAD 阈值，宁漏打断不可自打断。

### 模型内存策略

- 常驻：KWS / VAD / 声纹；按需：ASR / TTS；LLM 视内存预算常驻，否则利用唤醒提示音播放窗口预加载，会话内 KV cache 保热
- MNN MemoryPool 全局统一、多模型复用；只读权重走 mmap（RTOS 备选预拷贝 PSRAM）；禁止运行时 malloc/free，走 utils/mem_pool

### 其他已定决策

- 对外 API 纯 C（`include/agent_export.h`），只暴露 init/start/stop/事件回调/配置；上层不得直接操作 pipeline/node 内部
- 声纹校验在 KWS 之后、ASR 之前；失败直接退回 Idle（省算力）；spk ID 绑 session，短期记忆按说话人隔离
- 端侧优先：local 链路必须独立闭环可演示；router 语义 = 端侧优先，云端仅增强/兜底
- 存储：Linux/eMMC 用 sqlite；RTOS 用自建极简 KV；所有 flash 写（长记忆/日志）走缓冲+批量落盘，禁止高频写
- 验收指标：唤醒→TTS 出声 < 1s（全离线）；LLM 首 token < 500ms；误唤醒 24h < 1 次；打断后 200ms 内停播；声纹拦截 ≥95% / 误拒 <5%；断网状态下全功能可用

## 开发阶段（Phase 0–5，见 docs/todo.md 第 7 节）

Phase 0 选型 → Phase 1 框架骨架（★出口：PC host_sim 无硬件跑通）→ Phase 2 语音前端（3A/BSS/VAD；★tools/audio_debug 各节点 PCM 落盘，调参强依赖）→ Phase 3 KWS/声纹/ASR → Phase 4 对话闭环（LLM/TTS/Agent/barge-in）→ Phase 5 云端增强与优化。
