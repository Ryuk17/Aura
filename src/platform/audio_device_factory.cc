// 平台音频设备工厂：按目标平台返回采集/播放设备
#include "aura/platform/audio_device.h"

#ifdef _WIN32
#include "wasapi_device.h"
#elif defined(__linux__)
#include "alsa_device.h"
#else
#error "unsupported platform"
#endif

namespace aura::platform {

std::unique_ptr<IAudioSource> CreateAudioSource() {
#ifdef _WIN32
    return std::make_unique<WasapiCaptureDevice>();
#else
    return std::make_unique<AlsaCaptureDevice>();
#endif
}

std::unique_ptr<IAudioSink> CreateAudioSink() {
#ifdef _WIN32
    return std::make_unique<WasapiPlaybackDevice>();
#else
    return std::make_unique<AlsaPlaybackDevice>();
#endif
}

}  // namespace aura::platform
