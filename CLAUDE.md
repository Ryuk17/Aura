# CLAUDE.md

Aura — 板端 Voice Agent 运行时（树莓派 / Linux SBC）。

## 项目概况

```
mic → soundproof AFE/VAD/KWS/SR → ASR → Agent(flow/dialogue/tools/memory) → TTS → playback
```

Aura 只做 **Voice Agent 运行时**：音频 I/O、pipeline、事件总线、状态机、
Agent 数据流中间件、对话编排、工具、记忆、打断。**算法实现、模型权重、MNN、
算法测试全部在 [soundproof](https://github.com/Ryuk17/soundproof) 仓库**，
通过纯 C ABI `AudioEngine_*` 接入；Aura 的 `src/soundproof/` 只是适配层。

**状态：Phase A + Phase B + Phase C1（播放链/TTS）完成（2026-11-16）**：
- Aura 内算法代码/模型/工具已删除；`cmake/soundproof.cmake` FetchContent 接入；
- DSP/VAD/KWS/SR/ASR/TTS 适配节点 + `core/algorithm` 注册表；
- `src/agent/flow/`：bus/stage/middleware/session/store/bridge/metrics；
- Phase B：`src/agent/stages/` 五个 stage（session/dialogue/memory/tool_runner/
  orchestrator）+ `tools/` 注册表 + `llm/` 后端抽象（mock）+ 8 个中间件；
- 消息链路：`asr.final → dialogue.intent → tool.call → tool.result → tts.say`，
  call_id 幂等、重试、barge-in 取消；
- **Phase C1**：`tts_egress` → `playback_chain = tts, src, playback`；
  `playback` sink（HAL 后端 + AEC 回采）；TTS 适配同步引擎 → 异步 worker +
  10ms 分帧 + 实时 pacing；barge-in 时 flush 清队列 + generation 丢弃 + sink.flush；
- 测试：核心单测 + `test_flow` + `test_agent_stages` + `test_agent_orchestrator`
  + `test_playback` + `flow_chain` + `playback_chain` + `host_sim` + 真实 VAD，
  `ctest -L aura` 15/15；
- 剩余：AE_LLM 后端（C2）、真实模型/板级 HAL（C3）、soundproof 流式/abort。

> ⚠️ `docs/` **不入版本库**（`.gitignore` 有 `docs/`），设计文档只在本地工作副本中存在。
> 唯一权威计划：[docs/todo.md](docs/todo.md)（开工前先读）。

## 构建

- CMake + FetchContent。第三方：
  - **soundproof**（算法引擎，含 MNN/abseil/neon-fft/pffft/eigen）——
    [cmake/soundproof.cmake](cmake/soundproof.cmake)。
  - `GIT_TAG master`，集成稳定后建议改 tag/commit。
- 配置与编译（PC 开发）：
  ```bash
  cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_TOOLCHAIN_FILE=toolchains/windows-x86_64.cmake
  cmake --build build -j
  ctest --test-dir build -L aura --output-on-failure   # 只跑 Aura 自己的测试
  ```
- 离线开发（不拉 GitHub）：`-DFETCHCONTENT_SOURCE_DIR_SOUNDPROOF=F:/share/soundproof`
- 构建开关：
  - `AURA_WITH_SOUNDPROOF_MNN`（默认 ON）：KWS/ASR/SR/TTS/TURN/silero；
    OFF 时只构建 DSP，最快。
  - `AURA_WITH_SOUNDPROOF_LLM`（默认 OFF）：MNN-LLM + OpenCV，重型。
  - `AURA_BUILD_KWS/VOICEPRINT/ASR/LLM/TTS/BARGE_IN/NET`：是否编入适配节点。
- AE_KWS 与 AE_ASR 都把共享 transducer/hypothesis 源编进各自静态库，同时链接会
  重复定义；`aura_soundproof` 用 `--allow-multiple-definition` 兜底（见
  src/CMakeLists.txt 与 docs/soundproof_integration.md 第 6 节）。
- Windows/MinGW 工具链静态链接 GCC 运行时（`-static -static-libgcc -static-libstdc++`），
  否则 Git Bash 的 `/mingw64/bin` 会抢先加载 `libwinpthread-1.dll`，
  报 `STATUS_ENTRYPOINT_NOT_FOUND (0x0000139)`。
- 交叉编译：`scripts/cross_compile.sh aarch64|armhf`；工具链在 [toolchains/](toolchains/)。

## 架构要点（详见 docs/architecture.md）

### 三个平面

1. **音频实时面**（`core/pipeline`）：PCM、pts、帧池、每节点任务；允许丢帧、
   禁止长阻塞、错误上抛 event_bus。
2. **Agent 数据面**（`src/agent/flow` + `src/agent/stages`）：topic 路由、每 stage
   有界队列、背压策略、中间件链、取消令牌、session/turn 上下文、trace/metrics；
   session/dialogue/memory/tool_runner/orchestrator 五个 stage。
3. **控制/观察面**（`core/event_bus` + 状态机）：广播状态/错误/底层事件，可丢。

`flow_bridge` 负责 event_bus ↔ flow 的转换；两平面不直接 include。

### 分层依赖（禁止反向）

```
apps → agent（agent_core + agent/flow + dialogue/tools/memory）
        → core（pipeline/event_bus/state_machine/algorithm/config）
        → utils → platform
        → soundproof（适配层）── C ABI ──▶ soundproof 仓库
```

- Agent 层不做信号处理、不跑算法、不 include soundproof 算法头。
- 适配层只 include `audio_engine_*.h`（纯 C），不碰 MNN 头。
- 跨模块通信只走 pipeline / event_bus / flow。
- 只有 `platform/` 允许板级 SDK 头。
- 对外 API 纯 C（`include/agent_export.h`）。

### 算法统一接口

- 描述表 `core/algorithm.h`：名字/kind/io/param_specs/create。
- 注册名：`aec, aecm, ns, agc, agc2, bf, src, hs, ts, vad, kws, sr, asr`；
  `tts/llm` 为 Phase C（当前 stub 不注册）。
- 链语法：`chain = bf, aec, ns, agc, tee(vad, kws, sr), asr`；
  `chain_param_<algo>.<key> = value`。
- 装配期校验三类错误：算法名未注册 / 键未声明 / 值类型不符。
- 音频源节点（`produces_audio && !consumes_audio`，如 TTS）不接麦克风上游；
  `chain_build` 已区分。
- 适配层详情：[docs/soundproof_integration.md](docs/soundproof_integration.md)。

### 状态机

`Idle → Listening → Thinking → Speaking → Error`；`Interrupted` 是迁移事件。
迁移表见 [docs/pipeline.md](docs/pipeline.md)。`Speaking` 期间 AEC/VAD/KWS 继续跑；
barge-in = 停播 + 中止 LLM + 丢弃未播 + `aura_flow_cancel_turn`。
`agent_core` 在 `AURA_EVENT_BARGE_IN` 时调用 `aura_agent_stages_on_barge_in()`，
由 session runtime 定位当前 turn；LLM/TTS 的实际中止在 Phase C。

### 模型与内存

- 模型/下载/MNN 策略在 soundproof；Aura 的 `model_dir` 只作为路径前缀。
- KWS/VAD/SR 常驻；ASR/TTS 按需；LLM 视预算常驻或预加载。
- 禁止运行时高频 malloc/free；队列/帧池/消息在 init 期一次性分配。
- 板端预算：[docs/memory_budget.md](docs/memory_budget.md)。

## 测试

- `tests/unit/`：核心 + flow + agent stage + playback sink（零依赖 C11 框架）。
- `tests/host_sim/`：mock 节点驱动 pipeline + event_bus + 状态机（音频面）；
  **无模型依赖**。
- `tests/flow_chain/`：真实 event_bus → flow_bridge → stage 的文本面闭环
  （dialogue/tool/orchestrator/memory + trace + barge-in 取消）。
- `tests/playback_chain/`：`tts.say → TTS → playback` 播放链闭环 + barge-in
  停播（mock TTS，无模型依赖）。
- `tests/soundproof_chain/aura_test_ae_vad`：真实 soundproof VAD（WebRTC，无模型）
  驱动 `ns, tee(vad)`。
- soundproof 自带 `test_ae_*` 在 soundproof 仓库跑；Aura 的 CTest 也会列出它们
  （FetchContent 的子项目测试），但维护责任在 soundproof。
- DSP-only 与 MNN 全量构建均通过；`ctest -L aura` 15/15。

## 开发纪律

- 节点 `process()` 禁止长阻塞；ASR/TTS/LLM 走 worker + 结果队列 + `AURA_EVENT_NN_RESULT`。
  **唯一例外**：音频源节点（`produces_audio && !consumes_audio`，如 TTS）允许在
  自有 worker 中 `emit_audio`；此时 pts 由节点自填（见 node.h）。
- 节点不得 exit；错误上抛 event_bus。
- 所有音频帧带 pts；AEC 参考是独立时钟源，pipeline 按 pts 配对。
- 适配器要求 10ms 帧；DR（4ms）暂不接。
- 修改配置链/注册名时同步更新 docs/soundproof_integration.md。
- 每次改动先给计划，再改代码；同一问题三次未解决立即上报。
