// AudioChain：配置驱动的算法节点链（采集帧 → 节点[0..n] → 输出）
// 采集线程同步处理；节点按添加顺序执行，前一节点输出作为后一节点输入。
// v1 约束：节点内部分帧缓冲（160 样本/帧），链负责缓冲衔接与采样率适配预留。
#pragma once

#include "chain_node.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace aura::audio {

class AudioChain {
public:
    AudioChain() = default;
    ~AudioChain() = default;

    AudioChain(const AudioChain&) = delete;
    AudioChain& operator=(const AudioChain&) = delete;

    // 按添加顺序追加节点（Start 前调用）
    void AddNode(std::unique_ptr<IChainNode> node);

    // 初始化全部节点（16 kHz 默认）
    bool Init(int sample_rate = 16000);

    // 处理一帧：in（frames 样本）→ 逐节点 → out（cap_out 样本）
    // 返回输出样本数；任一节点失败返回 0
    size_t Process(const int16_t* in, size_t frames, int16_t* out, size_t cap_out);

    size_t size() const { return nodes_.size(); }
    bool empty() const { return nodes_.empty(); }

    // 查找指定名字的节点（如 "aecm"，用于 farend 注入）
    IChainNode* Find(const char* name);
    const IChainNode* Find(const char* name) const;

private:
    std::vector<std::unique_ptr<IChainNode>> nodes_;
};

}  // namespace aura::audio
