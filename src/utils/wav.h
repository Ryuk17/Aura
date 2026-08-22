// WAV 工具：16-bit PCM 读写（测试 / golden 样本用）
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace aura {

struct WavInfo {
    int sample_rate = 0;
    int channels = 0;
    int bits_per_sample = 0;
};

// 读取 WAV 文件为 16-bit PCM（内部自动转为 int16 线性数据，非 16bit 输入返回 false）
// 多声道数据按帧交错保存（interleaved）
bool ReadWav(const std::string& path, std::vector<int16_t>* samples, WavInfo* info);

// 写入 16-bit PCM WAV（interleaved）
bool WriteWav(const std::string& path, const int16_t* samples, size_t sample_count,
              int sample_rate, int channels);

inline bool WriteWav(const std::string& path, const std::vector<int16_t>& samples,
                     int sample_rate, int channels = 1) {
    return WriteWav(path, samples.data(), samples.size(), sample_rate, channels);
}

}  // namespace aura
