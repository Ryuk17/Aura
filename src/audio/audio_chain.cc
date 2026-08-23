// AudioChain 实现：配置驱动节点链，帧级同步处理
#include "audio_chain.h"

#include "log.h"

#include <algorithm>
#include <cstring>

namespace aura::audio {

namespace {
constexpr const char* kTag = "audio_chain";
// 链内最大帧长度（1 秒 @16k，为 SRC 上采样预留空间）
constexpr size_t kMaxChainFrames = 16000;
}

void AudioChain::AddNode(std::unique_ptr<IChainNode> node) {
    if (node) nodes_.push_back(std::move(node));
}

bool AudioChain::Init(int sample_rate) {
    if (nodes_.empty()) {
        ALOG_WARN(kTag, "chain is empty, passthrough mode");
        return true;
    }
    bool ok = true;
    for (auto& n : nodes_) {
        ALOG_INFO(kTag, "node[%s] in=%dHz out=%dHz", n->name(), n->in_rate(),
                  n->out_rate());
        // v1：节点按构造初始化（AecmNode 有 Init）；预留统一 Init 扩展
    }
    return ok;
}

size_t AudioChain::Process(const int16_t* in, size_t frames, int16_t* out,
                           size_t cap_out) {
    if (nodes_.empty()) {
        size_t n = std::min(frames, cap_out);
        if (in != out) std::memcpy(out, in, n * sizeof(int16_t));
        return n;
    }
    if (frames > kMaxChainFrames) return 0;

    // 双缓冲交替传递（最后一个节点直接写 out）
    std::vector<int16_t> buf_a(kMaxChainFrames);
    std::vector<int16_t> buf_b(kMaxChainFrames);
    std::vector<int16_t>* tmp = &buf_a;

    const int16_t* cur_in = in;
    size_t cur_frames = frames;

    for (size_t i = 0; i < nodes_.size(); ++i) {
        bool last = (i + 1 == nodes_.size());
        int16_t* dst = last ? out : tmp->data();
        size_t cap = last ? cap_out : tmp->size();
        size_t got = nodes_[i]->Process(cur_in, cur_frames, dst, cap);
        if (got == 0) return 0;
        if (!last) {
            cur_in = tmp->data();
            cur_frames = got;
            tmp = (tmp == &buf_a) ? &buf_b : &buf_a;
        } else {
            cur_frames = got;
        }
    }
    return cur_frames;
}

IChainNode* AudioChain::Find(const char* name) {
    for (auto& n : nodes_) {
        if (std::strcmp(n->name(), name) == 0) return n.get();
    }
    return nullptr;
}

const IChainNode* AudioChain::Find(const char* name) const {
    for (const auto& n : nodes_) {
        if (std::strcmp(n->name(), name) == 0) return n.get();
    }
    return nullptr;
}

}  // namespace aura::audio
