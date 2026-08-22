// 模块接口定义（公共头）：引擎模块对编排层暴露的契约
// M1 阶段：仅音频 I/O 接口落地；ASR/LLM/TTS 等为占位（M2/M3 补充）
#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace aura {

// ---------------- 音频 I/O ----------------

// 音频采集源：16 kHz / 16 bit / mono（由 AURA_SAMPLE_RATE 宏保证），回调在采集线程调用
class IAudioSource {
public:
    using SampleCallback = std::function<void(const int16_t* data, size_t frames)>;

    virtual ~IAudioSource() = default;

    // 启动采集，数据经回调逐块（约 10ms）送达
    virtual bool Start(SampleCallback cb) = 0;
    virtual void Stop() = 0;

    virtual int sample_rate() const = 0;
    virtual bool IsCapturing() const = 0;
};

// 音频播放出口：非阻塞入队，内部线程消费
class IAudioSink {
public:
    virtual ~IAudioSink() = default;

    virtual bool Start() = 0;
    virtual void Stop() = 0;
    // 入队播放（16 kHz/16bit/mono）；barge-in 时先调用 Stop() 立即静音
    virtual void Push(const int16_t* data, size_t frames) = 0;
    virtual void Clear() = 0;  // 清空待播数据（打断用）

    virtual int sample_rate() const = 0;
    virtual bool IsPlaying() const = 0;
};

// ---------------- 引擎模块（M1 占位，M2/M3 补充） ----------------

class IVad {
public:
    virtual ~IVad() = default;
};

class IAsr {
public:
    virtual ~IAsr() = default;
};

class ILlm {
public:
    virtual ~ILlm() = default;
};

class ITts {
public:
    virtual ~ITts() = default;
};

class IWakeWord {
public:
    virtual ~IWakeWord() = default;
};

}  // namespace aura
