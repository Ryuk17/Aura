// 平台音频设备工厂（公共头）：创建采集/播放设备
// 实现按平台选择：Windows → WASAPI，Linux → ALSA
#pragma once

#include "aura/core/interfaces.h"

#include <memory>

namespace aura::platform {

// 创建音频采集设备（16 kHz / 16 bit / mono）
std::unique_ptr<IAudioSource> CreateAudioSource();

// 创建音频播放设备（16 kHz / 16 bit / mono）
std::unique_ptr<IAudioSink> CreateAudioSink();

}  // namespace aura::platform
