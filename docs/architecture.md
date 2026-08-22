# Aura Voice Agent 架构设计

> 状态：**已定案 v0.2**（基于决策点 review 结果更新）
> 目标平台：Raspberry Pi（aarch64）/ 开发机（x86_64-windows）
> 推理后端：**MNN**（统一）
> 备注：树莓派算力有限，当前阶段以**评估 + 原型验证**为主，后续可能加算力卡（架构预留扩展点，不为此特殊设计）

---

## 1. 概述

Aura 是一个跑在树莓派上的端侧 voice agent：常驻监听唤醒词，用户说话 → 语音识别 → LLM 理解并回复 → 语音合成播放，支持工具调用、本地知识库与多轮对话记忆。全链路数据不出设备。

### 1.1 核心约束

| 约束 | 指标 | 说明 |
|---|---|---|
| 内存预算 | ≤ 2 GB（RPi4 4GB 版） | 模型 + 运行时 + 系统余量 |
| 端到端延迟 | 唤醒后首字回复 ≤ 1.5 s | 见 §7 延迟预算 |
| 常开功耗 | 唤醒词/ VAD 阶段 CPU 占用低 | 仅两级模型常驻 |
| 模型格式 | 统一 `.mnn` | **直接用官方 MNN 模型，不做模型转换** |
| 离线 | 100% | 不依赖任何云端服务 |

### 1.2 设计原则

1. **LLM 中心化**：LLM 是对话的唯一"大脑"。意图理解、对话策略、记忆、工具选择统一由 LLM 的 function calling + prompt 编排完成。**不设 NLU 模块**（决策 D4）。
2. **MNN 统一推理**：所有神经网络推理（VAD/ASR/TTS/LLM/Embedding）统一走 MNN，模型直接使用官方/社区发布的 `.mnn` 格式，不维护转换工具链。
3. **模块间只经事件总线通信**：模块无直接依赖，通过 `core/` 定义的消息类型解耦，便于单模块替换与测试。
4. **流式贯穿**：ASR 流式增量、LLM 流式输出、TTS 流式播放，全链路无"整句等待"。
5. **打断优先（Barge-in）**：TTS 播放期间用户说话必须立即打断，是全链路延迟的最高优先级约束。
6. **双平台开发**：开发期在 x86_64-windows 跑通全链路，交叉编译部署到 RPi。

---

## 2. 总体架构

```
                    ┌─────────────────────────────────────────────────┐
                    │                     core/                       │
                    │   Orchestrator（状态机）   EventBus（事件总线）   │
                    │   ModelManager（模型生命周期/内存预算）          │
                    └──────┬──────────────┬──────────────┬────────────┘
                           │              │              │
        ┌──────────────────▼───┐   ┌──────▼──────┐   ┌───▼────────────┐
        │  audio/ 音频 I/O      │   │  memory/     │   │  knowledge/    │
        │  采集/播放/AECM/焦点  │   │  md 记忆      │   │  md 文档 RAG   │
        └──────▲───────────────┘   └─────────────┘   └────────────────┘
               │
┌──────────────┴──────────────────────────────────────────────────┐
│                    事件流（语音 → 文本 → 回复 → 语音）            │
│                                                                    │
│  Mic ──► audio ──► vad/ ──► asr/ ──► llm/ ──► tts/ ──► audio ──► Spk
│        采集+AEC   turn检测   流式识别    核心循环   流式合成     播放
│                                                                    │
│  Barge-in：Spk 播放中 Mic 检出语音 ──► Interrupt ──► 停 TTS、回 LISTENING
└──────────────────────────────────────────────────────────────────┘
        │                              │              │
        │                              │              │
   platform/ HAL（ALSA/WASAPI/时钟/线程亲和）  utils/（日志/配置/wav）
```

依赖方向：`core/` 为顶层编排；各引擎模块（audio/asr/vad/llm/tts）彼此**不互相依赖**，只依赖 `core/` 的数据类型与事件总线、`platform/` 的 HAL、`utils/`。`dialogue/`、`memory/`、`knowledge/`、`tools/` 作为策略与上下文供给层，只被 Orchestrator 消费。

---

## 3. 会话状态机（Orchestrator）

```
                  ┌──────────┐
        ─────────►│  IDLE    │ 常驻：仅 VAD/轮次检测运行
                  └────┬─────┘
        检出语音起始    │
        ┌──────────────▼───────────────┐
        │  LISTENING   采集 + ASR 流式  │◄────────────┐
        └──────┬───────────────────────┘             │
        语句边界 │（VAD 静音 / 轮次检测判定）         │
        ┌──────▼───────────────┐                     │
        │  PROCESSING           │  ASR 整句 → LLM    │
        │  → LLM 流式生成       │  → 回复文本        │
        └──────┬───────────────┘                     │
        ┌──────▼───────────────┐                     │
        │  SPEAKING  TTS 流式播放│───播放完成─────────┤
        └──────┬───────────────┘                     │
               │ Barge-in：播放中检出用户语音         │
               └──────► 立即停止 TTS ──► 回到 LISTENING
```

| 状态 | 常驻模型 | 行为 |
|---|---|---|
| IDLE | VAD / 轮次检测 | 低功耗监听，不加载 ASR/LLM/TTS |
| LISTENING | + ASR | 采集开始，VAD 或轮次检测判定语句边界 |
| PROCESSING | + LLM | 整句 ASR 完成后送入 LLM，流式生成 |
| SPEAKING | + TTS | 流式播放；检出用户语音即打断 |

---

## 4. 事件定义（EventBus 消息）

全部消息为值类型（value type），跨线程传递，`core/` 中定义。

```cpp
// core/events.h（草案）
enum class EventType {
  AudioChunk,        // 麦克风 PCM 帧（AECM 处理后，16 kHz/16bit/mono）
  WakeWordDetected,  // 唤醒词命中（v0 占位，接口保留但不触发）
  SpeechStart,       // 检出语音起始（VAD 或轮次检测）
  SpeechEnd,         // 检出语句结束（附整段音频的引用）
  AsrPartial,        // ASR 增量文本
  AsrFinal,          // ASR 整句定稿（含置信度）
  LlmToken,          // LLM 流式 token 增量（文本或工具调用片段）
  LlmToolCall,       // LLM 请求调用工具（结构化）
  LlmToolResult,     // 工具执行结果返回 LLM
  LlmComplete,       // 回复生成完毕
  TtsChunk,          // TTS PCM 块（交由播放）
  TtsComplete,       // 播放完成
  BargeIn,           // 打断请求（优先级最高，总线立即调度）
  Error,             // 模块错误（带来源与错误码）
};
```

规则：

- **优先级**：BargeIn > 音频相关 > 文本/事件 > 工具结果。总线按优先级出队，BargeIn 可打断正在处理的 LLM 事件。
- **背压**：各模块按自己的节奏消费；`AudioChunk` 丢弃策略由消费端决定（VAD 不丢、ASR 可丢尾部）。
- **线程安全**：所有事件不可变（const），跨线程传递所有权。

---

## 5. 模块设计

### 5.1 audio/ — 音频 I/O 层

- `IAudioSource`：麦克风采集，ALSA（RPi）/ WASAPI（Windows），回调式输出 16 kHz / 16 bit / mono（全链路统一采样率，与 AECM 帧长 80/160 样本对齐）。
- `IAudioSink`：播放，TTS PCM 块入队即播，支持 `Stop()`（barge-in 用）。
- **回声消除（AECM）**（决策 D5）：复用 TrickRoom 仓库的 WebRTC AECM 移植（`src/audio_engine/audio_processing/acoustic_echo_cancellation_mobile/`，C API `WebRtcAecm_Create/Init/BufferFarend/Process`，已有 CMake 集成产 `libAE_AECM`）。TTS 播放的 PCM 同时送入 `BufferFarend`，麦克风采集经 `Process` 消回声后输出。AECM 16 kHz 帧处理与全链路采样率天然对齐。
- `AudioFocus`：管理播放与采集的互斥（barge-in 时 sink 立即停、source 继续采）。

### 5.2 vad/ — 语音活动检测与轮次检测

- 常驻运行，CPU 占用最小，播放阶段继续运行以支持 barge-in。
- **两种模式可配置切换**（决策 D4-其他2）：
  - `TURN_DETECTOR_VAD`（默认）：sherpa-mnn 内置 silero-vad 模型（MNN 格式，随 sherpa-mnn 框架提供）。
  - `TURN_DETECTOR_SMART`：pipecat-ai 的 smart-turn-v3 轮次检测模型（对话级判断"用户是否说完/是否期望回复"）。
- ⚠️ **待验证项**：smart-turn-v3 官方发布为 ONNX 格式，若需 MNN 运行需要转换或验证 MNN 兼容性——与"不做模型转换"原则冲突，M2 阶段验证，验证不过则 VAD 模式为唯一实现。
- 输出：`SpeechStart`、`SpeechEnd`。
- **唤醒词（占位，决策 D10）**：预留 `IWakeWord` 接口与 `WakeWordDetected` 事件（v0 返回未命中，不加载任何模型），v0 为"任意语音起始即唤醒"模式，词唤醒实现延后。

### 5.3 asr/ — 流式语音识别（sherpa-mnn 封装）

- 基于 MNN 官方 `apps/frameworks/sherpa-mnn` 框架，封装为 `IASR`：`FeedAudio(const AudioFrame&)` → 回调 `AsrPartial` / `AsrFinal`。
- 模型：**sherpa-mnn-streaming-zipformer-bilingual-zh-en-2023-02-20**（taobao-mnn 发布，`sherpa-mnn-streaming-zipformer-bilingual-zh-en-2023-02-20`，流式中英双语）。
- 输入为 AECM 处理后的音频，不做二次降噪。

### 5.4 llm/ — LLM 推理（MNN-LLM）

- 模型：**Qwen3.5-0.8B-MNN**（HuggingFace `taobao-mnn/Qwen3.5-0.8B-MNN`，官方 MNN 格式，**不量化，直接使用**，决策 D8）。
- 推理：基于 MNN-LLM（`MNN/apps/llm` 或 MNN-LLM 分支），`ILLM`：`Chat(messages, callbacks)`，流式回调 token；支持 function calling（Qwen3.5 原生工具格式）。
- 参数（默认，可配置）：KV cache 容量按 8-16 轮对话配置、温度 0.7。
- **关键路径**：单 LLM 线程，串行处理；BargeIn 时 `Abort()` 立即终止生成，释放线程。
- 上下文窗口管理：对话历史由 `memory/` 压缩后注入，超长时用摘要替换旧轮次。

### 5.5 dialogue/ — 对话策略层

- **不持有独立状态机**，职责为策略层：系统提示词模板、上下文组装顺序（长期记忆摘要 → 知识库 → 工具描述 → 对话历史 → 当前输入）、回复风格约束。
- 暴露 `DialoguePolicy` 接口，可插拔（默认 LLM 直通策略）。

### 5.6 tts/ — 语音合成（mnn-tts 封装）

- 基于 [wangzhaode/mnn-tts](https://github.com/wangzhaode/mnn-tts)（MNN 系 TTS），封装为 `ITTS`：`Synthesize(text)` → 回调 `TtsChunk`（16 kHz PCM）；支持 `Abort()`（barge-in）。
- 约束：首字延迟 < 300 ms、流式合成（逐句/逐 chunk 生成，不等整段）、内存 < 150 MB。
- 语速/音量由 AudioFocus 统一控制。

### 5.7 tools/ — 工具调用（Function Calling）

- 工具注册表：`ToolRegistry`，工具以 JSON Schema 声明（描述 + 参数），注入 LLM 系统提示。
- 执行器：`ToolExecutor`，同步执行（含超时与失败回退文案），结果经 `LlmToolResult` 回送 LLM 续聊。
- 内置工具 v1：音量/亮度控制、计时器、天气查询（本地缓存或用户自配源）、系统信息。扩展点清晰，后续加 IoT 控制。

### 5.8 knowledge/ — 本地知识库（md 文档 RAG）

- **v1（决策 D7）**：知识库以 md 文档形式存放（`resources/knowledge/*.md`），加载后按标题/段落切块，检索用**关键词匹配**（倒排/BM25 简化实现）。
- **v2（决策 D9 延伸）**：引入 **Qwen3-Embedding-0.6B-MNN** 做向量化检索（模型已定：`taobao-mnn/Qwen3-Embedding-0.6B-MNN`）。Embedding 模型与 LLM 共享 MNN 运行时，仅在 RAG 命中查询时加载（见 §6.3）。
- 检索结果注入上下文，格式与位置由 `dialogue/` 策略决定。

### 5.9 memory/ — 对话记忆

- **v1（决策 D9）**：持久化为 md 文档（`resources/memory/<session>.md`）：短期 = 最近 N 轮原始对话；长期 = LLM 生成的会话摘要与关键事实（"我叫小明"），会话结束/定期追加写入。
- **v2**：改用 embedding 抽取关键事实（复用 Qwen3-Embedding-0.6B-MNN），结构化存储。
- 注入优先级：长期记忆摘要 < 知识库结果 < 短期对话轮次 < 系统指令。

### 5.10 core/ — 编排引擎

- `Orchestrator`：状态机（§3）+ 模块装配 + 延迟监控（记录每段耗时，超预算打日志）。
- `EventBus`：§4 定义的事件分发，**自研轻量实现（决策 D6），不依赖 boost 或任何第三方异步框架**——单事件分发线程 + 优先级队列，总量约 300-500 行。
- `ModelManager`：模型生命周期管理（§6.3），统一加载/卸载/引用计数，监控内存占用，超预算时按 LRU 卸载非关键模型。

### 5.11 platform/ — HAL

- 音频设备抽象（ALSA / WASAPI）、时间源（`now()` 统一时钟）、线程亲和与优先级（采集/播放线程提权）、电源与散热接口（预留）。
- 通过 CMake 按目标平台选实现。

### 5.12 utils/ — 通用工具

- 日志（分级 + 环形缓冲）、配置加载（JSON/TOML）、md/wav 工具（测试与 golden 样本用）、字符串/JSON 工具。

---

## 6. 模型策略（MNN）

### 6.1 模型清单（全部已定案，官方 `.mnn` 格式，不做转换）

| 用途 | 模型 | 来源 | 说明 |
|---|---|---|---|
| LLM | Qwen3.5-0.8B-MNN | `taobao-mnn/Qwen3.5-0.8B-MNN` | 不量化，直接使用 |
| ASR | sherpa-mnn-streaming-zipformer-bilingual-zh-en-2023-02-20 | `taobao-mnn/sherpa-mnn-streaming-zipformer-bilingual-zh-en-2023-02-20` | 流式中英双语 |
| VAD | silero-vad（sherpa-mnn 内置） | MNN `apps/frameworks/sherpa-mnn` | 随框架提供 |
| TTS | mnn-tts 配套模型 | [wangzhaode/mnn-tts](https://github.com/wangzhaode/mnn-tts) | 按仓库说明获取 |
| Embedding（v2） | Qwen3-Embedding-0.6B-MNN | `taobao-mnn/Qwen3-Embedding-0.6B-MNN` | RAG/记忆抽取用 |
| 轮次检测（可选） | smart-turn-v3 | 模型由项目方后续提供（ONNX）⚠️需验证 MNN 兼容性或转换 | 与 VAD 模式互斥配置 |

### 6.2 模型来源

- 所有模型为 MNN 官方/社区发布的现成 `.mnn` 文件，**不维护任何模型转换工具链**（决策 D4-其他3）。
- `models/` 目录存放模型 + 来源说明（README + SHA 校验），运行时经配置引用。

### 6.3 内存预算与加载策略（ModelManager）

| 模型 | 估算占用 | 常驻 | 加载时机 |
|---|---|---|---|
| VAD/轮次检测 | ~20 MB | ✅ 常驻 | 启动 |
| ASR（zipformer） | ~150 MB | 唤醒后 | 事件触发，一次加载复用 |
| LLM（Qwen3.5-0.8B） | ~1 GB | 唤醒后 | 事件触发 |
| TTS（mnn-tts） | ~120 MB | 唤醒后 | 需要回复时 |
| Embedding（0.6B，v2） | ~600 MB | ❌ 不常驻 | RAG 查询时临时加载，用完即卸 |

- 总峰值预算 ≤ 2 GB（Embedding 与 LLM 不同时驻留）。
- 加载顺序：语音起始 → ASR（立即）→ LLM（并行后台）→ TTS（需要回复时）。
- 卸载策略：回到 IDLE 超过 N 秒（默认 120 s，可配）后按 LLM → ASR → TTS 顺序卸载，回落到常驻态。

---

## 7. 延迟预算

| 阶段 | 预算 | 测量点 |
|---|---|---|
| VAD/轮次检测语音起始 | ≤ 100 ms | SpeechStart 相对语音起点 |
| ASR 首个 partial | ≤ 400 ms | SpeechStart → AsrPartial |
| ASR 整句定稿 | 句长 × 实时率 | 实时率 ≥ 1× |
| LLM 首 token | ≤ 800 ms | AsrFinal → LlmToken |
| TTS 首字播放 | ≤ 300 ms | LlmComplete → 扬声器出声 |
| **端到端（语音起始场景）** | **≤ 1.5 s** | 语音起始 → 首字出声 |
| Barge-in 响应 | ≤ 100 ms | 播放中人声 → TTS 停止 |

预算由 Orchestrator 的延迟监控逐项统计，超预算输出 WARN 日志与事件，作为后续优化的依据。注：当前为 RPi 算力评估值，真机验证后修正；若后续加算力卡（NVIDIA Jetson 类），MNN 换 CUDA 后端即可，架构不变。

---

## 8. 关键流程时序

### 8.1 正常对话（语音起始场景）

```
Mic → audio(+AECM) ─► vad: SpeechStart ─► Orchestrator: IDLE→LISTENING
Mic → asr: FeedAudio（持续）…… vad: SpeechEnd ─► asr: AsrFinal
asr ─► llm: Chat(系统提示 + 记忆 + 知识 + 历史 + 用户句)
llm: LlmToken × N ─► Orchestrator 收集（遇 LlmToolCall ─► tools 执行 ─► LlmToolResult ─► llm 续聊）
llm: LlmComplete ─► tts: Synthesize(回复文本)
tts: TtsChunk ─► audio sink 播放（PCM 同时送 AECM farend）─► TtsComplete ─► Orchestrator: SPEAKING→LISTENING
```

### 8.2 Barge-in 打断

```
SPEAKING 中：Mic → vad: SpeechStart ─► Orchestrator
1) 发 BargeIn 事件（最高优先级）
2) tts.Abort() + audio sink Stop()（<100ms 静音）
3) 状态 → LISTENING，继续新一轮
```

### 8.3 工具调用（Function Calling）

```
llm 输出 tool_call(turn_off_light, {}) ─► LlmToolCall
Orchestrator ─► tools: 查表执行（超时 3s）─► LlmToolResult("ok")
─► llm: 继续生成 "已关灯" ─► tts
```

---

## 9. 线程模型

| 线程 | 职责 | 优先级 | 说明 |
|---|---|---|---|
| 采集线程 | 麦克风 → AECM → AudioChunk | 高（RT） | 不阻塞，不做重活 |
| VAD/轮次检测线程 | 常驻检测，发语音边界事件 | 高 | 播放阶段继续运行（barge-in） |
| ASR 线程 | FeedAudio 队列消费，流式识别 | 中 | 队列满丢弃尾部 |
| LLM 线程 | 单线程推理 + 流式输出 | 中 | 唯一阻塞点，Abort 可中断 |
| TTS 线程 | 合成 → PCM → sink | 中 | 播放与合成分离（合成快于播放） |
| 总线线程 | 事件分发调度 | 中 | 按优先级出队 |

死锁规避：模块回调只发事件不等待；跨线程共享状态统一收口在 Orchestrator。

---

## 10. 构建与部署

- CMake 单工程；`toolchains/` 已有 aarch64 / armhf / x86_64-windows 三份工具链文件，直接复用。
- 依赖：**MNN**（必选，交叉编译 aarch64 需开启 `MNN_BUILD_LLM` 等选项）、**sherpa-mnn**（ASR/VAD）、**mnn-tts**（TTS）、**AECM**（复用 TrickRoom `libAE_AECM`，以 submodule 或源码引入）、驱动库按平台（ALSA/WASAPI）。
- 目录约定：`third_party/` 内嵌依赖源码；`scripts/` 固化交叉编译、打包、刷机（SD 卡镜像）脚本。
- 产物：`build/x86_64-windows/`（开发）与 `build/aarch64-linux/`（RPi），互不影响。
- 资源路径：模型/配置/md 知识库从 `models/` 与 `resources/` 统一打包，运行时经 `utils/Config` 解析相对路径（可移植）。

---

## 11. 测试策略

- **单元测试**：模块级，主机 x86_64 运行；接口 Mock（如 `IFakeTts` 代替真实模型）保证不依赖模型即可跑。
- **Golden audio 回归**：`tests/golden/` 存放录制的音频样本 + 期望结果（VAD 边界、ASR 文本、打断场景），脚本跑全量回归，比对延迟与文本（容差可配）。
- **集成测试**：全链路 mock（LLM 用固定输出脚本化），验证状态机迁移与事件时序。
- **真机验证**：RPi 上人工对话冒烟脚本（`scripts/smoke.sh`），记录实际延迟对照 §7 预算。
- **第三方模型验证**：每个模型首次接入时验证 MNN 运行正确性（输出与官方 demo 对比）。

---

## 12. 决策记录（已定案）

| 编号 | 决策点 | 结论 |
|---|---|---|
| D1 | ASR 模型 | ✅ sherpa-mnn 框架 + streaming-zipformer-bilingual-zh-en-2023-02-20 |
| D2 | TTS | ✅ wangzhaode/mnn-tts |
| D3 | VAD | ✅ sherpa-mnn 内置 silero-vad；唤醒词 v0 不做，语音起始即唤醒 |
| D4 | NLU | ✅ **不做 NLU 模块**，LLM 直通 |
| D5 | 回声消除 | ✅ 复用 TrickRoom WebRTC AECM 移植（`libAE_AECM`，C API） |
| D6 | 事件总线 | ✅ 自研轻量（单分发线程 + 优先级队列），**不依赖 boost/第三方异步框架** |
| D7 | 知识库 | ✅ v1 用 md 文档 + 关键词检索；v2 上 Embedding 向量检索 |
| D8 | LLM 量化 | ✅ 不量化，直接使用官方 Qwen3.5-0.8B-MNN |
| D9 | 记忆持久化 | ✅ v1 md 文档；v2 embedding 抽取（Qwen3-Embedding-0.6B-MNN） |
| 其他1 | 轮次检测 | 可配置 `TURN_DETECTOR_VAD` / `TURN_DETECTOR_SMART`；smart-turn-v3 模型由项目方后续提供，接入时验证 MNN 兼容性 |
| 其他2 | 模型转换 | ✅ 不做，直接用 MNN 模型 |
| 其他3 | 算力演进 | RPi 评估阶段，后续加算力卡，MNN 换后端即可，架构不变 |
| D10 | 唤醒词 | ✅ 占位：`IWakeWord` 接口 + 事件保留，v0 不加载模型，语音起始即唤醒；词唤醒延后 |

---

## 13. 里程碑建议

1. **M1 骨架**：CMake 工程 + 事件总线 + 状态机 + 音频采集播放（WASAPI/ALSA）+ AECM 接入 + 日志配置，双平台可编译运行
2. **M2 语音链路**：sherpa-mnn（VAD + 流式 ASR）→ 文本进文本出（控制台显示）；验证 smart-turn-v3 MNN 兼容性
3. **M3 对话链路**：MNN-LLM（Qwen3.5-0.8B-MNN）+ dialogue + memory(md) → 文本回复
4. **M4 语音闭环**：mnn-tts + barge-in → 全语音对话
5. **M5 增强**：tools、knowledge(md RAG)、golden 回归、RPi 真机部署脚本
