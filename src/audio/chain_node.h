// 算法链节点接口：帧级同步处理（采集线程调用，不做重活）
// 全链路默认 16 kHz / 16 bit / mono；节点可通过 in_rate/out_rate 声明采样率
// 变换（如 SRC 节点），链负责缓冲衔接
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace aura::audio {

class IChainNode {
public:
    virtual ~IChainNode() = default;

    // 处理一帧。in/out 可为同一缓冲。
    //   frames   : 输入样本数
    //   cap_out  : out 容量（样本数）
    //   返回输出样本数；失败返回 0
    virtual size_t Process(const int16_t* in, size_t frames, int16_t* out,
                           size_t cap_out) = 0;

    virtual int in_rate() const = 0;
    virtual int out_rate() const = 0;
    virtual const char* name() const = 0;
};

}  // namespace aura::audio
