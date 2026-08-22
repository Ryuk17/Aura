#include "aecm_processor.h"

#include "log.h"

#include "audio_processing/acoustic_echo_cancellation_mobile/echo_control_mobile.h"

#include <cstring>

// WebRTC AECM 的 C API 声明在 webrtc 命名空间内（extern "C"）
using namespace webrtc;

namespace aura {

namespace {
constexpr const char* kTag = "aecm";
}

AecmProcessor::~AecmProcessor() {
    if (aecm_) {
        WebRtcAecm_Free(aecm_);
        aecm_ = nullptr;
    }
}

bool AecmProcessor::Init(int sample_rate) {
    if (inited_) return true;
    if (sample_rate != 8000 && sample_rate != 16000) {
        ALOG_ERROR(kTag, "unsupported sample rate %d (only 8k/16k)", sample_rate);
        return false;
    }
    aecm_ = WebRtcAecm_Create();
    if (!aecm_) {
        ALOG_ERROR(kTag, "WebRtcAecm_Create failed");
        return false;
    }
    if (WebRtcAecm_Init(aecm_, sample_rate) != 0) {
        ALOG_ERROR(kTag, "WebRtcAecm_Init failed");
        WebRtcAecm_Free(aecm_);
        aecm_ = nullptr;
        return false;
    }

    AecmConfig config;
    config.cngMode = AecmTrue;
    config.echoMode = static_cast<int16_t>(echo_mode_);
    WebRtcAecm_set_config(aecm_, config);

    frame_size_ = sample_rate / 100;  // 10ms 帧
    inited_ = true;
    ALOG_INFO(kTag, "initialized at %d Hz, frame=%d, echoMode=%d", sample_rate, frame_size_,
              echo_mode_);
    return true;
}

void AecmProcessor::ProcessFrame(const int16_t* farend, const int16_t* nearend,
                                 int16_t* out, int16_t ms_in_sndcard_buf) {
    if (!inited_ || !nearend || !out) return;

    // farend 静音补齐：AECM 需要与 nearend 等长的 farend 帧
    int16_t silence[160];
    if (!farend) {
        memset(silence, 0, sizeof(silence));
        farend = silence;
    }

    // 标准用法：farend 先入参考缓冲，再处理近端
    WebRtcAecm_BufferFarend(aecm_, farend, static_cast<int16_t>(frame_size_));

    // nearendClean 传 nullptr（无参考通道，AECM 内部按 noisy 处理）
    WebRtcAecm_Process(aecm_, nearend, nullptr, out, static_cast<int16_t>(frame_size_),
                       ms_in_sndcard_buf);
}

void AecmProcessor::SetEchoMode(int mode) {
    if (mode < 0 || mode > 4) {
        ALOG_WARN(kTag, "invalid echoMode %d, keep %d", mode, echo_mode_);
        return;
    }
    echo_mode_ = mode;
    if (inited_) {
        AecmConfig config;
        config.cngMode = AecmTrue;
        config.echoMode = static_cast<int16_t>(mode);
        WebRtcAecm_set_config(aecm_, config);
    }
}

}  // namespace aura
