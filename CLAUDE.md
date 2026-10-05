# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## 项目概况

Aura — 板端（树莓派 / Linux SBC）全链路语音交互框架：
`mic → AFE(3A/BSS) → VAD → KWS → 声纹 → ASR → LLM/Agent → TTS → playback`。
端侧全离线为主路径，云端仅增强/兜底；推理统一走 MNN。项目文档用中文撰写。

**状态：Phase 1（框架骨架）已完成**（2026-09-30），构建/OSAL/pipeline/事件总线/状态机/推理封装已落地并在 PC 上跑通。**Phase 2 前置「算法统一接口」已完成**（2026-10-02，见下文与 [docs/algorithm_unified_api.md](docs/algorithm_unified_api.md)）；**Phase 2 语音前端主体（AEC3+BF+NS+AGC+SRC）也已打通**（2026-10-05，麦克风未接，走读文件方式验证：合成回声场景 + 真实阵列录音，CTest 22/22 全绿）。已实现的结构见 [docs/architecture.md](docs/architecture.md)，节点契约与状态机见 [docs/pipeline.md](docs/pipeline.md)。总计划与 Phase 2–5 待办见 [docs/todo.md](docs/todo.md)（唯一权威文档，开工前先读）。尚未落位的设计文档（barge_in.md / risk.md / porting_guide.md）按该文档落位。

> ⚠️ `docs/` **不入版本库**（`.gitignore` 里有 `docs/`）—— 设计文档只在本地工作副本中存在，
> `git ls-files docs/` 为空。引用 docs/ 下文件时注意这一点。

## 构建

- 构建系统：CMake + FetchContent（第三方依赖不进 git，构建时拉取；.gitmodules 已删除）：
  - MNN **3.6.1** — [cmake/mnn.cmake](cmake/mnn.cmake)（3.3.0 加载不了 LLM/TTS 权重；带 `If` 子图的模型必须走 `Express::Module`，见 [docs/architecture.md](docs/architecture.md) 第 4 节）
  - TrickRoom（3A 音频引擎 v0.1.0）— [cmake/trickroom.cmake](cmake/trickroom.cmake)（URL+HASH 已于 2026-09-06 修复并编译验证）
- TrickRoom 是 CMake super-build（自带 FetchContent 拉 abseil/neon-fft/pffft/eigen），产物为 `libAE_*` 静态库目标（AE_AEC/AE_AECM/AE_NS/AE_AGC/AE_AGC2/AE_BF/AE_VAD/AE_SRC/…），直接 `target_link_libraries` 使用；无全局 include/lib 目录。
- 工具链文件在 [toolchains/](toolchains/)：`windows-x86_64.cmake`（当前主开发平台，MinGW-w64）、`linux-x86_64.cmake`、`aarch64-linux-gnu.toolchain.cmake`、`arm-linux-gnueabihf.toolchain.cmake`
- Windows/MinGW 工具链**静态链接 GCC 运行时**（`-static -static-libgcc -static-libstdc++`）：否则产物依赖 `libwinpthread-1.dll`，被 Git Bash 的 `/mingw64/bin` 抢先加载会报 `STATUS_ENTRYPOINT_NOT_FOUND (0xc0000139)`。
- 配置与编译（PC 开发）：
  ```bash
  cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_TOOLCHAIN_FILE=toolchains/windows-x86_64.cmake
  cmake --build build -j
  ctest --test-dir build --output-on-failure
  ```
- 模型权重不入库：下载方式见 [models/download.sh](models/download.sh)（hfd.sh + `HF_ENDPOINT=https://hf-mirror.com`）。清单：ASR `sherpa-mnn-streaming-zipformer-bilingual-zh-en-2023-02-20`、TTS `Kokoro-82M`、LLM `Qwen3.5-0.8B-MNN`、Embedding `Qwen3-Embedding-0.6B-MNN`、轮次检测 `smart-turn-v3.2-gpu.mnn`、VAD `silero_vad.mnn`
- 语音前端验证工具（麦克风接不上时的两条路径）：
  ```bash
  # 合成回声场景 → 真链 aec3,bf,ns,agc,src → 每个节点 PCM 落盘（人工听评）
  ./build/tools/audio_debug/audio_debug --gen-scene /tmp/scene
  ./build/tools/audio_debug/audio_debug --in /tmp/scene_mic.wav --ref /tmp/scene_ref.wav \
      --config tests/audio_front/configs/frontend_synth.conf --out-dir /tmp/out --check
  ```
  真实录音（`assets/audio/microphone_array` 的 CH0/CH1，**无播放回采** → AEC 走"缺参考→静音顶替"的降级路径）用 `--in` 且不带 `--ref`。`--profile` 打印帧级能量剖面（排查"哪一段被吃了"）。
- 测试：`tests/unit`（零依赖 C11 测试框架，PC 上跑）+ `tests/host_sim`（PC 全链路仿真，`host_sim_in.wav` 喂音频驱动 pipeline+event_bus+状态机）+ `tests/audio_front`（真算法链的指标断言与听评落盘）。CTest 当前 **22/22** 通过（含 TrickRoom 自带的 6 条 `test_ae_*` 引擎回归、`host_sim_silero`：真实 silero 模型经算法链装配跑完 7s 全链路）；host_sim 另有 `--voiceprint-fail` / `--inject-error N` / `--use-silero` / `--config <path>` 变体。
  ⚠️ host_sim 里除 silero VAD 外都是 **mock 算法节点**（KWS 匹配 / 声纹概率 / ASR 定长延迟 / LLM 流式出 token / TTS 按 token 出块），只为验证骨架，**不是真实算法** —— 真实实现在 Phase 3/4。
  ⚠️ `tests/audio_front` 的落盘 WAV 在 `build/tests/audio_front{,_real}/`；`test_audio_debug_real` 在素材缺失时 **SKIP（返回成功）**，别把它当成"真实录音验过了"。

## 架构要点（详见 docs/todo.md 第 3–4 节）

### 分层依赖（自上而下，禁止反向依赖）

```
apps → agent/net → kws/asr/llm/tts/voiceprint（各含 router）→ audio/dsp/inference → core/utils → platform(HAL)
```

- 跨模块通信只走 core 的 pipeline 消息与 event_bus，不直接 include 别的算法模块
- 算法模块只依赖 inference（MNN 封装）接口，不直接碰 MNN/NPU SDK 头
- 只有 platform/ 允许 include 板级 SDK 头（osal 只有 POSIX 后端，**不支持 RTOS**）；移植只改这一层
- agent 层不做信号处理、不跑推理，只消费事件/文本并下发启停指令；net 层只做传输与网络状态事件上报，端云路由切换在 asr/llm 的 router 内

### Pipeline 节点契约

- 三种流：音频帧流（PCM）、事件流（event_bus）、文本 chunk 流；一个 node 可同时消费音频+事件
- 推模式；`process()` 内禁止长阻塞 —— ASR/LLM/TTS 大推理内部抛子 task，process 只做入队
- 所有音频帧必须携带 `pts`（pipeline 层统一维护）：AEC 参考/多麦对齐、BSS、barge-in 裁决都依赖它
- 错误处理：node 不得 exit；异常上抛 event_bus 错误事件 → 状态机迁入 Error，可配置自动复位回 Idle

### 算法统一接口（Phase 2 前置，已落地）

TrickRoom（DSP 族）与 MNN（NN 族）走**同一套接口**：算法描述表（`core/algorithm.h`，
只回答"流形状"+"怎么造"）→ 注册表（名字→描述表）→ 链组装（`chain = aec3, ns,
tee(silero_vad, kws), asr` + `chain_param_<算法>.<键>`）。新增算法 = 填一张描述表 + 几个回调，
**不改 pipeline/agent，也不写配置解析代码**。拓扑 = 树形受限 DAG（线性主链 + tee 扇出旁路 +
AEC 参考第二输入，不做通用有向图）。

- DSP 族模板 [src/dsp/aura_dsp_adapter.h](src/dsp/aura_dsp_adapter.h)，真实描述表 [src/dsp/trickroom/](src/dsp/trickroom/)（AEC/NS/VAD）
- NN 族模板 [src/algorithm/aura_nn_adapter.h](src/algorithm/aura_nn_adapter.h)（滑窗 `aura_nn_window_*`、`NN_SYNC`；`NN_ASYNC` 留接口未实现）
- 配置三类错误（算法名未注册 / 键不在 param_specs / 值类型不符）一律**装配期报错**，不静默失效
- 详见 [docs/algorithm_unified_api.md](docs/algorithm_unified_api.md)

### 状态机

`Idle → Listening → Thinking → Speaking → Error`；`Interrupted` 是迁移事件不是可停留状态。
Speaking 期间 AEC/VAD/KWS 继续运行；有效打断 = 停 TTS 播放 + **中止 LLM 推理 task**（推理须支持外部中止）+ 丢弃未播内容，迁回 Listening。
barge-in 多因子裁决：AEC 近端能量 + 播放标志 + NN-VAD 概率 + KWS 命中（优先）+ DoA（可选）；播放期间抬高 VAD 阈值，宁漏打断不可自打断。

### 模型内存策略

- 常驻：KWS / VAD / 声纹；按需：ASR / TTS；LLM 视内存预算常驻，否则利用唤醒提示音播放窗口预加载，会话内 KV cache 保热
- MNN MemoryPool 全局统一、多模型复用；只读权重走 mmap；禁止运行时 malloc/free，走 utils/mem_pool

### 其他已定决策

- 对外 API 纯 C（`include/agent_export.h`），只暴露 init/start/stop/事件回调/配置；上层不得直接操作 pipeline/node 内部
- 声纹校验在 KWS 之后、ASR 之前；失败直接退回 Idle（省算力）；spk ID 绑 session，短期记忆按说话人隔离
- 端侧优先：local 链路必须独立闭环可演示；router 语义 = 端侧优先，云端仅增强/兜底
- 存储：Linux/eMMC 用 sqlite；所有 flash 写（长记忆/日志）走缓冲+批量落盘，禁止高频写
- 验收指标：唤醒→TTS 出声 < 1s（全离线）；LLM 首 token < 500ms；误唤醒 24h < 1 次；打断后 200ms 内停播；声纹拦截 ≥95% / 误拒 <5%；断网状态下全功能可用

## 开发阶段（Phase 0–5，见 docs/todo.md 第 7 节）

Phase 0 选型 → Phase 1 框架骨架（★出口：PC host_sim 无硬件跑通）→ **Phase 2 前置：算法统一接口 ✅** → Phase 2 语音前端（3A/BSS/VAD；★tools/audio_debug 各节点 PCM 落盘，调参强依赖；3A 接入走 DSP 描述表）→ Phase 3 KWS/声纹/ASR → Phase 4 对话闭环（LLM/TTS/Agent/barge-in）→ Phase 5 云端增强与优化。
