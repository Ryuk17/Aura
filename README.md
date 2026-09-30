# Aura
Voice Agent on Raspberry Pi

## Aura Algorithms Flow
 ![Aura Flow](/assets/Aura_flow.png)

端侧全离线语音交互框架：`mic → AFE(3A/BSS) → VAD → KWS → 声纹 → ASR → LLM/Agent → TTS → playback`。
推理统一走 MNN；云端仅作增强与兜底。目标平台为 Linux SBC（树莓派），**不支持 RTOS**。

**当前状态：Phase 1（框架骨架）完成** —— 构建/OSAL/pipeline/事件总线/状态机/推理封装已落地，
PC 上 CTest 8/8 全绿。

## 文档

> ⚠️ `docs/` 当前**不入版本库**（见 [.gitignore](.gitignore)）—— 设计文档只存在于本地工作副本，
> 下面这些链接在 clone 出来的仓库里打不开。

| 文档 | 内容 |
|---|---|
| [docs/todo.md](docs/todo.md) | ★ 总计划：目录结构、分层规则、Phase 0–5、验收指标（唯一权威文档） |
| [docs/architecture.md](docs/architecture.md) | 已实现的结构、推理层两种形态、构建裁剪 |
| [docs/pipeline.md](docs/pipeline.md) | 节点契约、事件总线、状态机迁移表、barge-in |
| [docs/model_requirement.md](docs/model_requirement.md) | 各模型 I/O、量化方式、MNN 版本约束 |

## 构建

第三方依赖（MNN / TrickRoom）不进 git，由 CMake FetchContent 在配置阶段拉取并校验 SHA256。

**Windows（MinGW-w64，当前主开发平台）**

```bash
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_TOOLCHAIN_FILE=toolchains/windows-x86_64.cmake
cmake --build build -j
```

**Linux**

```bash
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=toolchains/linux-x86_64.cmake
cmake --build build -j
```

常用裁剪开关（默认见 [CMakeLists.txt](CMakeLists.txt)）：`AURA_BUILD_KWS/VOICEPRINT/ASR/LLM/TTS/BARGE_IN/NET`
控制模块是否编入，`AURA_WITH_MNN / AURA_WITH_MNN_LLM / AURA_WITH_TRICKROOM` 控制引擎依赖。

## 运行与测试

```bash
ctest --test-dir build --output-on-failure        # 单测 + host_sim，当前 8/8

./build/tests/host_sim                            # PC 全链路仿真：对话闭环 + 打断
./build/tests/host_sim --voiceprint-fail          # 声纹拒绝退回 Idle
./build/tests/host_sim --inject-error 500         # 注入模块错误 → Error → 自动复位
./build/tests/host_sim --use-silero               # 用真实 silero VAD 替能量 VAD
```

`host_sim` 用 `host_sim_in.wav` 喂音频驱动完整链路。**注意其中的算法节点是 mock**
（KWS 匹配 / 声纹概率 / ASR 定长延迟 / LLM 流式出 token / TTS 按 token 出块），
只用于验证骨架与状态机，真实实现在 Phase 3/4。

模型权重不入库，下载方式见 [models/download.sh](models/download.sh)。
