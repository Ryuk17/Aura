/* Aura — 音频帧（pipeline 的第一种流）
 *
 * 契约（todo.md 4.2）：
 *   - **所有音频帧必须携带 pts** —— AEC 参考/多麦对齐、BSS、barge-in 裁决都依赖它，
 *     缺失会造成隐性时序错位。pts 由 pipeline 层统一维护，节点不得自行猜测。
 *   - 帧是 POD，按值在队列中传递；`data` 指向的样本区由 pipeline 的内存池持有，
 *     其生命周期 = 该帧进入下一个节点之前，节点**不得跨调用持有指针**。
 */
#ifndef AURA_CORE_MSG_AUDIO_FRAME_H
#define AURA_CORE_MSG_AUDIO_FRAME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AURA_SAMPLE_S16 = 0, /* int16_t，链路主格式 */
    AURA_SAMPLE_F32 = 1, /* float，DSP/模型前端 */
} aura_sample_fmt_t;

/* 单个音频帧的最大字节容量（按最大通道数 × 最长帧预留）。
 * 需要更大帧长时改这里，不要改成动态分配。 */
#ifndef AURA_AUDIO_FRAME_MAX_BYTES
#define AURA_AUDIO_FRAME_MAX_BYTES 8192
#endif

typedef struct aura_audio_frame {
    uint64_t         pts_us;     /* 帧首样本的采集时刻（单调时钟，微秒） */
    uint32_t         sample_rate;
    uint16_t         channels;   /* 通道数（多麦阵列 = 麦数；AEC 参考通道另计） */
    uint16_t         frame_count;/* 每通道样本数 */
    uint32_t         data_bytes; /* data 中有效字节数 */
    aura_sample_fmt_t fmt;
    uint16_t         channel_id; /* 单通道拆分时标识来源通道，未拆分时为 0 */
    uint16_t         _reserved;
    uint8_t         *data;       /* 样本区；由 pipeline 内存池持有 */
} aura_audio_frame_t;

/* 计算帧所需字节数（用于校验容量）。 */
static inline uint32_t aura_audio_frame_bytes(uint16_t channels, uint16_t frame_count,
                                              aura_sample_fmt_t fmt)
{
    uint32_t sample = (fmt == AURA_SAMPLE_F32) ? 4u : 2u;
    return (uint32_t)channels * (uint32_t)frame_count * sample;
}

/* 帧内某个通道、某个样本的偏移（交错存储）。 */
static inline uint32_t aura_audio_frame_offset(const aura_audio_frame_t *f, uint16_t ch,
                                               uint16_t i)
{
    uint32_t stride = (f->fmt == AURA_SAMPLE_F32) ? 4u : 2u;
    return ((uint32_t)i * f->channels + ch) * stride;
}

/* 帧结束时刻（微秒）：pts + 帧长对应的时长。 */
static inline uint64_t aura_audio_frame_end_us(const aura_audio_frame_t *f)
{
    if (f->sample_rate == 0) {
        return f->pts_us;
    }
    uint64_t dur_us = (uint64_t)f->frame_count * 1000000ull / f->sample_rate;
    return f->pts_us + dur_us;
}

#ifdef __cplusplus
}
#endif

#endif /* AURA_CORE_MSG_AUDIO_FRAME_H */
