# Aura 实现顺序计划

> 依据：[docs/architecture.md](architecture.md)（v0.3 定案）、[docs/audio_engine_integration.md](audio_engine_integration.md)（audio_engine 集成方案，已确认）
> 原则：每步**可编译、可运行、有验收**；开发机 x86_64-linux（`toolchains/linux-x86_64.cmake`）先行，aarch64/RPi 交叉编译每阶段末验证一次

---

## 总览

```
M1 骨架 ──► M2 语音链路 ──► M3 对话链路 ──► M4 语音闭环 ──► M5 增强
  │             │               │
  │             │               └──► 可与 M2 并行：llm/ 只依赖 core/（事件总线）
  │             │
  │             └──► 与 M3 无依赖：audio+vad+asr 独立成链
  │
  └──► M5 的 tools/ 可与 M4 并行：tools 只依赖 llm/ 的 function calling
```

**并行建议**：M2 与 M3 可同时开发（两条链在"文本"处汇合）；M4 与 M5-tools 可同时开发。单人开发建议按顺序推进，并行项用于等模型/等硬件时的空档。

---

## M1 骨架 — 可编译的空中楼阁

> 目标：多平台编译通过，事件总线 + 状态机 + 音频 I/O + audio_engine 算法链全部连通，无模型可运行

| # | 内容 | 产出 | 验收 |
|---|---|---|---|
| 1.1 | 根 CMakeLists + 目录结构 + 三方依赖管理（`third_party/` 全部 submodule，见 audio_engine_integration.md）+ 复用 `toolchains/` 四份工具链 | 空工程多平台编译通过 | `cmake -B build/linux-x86_64.cmake` 与 `-B build/aarch64-linux` 均成功 |
| 1.2 | `utils/`：日志（分级+环形缓冲）、配置加载（JSON）、wav 工具 | 可用的基础设施 | 日志单测、配置单测、wav 读写单测 |
| 1.3 | `core/`：`events.h`（§4 全部事件）+ EventBus（优先级队列 + 单分发线程） | 事件总线 | 单测：优先级排序、BargeIn 抢占、背压丢弃、多生产者单消费者 |
| 1.4 | `core/`：Orchestrator 状态机骨架（IDLE/LISTENING/PROCESSING/SPEAKING）+ 模块接口占位（IASR/ILLM/ITTS/IWakeWord 等） | 状态机可测 | 单测：全状态迁移时序、BargeIn 打断路径 |
| 1.5 | `platform/`：HAL 接口 + WASAPI（Win）与 ALSA（Linux/aarch64）实现 | 采集/播放可用 | 开发机（linux-x86_64）采集播放回环测试（录→播）；ALSA 在 RPi 冒烟 |
| 1.6 | **audio_engine 全量引入**（决策 D11/D12）：引入trickroom.cmake编译算法库 | 算法库就绪 | 12 个 `libAE_*` 全编译|
| 1.7 | `audio/`：重建为 **AudioChain 算法链**（配置驱动节点链）+ AECM 节点（使用`libAE_AECM` 统一 C API + 16kHz 封装）+ AudioFocus | 音频管线框架 | 单测：farend+近端合成信号 → 回声残余低于阈值（参照 WebRTC 测试向量思路）；链节点可配置挂载 |
| 1.8 | M1 集成 demo：采集 → 算法链 → 事件总线 → 播放回环 | demo 可跑 | 开发机上听到回声消除后的回放，日志与延迟监控输出正常 |

**M1 完成标志**：无模型条件下全链路数据流（音频→事件→状态机）跑通，`git log` 有清晰的里程碑 commit。

---

## M2 语音链路 — 语音进，文本出

> 目标：对着麦克风说话，控制台实时打印识别文本（流式）
> 依赖：M1.7（算法链/AECM）、M1.3（总线）

| # | 内容 | 产出 | 验收 |
|---|---|---|---|
| 2.1 | 引入 MNN + sherpa-mnn（third_party，aarch64 交叉编译验证） | 依赖就绪 | 双平台编译；官方 demo（asr-recorder）跑通 |
| 2.2 | `vad/`：silero VAD 封装（`TURN_DETECTOR_VAD` 模式），SpeechStart/SpeechEnd 判定，播放阶段持续运行（为 barge-in 铺垫）；libAE_VAD（WebRTC VAD）作为零模型开销的预检/降级备选接入 | VAD 模块 | 单测：golden 音频样本边界检出正确（正/负样本）；说话中断静音 600ms 触发 SpeechEnd；两种 VAD 可配置切换 |
| 2.3 | `asr/`：sherpa-mnn zipformer 封装（`IASR::FeedAudio` + Partial/Final 回调） | ASR 模块 | 单测：golden 音频识别文本比对（容差）；实时率 > 1× |
| 2.4 | 集成 demo：采集 → 算法链（AECM）→ VAD → ASR → 控制台流式文本 | 语音进文本出 | 开发机上实时对话识别，端点延迟对照 §7 预算（SpeechStart→首个 partial ≤400ms） |
| 2.5 | ⚠️ smart-turn-v3 验证（**等用户提供模型后做**）：MNN 兼容性验证，`TURN_DETECTOR_SMART` 模式实现 + 配置切换 | 可配置轮次检测 | 两种模式可切换；smart 模式在测试语料上端点判定合理 |
| 2.6 | M2 回归：golden audio 目录建立（唤醒/端点/识别样本 + 期望文本） | 回归基线 | 脚本一键跑全量回归 |
| 2.7 | 算法链增强节点接入（决策 D11）：SRC（HAL 采样率兜底）+ NS（ASR 前降噪）+ AGC2（音量归一化）入链，配置驱动挂载 | 增强链 | 各节点单测（复用 TrickRoom golden wav）+ 对照实验：同语料开关 NS 的识别文本/实时率对比（记录增益） |

**M2 完成标志**：对着麦克风连续说话，控制台实时出字，端点正确。

---

## M3 对话链路 — 文本进，文本出

> 目标：控制台输入文本，LLM 流式回复，带记忆
> 依赖：M1.3（总线）；与 M2 并行开发

| # | 内容 | 产出 | 验收 |
|---|---|---|---|
| 3.1 | 引入 MNN-LLM（Qwen3.5-0.8B-MNN 下载脚本 + third_party 接入，aarch64 验证） | 依赖就绪 | 开发机上官方 llm demo 跑通，首 token 延迟记录 |
| 3.2 | `llm/`：`ILLM::Chat` 封装（流式 token 回调、`Abort()`、function calling 透传） | LLM 模块 | 单测：mock 会话（短输入输出）、Abort 后立即返回；流式回调顺序正确 |
| 3.3 | `dialogue/`：DialoguePolicy（系统提示模板 + 上下文组装：记忆→历史→输入） | 策略层 | 单测：组装顺序正确、token 预算截断逻辑 |
| 3.4 | `memory/`：v1 md 记忆（短期轮次 + 长期摘要 md 文件读写） | 记忆模块 | 单测：会话摘要写入/加载；重启后记忆可恢复 |
| 3.5 | 集成 demo：控制台对话（输入→LLM→回复，多轮 + 记忆） | 文本对话闭环 | 连续 10 轮对话正常，重启后记得前轮关键事实（摘要级） |

**M3 完成标志**：控制台连续多轮对话，记忆跨会话存活。

---

## M4 语音闭环 — 全语音对话

> 目标：说话 → 听见回复，可打断
> 依赖：M2（语音链路）、M3（对话链路）

| # | 内容 | 产出 | 验收 |
|---|---|---|---|
| 4.1 | 引入 mnn-tts（third_party，模型按仓库说明获取） | 依赖就绪 | 官方 demo 跑通，TTS 首字 < 300ms |
| 4.2 | `tts/`：`ITTS::Synthesize` 封装（流式 PCM chunk、`Abort()`） | TTS 模块 | 单测：短文本合成完整、Abort 立即停 |
| 4.3 | `audio/`：AudioFocus（播放/采集互斥）+ **barge-in 联动**（SPEAKING 中 VAD 检出 → BargeIn → tts.Abort + sink.Stop） | 打断闭环 | 单测 + 手动：播放中说话，≤100ms 静音切换 |
| 4.4 | 全语音闭环集成（Orchestrator 接通 4 个状态 + 算法链 AECM farend 接 TTS 输出） | 完整 voice agent v0 | 手动验收：唤醒（语音起始）→ 对话 → 打断 → 再对话；延迟记录对照 §7 |
| 4.5 | RPi 真机首测：交叉编译部署 + `scripts/smoke.sh` 冒烟 | RPi 可跑 | RPi 上完成一轮完整对话；记录实际内存/延迟，修正预算表 |

**M4 完成标志**：RPi 上全语音对话 + 打断可用，延迟预算表有实测数据。

---

## M5 增强

> 目标：工具、知识、唤醒词、回归与部署完备

| # | 内容 | 产出 | 验收 |
|---|---|---|---|
| 5.1 | `tools/`：ToolRegistry + ToolExecutor + 内置工具 v1（音量/计时器/系统信息/天气[待定源]） | 工具调用 | 单测：工具声明 JSON 正确、超时回退；对话中触发工具并续聊 |
| 5.2 | `knowledge/`：v1 md 知识库（md 切块 + 关键词检索 + 注入） | md RAG | 单测：命中正确段落并注入对话；未命中不影响对话 |
| 5.3 | Embedding v2：Qwen3-Embedding-0.6B-MNN 接入（临时加载/卸载 + 向量检索 + 记忆抽取） | 向量 RAG + 记忆升级 | 内存峰值不超预算；检索质量对比 v1 关键词 |
| 5.4 | 唤醒词解占位（**等模型定**）：IWakeWord 实现 + 常驻模式切换 | 词唤醒 | 唤醒率/误唤醒率基线测试 |
| 5.5 | golden 回归完善 + `scripts/`：打包、刷机（SD 镜像）、性能采集 | 工程化完备 | 回归一键跑通；RPi 刷机步骤文档化 |
| 5.6 | 储备算法点亮（决策 D11，接口统一按需挂载）：AEC(AEC3)（高算力平台）、HS 啸叫抑制、BF 波束形成（多麦阵列）、DR 去混响、TS/IE | 算法扩展可用 | 各库 golden 验证（复用 audio_engine unitest 数据）；配置示例文档化；RPi 内存/CPU 增量记录对照预算 |

---

## 每步通用验收

1. **编译**：x86_64-linux（`linux-x86_64.cmake`）必过；涉及平台层时 aarch64-linux 交叉编译过
2. **测试**：该模块单测全绿（golden 样本入 `resources/audio_engine/data/` 与 `tests/golden/`）
3. **延迟**：触及音频/模型路径的步骤记录实测耗时，对照 architecture.md §7
4. **commit**：每步一个主题 commit，M 里程碑打 tag

## 待外部输入的前置项（不阻塞主线）

| 依赖 | 阻塞步骤 | 说明 |
|---|---|---|
| smart-turn-v3 模型 | 2.5 | 用户提供模型后接入；之前 VAD 模式即可跑 |
| 唤醒词模型 | 5.4 | 占位接口已留，不阻塞 M1-M4 |
| RPi 真机 | 4.5 | 无真机时 M4 在 Win 验收，预算表标注"待真机修正" |
