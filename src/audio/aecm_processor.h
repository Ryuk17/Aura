// AECM（Acoustic Echo Canceller Mobile）封装
// 底层为 WebRTC AECM 移植（third_party/aecm，C API），此处提供 C++ 帧级接口。
// 使用约定：16 kHz 采样率，每帧 10ms = 160 样本。
#pragma once

#include <cstdint>

namespace aura {

class AecmProcessor {
public:
    AecmProcessor() = default;
    ~AecmProcessor();

    AecmProcessor(const AecmProcessor&) = delete;
    AecmProcessor& operator=(const AecmProcessor&) = delete;

    // 初始化（默认 16 kHz；AECM 仅支持 8k/16k）
    bool Init(int sample_rate = 16000);

    // 处理一帧（160 样本 @16k）：
    //   farend  : 播放信号（无播放数据时传 nullptr，等效静音）
    //   nearend : 麦克风采集信号
    //   out     : 输出缓冲（长度 >= 160）
    //   ms_in_sndcard_buf: 播放卡内缓冲延迟估计（帧同步输入传 0；真实管线传播放缓冲深度）
    void ProcessFrame(const int16_t* farend, const int16_t* nearend, int16_t* out,
                      int16_t ms_in_sndcard_buf = 20);

    // echoMode 0-4（0 最激进，4 最保守），默认 3
    void SetEchoMode(int mode);

    bool initialized() const { return inited_; }
    int frame_size() const { return frame_size_; }

private:
    void* aecm_ = nullptr;
    int frame_size_ = 0;
    int echo_mode_ = 3;
    bool inited_ = false;
};

}  // namespace aura
