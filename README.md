# Aura

Voice Agent runtime for Linux SBC (Raspberry Pi).

```
mic → soundproof AFE/VAD/KWS/SR → ASR → Agent(flow/dialogue/tools/memory) → TTS → playback
```

Aura 只负责 **Voice Agent 运行时**：音频 I/O、pipeline、事件总线、状态机、
Agent 数据流中间件、对话编排、工具、记忆、打断。**算法、模型、MNN 全部在
[soundproof](https://github.com/Ryuk17/soundproof)**，通过纯 C ABI 接入。

**当前状态（2026-11-11）**：架构解耦完成 —— Aura 内算法代码/模型/工具已删除，
soundproof 通过 FetchContent 接入，DSP/VAD/KWS/SR/ASR 适配层与 Agent 数据流
中间件骨架落地；DSP-only 与 MNN 全量构建均通过，`ctest -L aura` 10/10。

## 架构

三个平面：

- **音频实时面**：`core/pipeline`，PCM/pts/帧池，允许丢帧；
- **Agent 数据面**：`src/agent/flow`，topic 路由、背压、中间件、取消、追踪；
- **控制/观察面**：`core/event_bus` + 状态机。

详见 [docs/architecture.md](docs/architecture.md)、[docs/agent_dataflow.md](docs/agent_dataflow.md)。

## 构建

第三方依赖（soundproof）由 CMake FetchContent 拉取；模型权重在 soundproof 的
`models/` 下，由它自己的 `download.sh` 获取。

**Windows（MinGW-w64，当前主开发平台）**

```bash
cmake -S . -B build -G "MinGW Makefiles" \
  -DCMAKE_TOOLCHAIN_FILE=toolchains/windows-x86_64.cmake
cmake --build build -j
```

**Linux**

```bash
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=toolchains/linux-x86_64.cmake
cmake --build build -j
```

**离线 / 本地 soundproof 源码**

```bash
cmake ... -DFETCHCONTENT_SOURCE_DIR_SOUNDPROOF=F:/share/soundproof
```

开关：

- `AURA_WITH_SOUNDPROOF_MNN`（默认 ON）：KWS/ASR/SR/TTS/TURN/silero VAD；
  关闭后只构建 DSP（AEC/NS/AGC/BF/SRC/WebRTC VAD），构建最快。
- `AURA_WITH_SOUNDPROOF_LLM`（默认 OFF）：额外拉 MNN-LLM + OpenCV。
- `AURA_BUILD_KWS/VOICEPRINT/ASR/LLM/TTS/BARGE_IN/NET`：是否编入对应适配节点。
- `AURA_SOUNDPROOF_TESTS`（默认 OFF）：是否构建/运行 soundproof 自带的 `test_ae_*`。

## 运行与测试

```bash
ctest --test-dir build -L aura --output-on-failure   # 只跑 Aura 自己的测试

./build/tests/host_sim                          # mock 全链路：对话闭环 + 打断
./build/tests/host_sim --voiceprint-fail        # 声纹拒绝退回 Idle
./build/tests/host_sim --inject-error 500       # 注入错误 → Error → 自动复位
./build/tests/aura_test_ae_vad # 真实 soundproof VAD 适配链
```

`host_sim` 里的 KWS/声纹/ASR/LLM/TTS 仍是 mock 节点，只验证骨架与状态机；
真实 soundproof 适配层由 `aura_test_ae_vad` 覆盖，算法本身的正确性在
soundproof 仓库的 `test_ae_*` 里。

## 文档

| 文档 | 内容 |
|---|---|
| [docs/todo.md](docs/todo.md) | ★ 总计划、Phase A–D、验收指标 |
| [docs/architecture.md](docs/architecture.md) | 三平面、目录、分层、构建 |
| [docs/pipeline.md](docs/pipeline.md) | 节点契约、事件总线、状态机、barge-in |
| [docs/soundproof_integration.md](docs/soundproof_integration.md) | 算法注册名/参数/模型路径/限制 |
| [docs/agent_dataflow.md](docs/agent_dataflow.md) | Agent 数据流中间件 |
| [docs/memory_budget.md](docs/memory_budget.md) | 板端内存/算力预算 |
| [docs/archive/model_requirement.md](docs/archive/model_requirement.md) | 历史模型规格（已移交 soundproof） |

> ⚠️ `docs/` 不入版本库（`.gitignore`），clone 后看不到这些文档。
