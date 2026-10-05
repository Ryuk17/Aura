/* Aura — 合成回声场景（麦克风接不上时的验证信号源）
 *
 * 为什么要有它：Phase 2 的验证要在**没有真麦克风**的前提下回答"回声消干净了吗"。
 * 真实录音（assets/audio/microphone_array）里没有播放回采，AEC 拿不到参考，
 * 那条路径只能验证"缺参考时不崩"；要验证"有参考时真的在消"，就得自己造一个
 * **回声路径已知**的场景。
 *
 * 场景（16 kHz / 10 ms 帧 / 6.0 s / 600 帧 / 2 路麦，帧号 = 时间 × 100）：
 *
 *   帧区间      近端（每通道）                参考流（远端）
 *   [  0,  60)  近端语音 N（amp 0.5）         静音
 *   [ 60, 120)  静音（仅 LSB 底噪）            静音
 *   [120, 400)  纯回声 h(t)⊛F                  F（独立种子，amp 0.9）
 *   [400, 550)  双讲：N2 + h(t)⊛F2             F2（独立种子，amp 0.9）
 *   [550, 600)  仅近端 N2                      静音
 *
 * 段边界为什么拉这么靠后：AEC3 的延迟估计在跑动（引擎日志 "Delay changed to 0
 * at block 562" ≈ 2.25 s），估计没稳之前的双讲段不可信 —— 早期版本把双讲放在
 * 2.8 s 起，量到的近端被压掉 45 dB，而同引擎在自带双讲数据上有 −2~−10 dB。
 * 回声段留到 4.0 s 才结束，双讲 5.5 s 停，就是为了让"收敛"与"双讲"这两件事
 * 在时间上彻底分开。
 *
 * 回声路径是**线性**的（一串衰减反射，无削波无非线性失真）：这样 AEC3 收敛快、
 * ERLE 稳定，适合做"不低于某个下限"的断言，而不是"看今天收敛得怎么样"。
 * 真实的非线性回声（扬声器失真）留给上板阶段。
 *
 * 完全确定性：全部用带种子的 LCG，同一个种子逐样本一致。测试要能复现，
 * 就不能依赖"这次跑出来的随机数"。
 */
#ifndef AURA_TOOLS_AUDIO_DEBUG_SYNTH_SCENE_H
#define AURA_TOOLS_AUDIO_DEBUG_SYNTH_SCENE_H

#include <stdint.h>

#include "utils/error.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SYNTH_RATE      16000u
#define SYNTH_FRAME_MS  10u
#define SYNTH_CHANNELS  2u
#define SYNTH_SAMPLES   (6u * SYNTH_RATE)                        /* 每通道样本数 */
#define SYNTH_FRAMES    (6u * 1000u / SYNTH_FRAME_MS)            /* 帧数 */
#define SYNTH_ECHO_DELAY 96u  /* 回声路径首径延迟（样本）= 6ms @16k。真实板子上
                               * 播放→回采本来就带延迟，AEC3 的延迟估计要的就是
                               * 这个量级；设 0 会让"估计"这条路永远不被走到。 */
#define SYNTH_ECHO_GAIN  0.35 /* 回声路径首径增益（线性） */

/* 回声路径的抽头（延迟/样本，增益）。首径用 SYNTH_ECHO_DELAY/GAIN 的值，
 * 其余是几条衰减的反射 —— 为什么要多根见 synth_scene.c 的回声注入处。 */
typedef struct synth_echo_tap {
    uint32_t delay;
    float    gain;
} synth_echo_tap_t;

#define SYNTH_ECHO_TAP_COUNT 4u
static const synth_echo_tap_t SYNTH_ECHO_TAPS[SYNTH_ECHO_TAP_COUNT] = {
    {SYNTH_ECHO_DELAY, SYNTH_ECHO_GAIN}, /* 6.0 ms  直达声 */
    {232u, 0.18f},                       /* 14.5 ms 桌面 */
    {481u, 0.09f},                       /* 30.1 ms 侧墙 */
    {913u, 0.05f},                       /* 57.1 ms 后墙 */
};

/* 段边界（帧号）。测试的指标窗口与这里的定义必须对得上，所以只在这里写一份，
 * 别处引用这些宏 —— 两边各写一遍是"改了场景忘了改阈值"的经典来源。 */
#define SYNTH_FRAME_SPEECH_A_0  0u   /* 近端单讲 */
#define SYNTH_FRAME_SPEECH_A_1  60u
#define SYNTH_FRAME_SILENCE_0   60u  /* 全静音（只有底噪） */
#define SYNTH_FRAME_SILENCE_1   120u
#define SYNTH_FRAME_ECHO_0      120u /* 纯回声段（远端单讲） */
#define SYNTH_FRAME_ECHO_1      400u
#define SYNTH_FRAME_ERLE_0      320u /* ERLE 统计窗口：回声段尾部，收敛已完成 */
#define SYNTH_FRAME_ERLE_1      395u
#define SYNTH_FRAME_DT_0        400u /* 双讲段 */
#define SYNTH_FRAME_DT_SOLO_0   550u /* 远端停播处：其后只剩近端（参考也静音） */
#define SYNTH_FRAME_TOTAL       600u
#define SYNTH_FRAME_NEARFID_0   20u  /* 近端保真窗口（首段单讲，跳过起播 0.2s）*/
#define SYNTH_FRAME_NEARFID_1   55u

/* 帧号 → 样本号。**必须**用这个宏，别在别处写 `frame * SYNTH_RATE / 1000`
 * —— 那等于把帧号当毫秒用，而 1 帧是 SYNTH_FRAME_MS 毫秒，转换结果会整体
 * 差 10 倍：段边界全部提前，场景被压进头十分之一的时长里，剩下的全是静音，
 * 而"静音段"上的能量比指标退化成 0/0，看上去还像是跑通了。 */
#define SYNTH_F2S(f) ((uint32_t)(f) * (SYNTH_RATE / 1000u) * SYNTH_FRAME_MS)
#define SYNTH_S2F(s) ((uint32_t)(s) / ((SYNTH_RATE / 1000u) * SYNTH_FRAME_MS))

typedef struct synth_scene {
    uint32_t sample_rate;
    uint32_t frame_ms;
    uint32_t channels;
    uint32_t frames;  /* 每通道样本数 */
    int16_t *mic;     /* 交错 2 路：近端（含回声） */
    int16_t *ref;     /* 交错 2 路：远端参考（mono 复制两通道）*/
} synth_scene_t;

/* 造场景（malloc；用 synth_scene_free 释放）。seed 相同 → 逐样本相同。 */
aura_err_t synth_scene_build(synth_scene_t *out, uint32_t seed);

void synth_scene_free(synth_scene_t *s);

/* 落盘：<prefix>_mic.wav + <prefix>_ref.wav。 */
aura_err_t synth_scene_write(const char *prefix, const synth_scene_t *s);

#ifdef __cplusplus
}
#endif

#endif /* AURA_TOOLS_AUDIO_DEBUG_SYNTH_SCENE_H */
