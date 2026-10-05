/* Aura — 合成回声场景（实现，见 synth_scene.h） */
#include "synth_scene.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wav_io.h"

/* 确定性伪随机：LCG（与 host_sim 的 mock 同一套常数，便于两边对照，
 * 也省得为几个随机数引入 <random>）。 */
static uint32_t lcg_next(uint32_t *s)
{
    *s = (*s) * 1664525u + 1013904223u;
    return *s;
}

/* 拟语音：白噪声 → 一阶低通 → 基音 f0 → 音节包络 → 首尾 50ms 淡入淡出。
 * 与 host_sim 的 gen_speech 是同一个信号模型（那里是 Phase 1 的 mock），
 * 这里重写成**带种子**的独立实现：本工具的卖点是"同一场景每次跑出来一致"，
 * 而 host_sim 那份用 static 递增 rng，跑第二次就不是同一段音频了。
 *
 * 两处"像真语音"的讲究，都不是为好看，而是场景能不能解释指标的前提：
 *
 * 1. **f0 由调用方给，近端/远端必须不同**。同频率同相位起点的两段信号是强
 *    相关的，AEC 的自适应滤波器会把近端的那个分量当回声一并消掉 —— 那样
 *    "双讲段近端保真"量到的就不是"近端有没有被吃掉"，而是"我造的两段信号
 *    有多像"。两个人本来就该是不同的基频。
 * 2. **音节包络**。没有它信号是平稳的，而 AEC3 的双讲检测靠的就是近端电平
 *    相对回声的**非平稳**起伏；喂稳态信号进去，整段近端会被判成回声压到
 *    本底（实测 -47 dB），双讲段就完全失去解释力。真实语音天然非平稳，
 *    合成的必须把这一条补上。 */
static void gen_voice(int16_t *dst, uint32_t n, uint32_t seed, float amp, float f0)
{
    uint32_t rng  = seed;
    float    lp   = 0.f;
    const uint32_t fade = (n / 20u) + 1u; /* 50ms 淡入淡出 */

    /* 音节包络：每 60~180ms 换一个增益，段间线性过渡（避免阶跃成咔哒声）。 */
    const uint32_t syll_min = SYNTH_RATE * 60u / 1000u;
    const uint32_t syll_var = SYNTH_RATE * 120u / 1000u;
    uint32_t       seg_a = 0, seg_b = 0;
    float          g_a = 0.f, g_b = 1.f;

    for (uint32_t i = 0; i < n; i++) {
        if (i >= seg_b) {
            g_a   = g_b;
            g_b   = 0.15f + (float)(lcg_next(&rng) % 1000u) / 1000.f * 0.85f;
            seg_a = seg_b;
            seg_b = i + syll_min + (lcg_next(&rng) % syll_var);
        }
        const float syll = g_a + (g_b - g_a) * (float)(i - seg_a) / (float)(seg_b - seg_a);

        const float noise = (float)((int32_t)(lcg_next(&rng) >> 8) & 0xFFFF) / 32768.f - 1.f;
        lp                = lp + 0.25f * (noise - lp); /* 一阶低通 ~1kHz */
        const double ph   = 2.0 * 3.14159265358979 * (double)f0 * (double)i / (double)SYNTH_RATE;
        const float  tone = (float)sin(ph) * 0.4f;
        const float  v    = (lp * 0.6f + tone) * amp * syll;

        float env = 1.f;
        if (i < fade) {
            env = (float)i / (float)fade;
        } else if (i > n - fade) {
            env = (float)(n - i) / (float)fade;
        }
        dst[i] = (int16_t)(v * env * 32000.f);
    }
}

aura_err_t synth_scene_build(synth_scene_t *out, uint32_t seed)
{
    if (out == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    const uint32_t n = SYNTH_SAMPLES;
    /* 中间缓冲都用**单通道原型**：近端与远端各自是一条 mono 时间线，
     * 立体声与回声注入在最后统一按样本做，避免在两条线上各写一遍。 */
    int16_t *near = (int16_t *)calloc(n, sizeof(int16_t));
    int16_t *far  = (int16_t *)calloc(n, sizeof(int16_t));
    int16_t *mic  = (int16_t *)calloc((size_t)n * SYNTH_CHANNELS, sizeof(int16_t));
    int16_t *ref  = (int16_t *)calloc((size_t)n * SYNTH_CHANNELS, sizeof(int16_t));
    if (near == NULL || far == NULL || mic == NULL || ref == NULL) {
        free(near);
        free(far);
        free(mic);
        free(ref);
        return AURA_ERR_NOMEM;
    }

    const uint32_t sp_a  = SYNTH_F2S(SYNTH_FRAME_SPEECH_A_0);
    const uint32_t sp_a1 = SYNTH_F2S(SYNTH_FRAME_SPEECH_A_1);
    const uint32_t sil0  = SYNTH_F2S(SYNTH_FRAME_SILENCE_0);
    const uint32_t sil1  = SYNTH_F2S(SYNTH_FRAME_SILENCE_1);
    const uint32_t echo0 = SYNTH_F2S(SYNTH_FRAME_ECHO_0);
    const uint32_t echo1 = SYNTH_F2S(SYNTH_FRAME_ECHO_1);
    const uint32_t dt0   = SYNTH_F2S(SYNTH_FRAME_DT_0);
    const uint32_t solo  = SYNTH_F2S(SYNTH_FRAME_DT_SOLO_0);

    /* --- 近端：两段独立种子的拟语音（近端基频 180Hz） --- */
    gen_voice(near + sp_a, sp_a1 - sp_a, seed + 101u, 0.5f, 180.f);
    gen_voice(near + dt0, (uint32_t)n - dt0, seed + 303u, 0.5f, 180.f);

    /* 底噪：静音段给 LSB 级微扰。真实录音从来不是数字静音，而全零会让
     * "能量比"退化成 0/0 —— 那种输入上算出来的指标没有解释力。 */
    uint32_t rng = seed + 777u;
    for (uint32_t i = sil0; i < sil1; i++) {
        near[i] = (int16_t)((int32_t)(lcg_next(&rng) >> 16) & 0x3F) - 32;
    }

    /* --- 远端：回声段的 F 与双讲段的 F2（远端基频 210Hz，见 gen_voice 注释）。
     * F2 到 solo 就停 —— 远端停播与回声消失必须是同一时刻（见回声注入处的注释）。 --- */
    gen_voice(far + echo0, echo1 - echo0, seed + 202u, 0.9f, 210.f);
    gen_voice(far + dt0, solo - dt0, seed + 404u, 0.9f, 210.f);

    /* --- 回声注入：mic[i] += Σ gain_k · far[i − delay_k]，直到远端停播处。
     *
     * 区间必须**正好覆盖远端出声的时段**：参考还在响而回声已经消失，是物理上
     * 不可能的输入（回声路径不会凭空断掉）。那样造出来的尾段会让 AEC 把整个
     * 近端当成回声压掉（实测 −22 dB），指标看着像 AEC 坏了，实际是场景坏了。
     *
     * 路径取**多根反射**而不是单根延迟尖峰：单尖峰是自适应滤波器最容易建模的
     * 路径，收敛后残余近似为 0，AEC 的残余回声抑制器会据此认定"回声已完全掌握"，
     * 双讲段的近端会被连带压掉（实测 −44 dB，而同引擎在 TrickRoom 自带双讲
     * 数据上是 −2~−10 dB）。真实房间有四壁与桌面反射，路径本来就是一串尖峰。 --- */
    for (uint32_t i = echo0; i < solo; i++) {
        float echo = 0.f;
        for (uint32_t k = 0; k < SYNTH_ECHO_TAP_COUNT; k++) {
            /* i ≥ echo0 = 19200 > 最大抽头延迟，不会下溢 */
            echo += SYNTH_ECHO_TAPS[k].gain * (float)far[i - SYNTH_ECHO_TAPS[k].delay];
        }
        float v = (float)near[i] + echo;
        if (v > 32767.f) {
            v = 32767.f;
        } else if (v < -32768.f) {
            v = -32768.f;
        }
        near[i] = (int16_t)v;
    }

    /* --- 双麦：两路同相。
     *
     * 这里曾经写成 ch1 = delay1(ch0)，理由"20mm 间距 ≈ 1 样本"是错的：阵元间距
     * 只在**偏离宽侧向**时才产生程差，程差 = 间距·cos(方位角)。本场景的声源在
     * 半径远大于 20mm 处、方位角正是 BF 指向的 π/2（宽侧向），程差为 0 —— 两路
     * 就该是同一份信号。按错的那版造，等于把声源放到端射方向却把波束指向宽侧向，
     * BF 压掉它是对的（实测 −14 dB），而"BF 保形"这条指标就永远过不了。
     *
     * 后果：本场景**不**验证 BF 的指向性（没有可用的相位差），只验证"双通道
     * 相干求和 → 能量约 ×4"。指向性只能等真实录音或按真实坐标造的阵列仿真。
     * 两路不是简单复制同一路：ch0/ch1 的**交错**由 wav 层与 AEC/BF 的 2 路读取
     * 共同走通，写错顺序时 AEC 的 far/capture 缓冲会整体错位，指标会崩。--- */
    for (uint32_t i = 0; i < n; i++) {
        mic[2u * i]      = near[i];
        mic[2u * i + 1u] = near[i];
        ref[2u * i]      = far[i];
        ref[2u * i + 1u] = far[i];
    }

    free(near);
    free(far);

    out->sample_rate = SYNTH_RATE;
    out->frame_ms    = SYNTH_FRAME_MS;
    out->channels    = SYNTH_CHANNELS;
    out->frames      = n;
    out->mic         = mic;
    out->ref         = ref;
    return AURA_OK;
}

void synth_scene_free(synth_scene_t *s)
{
    if (s == NULL) {
        return;
    }
    free(s->mic);
    free(s->ref);
    memset(s, 0, sizeof(*s));
}

static aura_err_t write_one(const char *path, const int16_t *samples, uint32_t frames,
                            uint32_t channels, uint32_t rate)
{
    wav_writer_t *w = wav_writer_open(path, rate, channels);
    if (w == NULL) {
        return AURA_ERR_IO;
    }
    aura_err_t rc = wav_writer_write(w, samples, frames);
    const aura_err_t crc = wav_writer_close(w);
    return (rc != AURA_OK) ? rc : crc;
}

aura_err_t synth_scene_write(const char *prefix, const synth_scene_t *s)
{
    if (prefix == NULL || s == NULL || s->mic == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    char path[640];
    snprintf(path, sizeof(path), "%s_mic.wav", prefix);
    aura_err_t rc = write_one(path, s->mic, s->frames, s->channels, s->sample_rate);
    if (rc != AURA_OK) {
        return rc;
    }
    snprintf(path, sizeof(path), "%s_ref.wav", prefix);
    return write_one(path, s->ref, s->frames, s->channels, s->sample_rate);
}
