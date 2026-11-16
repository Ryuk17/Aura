# Aura

> 面向 Linux SBC 的端侧 Voice Agent 运行时。

[![License: Apache-2.0](https://img.shields.io/badge/License-Apache_2.0-blue.svg)](LICENSE)
[![Language: C11](https://img.shields.io/badge/C-C11-00599C.svg)](include/agent_export.h)
[![Platform: Linux | Windows](https://img.shields.io/badge/Platform-Linux%20%7C%20Windows-lightgrey.svg)](#快速开始)

Aura 把麦克风采集、语音算法链、语音识别、对话生成、语音合成和播放串成一条低延迟的
流式管线，并提供状态机、事件总线、有界队列、内存池、打断等运行时能力。目标平台是
树莓派等 Linux SBC，端侧全离线为主，云端仅作增强或兜底。

```text
mic → soundproof AFE/VAD/KWS/SR → ASR → Agent(dialogue / tools / memory) → TTS → playback
```

**Aura 只做运行时**：音频 I/O、pipeline、事件总线、状态机、Agent 数据流中间件、
对话编排、工具、记忆、打断。算法实现、模型权重、MNN 推理全部在
[soundproof](https://github.com/Ryuk17/soundproof) 仓库，通过纯 C ABI
`AudioEngine_*` 接入；本仓库的 `src/soundproof/` 只是适配层。

## 特性

- **纯 C 对外 API** —— [include/agent_export.h](include/agent_export.h) 是上层唯一
  允许接触的接口面，C / C++ / Rust / Android JNI 均可直接调用，内部拓扑对调用方不可见。
- **可配置算法链** —— 链语法 `aec, bf, ns, agc, tee(vad, kws, sr), asr`，
  参数 `chain_param_<algo>.<key>`；装配期校验算法名、参数键与类型，写错不会静默失效。
- **三平面架构** —— 音频实时面 / Agent 数据面 / 控制观察面互相隔离，分层依赖单向。
- **完整对话状态机** —— `Idle → Listening → Thinking → Speaking → Error`，
  支持 barge-in：停播 + 中止推理 + 丢弃未播音频 + 取消当前 turn。
- **Agent 数据流中间件** —— topic 路由、每 stage 有界队列与背压、中间件链、
  取消令牌、session/turn 上下文、trace 与 metrics。
- **对话闭环** —— session / dialogue / memory / tool_runner / orchestrator 五个
  stage：`asr.final → dialogue.intent → tool.call → tool.result → tts.say`；
  工具支持重试与 call_id 幂等（副作用不重复执行）。
- **全双工播放链** —— `tts → src → playback` 独立子图；TTS 适配同步引擎
  （异步 worker + 10ms 分帧 + 实时 pacing）；`playback` sink 通过可插拔 HAL
  后端写设备并回采给 AEC 参考。
- **端侧内存纪律** —— 队列、帧池、消息在 init 期一次性分配，运行期避免高频 malloc/free。
- **PC 可验证** —— `host_sim` / `flow_chain` / `playback_chain` 在无硬件、
  无模型条件下跑通音频面、文本面与播放链闭环。
- **跨平台构建** —— Windows（MinGW-w64）/ Linux x86_64 开发，aarch64 / armhf 交叉编译。

## 快速开始

### 环境要求

- CMake ≥ 3.20
- C11 + C++17 编译器（GCC / Clang；Windows 下使用 MinGW-w64）
- Git 与网络（首次构建通过 FetchContent 拉取 soundproof），或本地 soundproof 源码
- 模型权重不在本仓库，由 soundproof 的 `models/download.sh` 获取

### 构建

**Windows（MinGW-w64）**

```bash
cmake -S . -B build -G "MinGW Makefiles" \
  -DCMAKE_TOOLCHAIN_FILE=toolchains/windows-x86_64.cmake
cmake --build build -j
```

**Linux x86_64**

```bash
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=toolchains/linux-x86_64.cmake
cmake --build build -j
```

**离线 / 使用本地 soundproof 源码**

```bash
cmake -S . -B build -G "MinGW Makefiles" \
  -DCMAKE_TOOLCHAIN_FILE=toolchains/windows-x86_64.cmake \
  -DFETCHCONTENT_SOURCE_DIR_SOUNDPROOF=/path/to/soundproof
cmake --build build -j
```

**仅构建 DSP（最快，不编译 KWS/ASR/SR 等模型节点）**

```bash
cmake -S . -B build -DAURA_WITH_SOUNDPROOF_MNN=OFF
```

### 运行测试

```bash
# Aura 自己的测试（不包含 soundproof 的 test_ae_*）
ctest --test-dir build -L aura --output-on-failure

# PC 全链路仿真：对话闭环 + 打断
./build/tests/host_sim

# 其他仿真路径
./build/tests/host_sim --voiceprint-fail    # 声纹拒绝，退回 Idle
./build/tests/host_sim --inject-error 500   # 注入错误 → Error → 自动复位
./build/tests/host_sim --wav in.wav --out out.wav --config configs/agent/minimal.conf

# 文本面闭环：真实 event_bus → flow_bridge → stage（含工具循环/取消）
./build/tests/flow_chain

# 播放链闭环：tts.say → TTS → playback + barge-in（mock TTS，无需模型）
./build/tests/playback_chain

# 真实 soundproof VAD 适配链（WebRTC VAD，无需模型）
./build/tests/aura_test_ae_vad
```

### 交叉编译

```bash
# 前置：sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu
./scripts/cross_compile.sh aarch64

# 32 位 ARM
./scripts/cross_compile.sh armhf
```

## 配置与算法链

运行期配置为 `key = value` 文本文件，样例见 [configs/agent/](configs/agent/)：

```ini
# configs/agent/minimal.conf —— 单通道 + WebRTC VAD，无需模型
sample_rate = 16000
frame_ms    = 10
channels    = 1
log_level   = info

chain = ns, tee(vad)
chain_param_vad.vad_mode  = 0
chain_param_vad.threshold = 0.5
```

- **链语法**：`chain = bf, aec, ns, agc, tee(vad, kws, sr), asr`；
  `tee(...)` 表示把同一路音频分发给多个下游节点。
- **算法参数**：`chain_param_<算法名>.<参数> = 值`，类型由算法注册表校验。
- **可选算法名**：`aec, aecm, ns, agc, agc2, bf, src, hs, ts, vad, kws, sr, asr,
  tts, playback`（`llm` 在 Phase C2 接入）。
- **模型路径**：`model_dir` 指向 soundproof 的 `models/` 目录，示例见
  [configs/agent/capture.conf](configs/agent/capture.conf)。

播放链（Phase C）单独配置，示例见
[configs/agent/full_duplex.conf](configs/agent/full_duplex.conf)：

```ini
playback_sample_rate = 24000          # TTS 引擎输出率
playback_chain = tts, src, playback
playback_chain_param_src.out_sample_rate = 16000
playback_chain_param_tts.model_path = tts/kokoro-v1_0.mnn/tts.mnn
playback_chain_param_tts.voices_path = tts/kokoro-v1_0.mnn/voices.mnn
playback_chain_param_tts.tokens_path = tts/kokoro-v1_0.mnn/tokenizer.txt
```

## C API 示例

```c
#include <stdio.h>

#include "agent_export.h"

static void on_event(const aura_agent_event_data_t *ev, void *user)
{
    (void)user;
    if (ev->type == AURA_AGENT_EVENT_STATE_CHANGED) {
        printf("state: %d -> %d\n", ev->from_state, ev->to_state);
    } else if (ev->type == AURA_AGENT_EVENT_ASR_FINAL) {
        printf("asr: %s\n", ev->text);
    }
}

int main(void)
{
    aura_agent_config_t cfg;

    aura_agent_config_default(&cfg);
    if (aura_agent_config_load(&cfg, "configs/agent/minimal.conf") != AURA_OK) {
        return 1;
    }

    if (aura_agent_init(&cfg) != AURA_OK) {
        return 1;
    }
    aura_agent_set_event_callback(on_event, NULL);
    if (aura_agent_start() != AURA_OK) {
        aura_agent_deinit();
        return 1;
    }

    /* 采集线程循环喂入：16 kHz 单声道、10 ms（160 样本）一帧 */
    /* aura_agent_feed_audio(pcm, 160, 1, AURA_AGENT_AUDIO_S16, 0, 10); */

    aura_agent_stop();
    aura_agent_deinit();
    return 0;
}
```

CMake 中链接主库即可（头文件路径随目标传播）：

```cmake
target_link_libraries(your_app PRIVATE aura_agent)
```

线程与错误约定见 [include/agent_export.h](include/agent_export.h) 头部注释：
生命周期接口由同一外部线程串行调用，`feed_audio` 可由采集线程并发调用；
事件回调在框架内部线程执行，禁止长阻塞。

## 播放 HAL

`playback` sink 不绑定具体音频设备，由应用在 `aura_agent_start()` 前注入后端
（[include/aura_playback.h](include/aura_playback.h)）：

```c
static aura_err_t hal_open(void *user, uint32_t rate, uint32_t channels) { /* 打开 ALSA/板级设备 */ }
static aura_err_t hal_write(void *user, const int16_t *pcm, uint32_t frames,
                            uint32_t channels, uint64_t pts_us) { /* 写入设备（10ms 一帧） */ }
static void hal_flush(void *user) { /* barge-in：立即停播并丢弃未播数据 */ }
static void hal_close(void *user) { /* 关闭设备 */ }

aura_playback_backend_t backend = {
    .name = "board", .user = NULL,
    .open = hal_open, .write = hal_write, .flush = hal_flush, .close = hal_close,
};
aura_agent_set_playback_backend(&backend);
```

未注入后端时使用 null 后端（丢弃音频，仅保留流与统计）。sink 会把送播 PCM
按 pts 回采给采集链的 AEC 参考流。

## 架构

### 三个平面

| 平面 | 位置 | 职责 |
|---|---|---|
| 音频实时面 | `core/pipeline` | PCM / pts / 帧池、每节点任务；允许丢帧、禁止长阻塞 |
| Agent 数据面 | `src/agent/flow` + `stages` | topic 路由、有界队列、背压、中间件链、取消、trace/metrics；含 tts_egress |
| 控制观察面 | `core/event_bus` + 状态机 | 广播状态、错误与底层事件；可丢 |

`flow_bridge` 负责 event_bus 与 flow 之间的转换，两个平面不直接互相 include。

### 分层依赖（禁止反向）

```text
apps → agent（agent_core + flow + dialogue/tools/memory）
        → core（pipeline / event_bus / state_machine / algorithm / config）
        → utils → platform
        → soundproof（适配层）── C ABI ──▶ soundproof 仓库
```

- Agent 层不做信号处理、不跑算法、不 include soundproof 算法头；
- 适配层只 include `audio_engine_*.h`（纯 C），MNN 细节不泄漏；
- 只有 `platform/` 允许板级 SDK 头；
- 对外 API 为纯 C（`include/agent_export.h`）。

### 目录结构

```text
Aura/
├── include/agent_export.h     # 对外纯 C API
├── include/aura_playback.h    # 播放 HAL 后端契约
├── src/
│   ├── agent/                 # agent_core + flow 中间件
│   │   ├── stages/            # session / dialogue / memory / tool_runner /
│   │   │                      # orchestrator / tts_egress
│   │   ├── tools/             # 工具注册表（重试/幂等）
│   │   ├── llm/               # LLM 后端抽象（mock；AE_LLM 在 Phase C2）
│   │   └── memory/            # 短期记忆环（session/speaker 隔离）
│   ├── core/                  # pipeline / event_bus / state_machine / algorithm /
│   │                          # config / playback sink
│   ├── soundproof/            # soundproof C ABI 适配节点（无算法实现）
│   └── utils/                 # logger / mem_pool / ringbuf / profiler / error
├── platform/osal/             # OSAL（当前仅 POSIX：Linux / Windows）
├── configs/agent/             # 运行期配置样例（minimal/capture/full_duplex）
├── cmake/                     # soundproof FetchContent、aura_config.h 模板
├── toolchains/                # x86_64 / aarch64 / armhf CMake 工具链
├── tests/
│   ├── unit/                  # 零依赖 C11 单元测试（核心 / flow / agent / playback）
│   ├── host_sim/              # 无硬件 / 无模型的音频面全链路仿真
│   ├── flow_chain/            # 文本面闭环仿真（真实 bridge + stage）
│   ├── playback_chain/        # tts.say → TTS → playback + barge-in（mock）
│   └── soundproof_chain/      # 真实 soundproof VAD 适配链测试
├── apps/                      # 板端 demo（Phase C 起补齐）
└── scripts/cross_compile.sh   # aarch64 / armhf 交叉编译
```

## 构建选项

| 选项 | 默认 | 说明 |
|---|---|---|
| `AURA_WITH_SOUNDPROOF` | ON | 通过 FetchContent 接入 soundproof 算法引擎 |
| `AURA_WITH_SOUNDPROOF_MNN` | ON | 启用 MNN 后端（KWS / ASR / SR / TTS / TURN / silero VAD）；OFF 时只构建 DSP |
| `AURA_WITH_SOUNDPROOF_LLM` | OFF | 启用 MNN-LLM + OpenCV（重型，Phase C2） |
| `AURA_BUILD_VOICEPRINT` | ON | 编入声纹（SR）适配节点 |
| `AURA_BUILD_TTS` | ON | 编入 TTS 适配节点（需要 MNN） |
| `AURA_BUILD_LLM` | OFF | 编入 LLM 适配节点（需 soundproof LLM，Phase C2） |
| `AURA_BUILD_TESTS` | ON | 构建单元测试 |
| `AURA_BUILD_HOST_SIM` | ON | 构建 PC 全链路仿真 `host_sim` |
| `AURA_SOUNDPROOF_TESTS` | OFF | 同时构建 soundproof 自带的 `test_ae_*` |

> KWS / ASR 适配节点随 `AURA_WITH_SOUNDPROOF_MNN` 一并编译；
> `AURA_BUILD_BARGE_IN` / `AURA_BUILD_NET` 当前仅影响编译期能力标记（Phase C/D 落地）。

## 项目状态

**Phase A / B 已完成，Phase C1（播放链与 TTS）已完成**：soundproof 通过
FetchContent 接入，DSP / VAD / KWS / SR / ASR / TTS 适配节点、Agent 数据流
中间件、对话 stage 与 `tts → src → playback` 播放链落地；DSP-only 与 MNN 全量
构建均通过，`ctest -L aura` 15/15。LLM 仍为 mock 后端（C2），板级播放 HAL
与真实模型回归属 C3。

- ✅ Phase 0–3：选型、框架骨架、算法主体（已移交 soundproof）
- ✅ **Phase A**：架构解耦、soundproof 适配层、Agent 数据流骨架
- ✅ **Phase B**：Agent 数据流 stage、工具循环、记忆、`flow_chain` 闭环
- 🚧 **Phase C**：C1 播放链/TTS/barge-in 已完成；C2 AE_LLM、C3 上板回归待做
- ⏳ **Phase D**：云端增强与 net 层

## 相关仓库与文档

- [soundproof](https://github.com/Ryuk17/soundproof) —— 算法引擎、模型权重、MNN、
  `AudioEngine_*` C ABI 与算法测试 `test_ae_*`。
- 设计文档（架构、pipeline、数据流、内存预算等）在本地 `docs/` 目录维护，
  按项目约定不随仓库分发。

## License

[Apache License 2.0](LICENSE)
