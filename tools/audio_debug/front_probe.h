/* Aura — 语音前端探针：把一条链改写成"处处可观测"的链，并喂数据
 *
 * 工具（audio_debug）与测试（tests/audio_front）共用这里的两件事：
 *
 *   ① 探针链重写 —— 每个产音频的节点后面挂一个 wavtap 观察者，链尾再挂一个
 *      终点 tap。观察者是**旁路扇出**，主线照原样往下走，所以落下来的 PCM
 *      就是该节点真实的输出。
 *
 *   ② 喂帧节奏 —— 每帧"先参考后近端"，**同一时刻**（同一个 pts），参考不领先。
 *      这里曾经写成"参考领先一帧"，那是在参考无 pts 配对、靠 FIFO 撞的时刻：
 *      一旦近端跑得比参考快，AEC 取到的就是未来的参考，偏差会一路累积成几百
 *      毫秒的固定错位，而表现只是 ERLE 偏低，不报错。现在配对由 pipeline 按
 *      pts 做（见 aura_pipeline_feed_ref 的注释），喂错时刻会被明确计入
 *      ref_stale，所以喂法是"同轴对齐"而不是"提前一点"。
 *
 * 这两件事都很容易"两边各写一遍然后慢慢跑偏"（一边改对了，另一边还是旧的，
 * 于是出现"测试通过但工具里听得出来"这类没人解释得清的偏差）。所以放这里，
 * 有且只有一份。
 */
#ifndef AURA_TOOLS_AUDIO_DEBUG_FRONT_PROBE_H
#define AURA_TOOLS_AUDIO_DEBUG_FRONT_PROBE_H

#include <stdbool.h>
#include <stdint.h>

#include "core/algorithm.h"
#include "core/pipeline/pipeline.h"
#include "utils/error.h"
#include "wav_io.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AURA_PROBE_MAX_TAPS 16

/* 一个探针的登记项：观察者名 → 被观察节点，外加该节点声明过的形状变化。
 * 有了它，"SRC 真的跑到 8k 了吗""BF 真的收成 1 路了吗"才能被自动断言 ——
 * 这类错误不报错、不崩，只会让下游拿到错误时基的数据。 */
typedef struct aura_probe_tap {
    char     tap[AURA_CHAIN_NAME_MAX];  /* 观察者节点名（= 落盘文件名） */
    char     node[AURA_CHAIN_NAME_MAX]; /* 被观察节点名 */
    char     algo[AURA_CHAIN_NAME_MAX]; /* 被观察算法注册名 */
    uint32_t declared_out_rate;         /* changes_frame_rate 的声明输出率；0 = 不变 */
    bool     changes_channels;
} aura_probe_tap_t;

/* 把 src 改写成带探针的链 dst，并把每个探针登记到 taps（最多 AURA_PROBE_MAX_TAPS）。
 * 调用方负责给 taps 数组（大小 AURA_PROBE_MAX_TAPS）。 */
aura_err_t aura_probe_chain(const aura_chain_t *src, aura_chain_t *dst, aura_probe_tap_t *taps,
                            uint32_t *tap_count);

/* 按名字在 build 出的节点数组里找节点（找不到返回 NULL）。 */
aura_node_t *aura_probe_find(aura_node_t **nodes, uint32_t count, const char *name);

/* 输入源（近端 + 可选参考），都已读进内存。 */
typedef struct aura_probe_input {
    const int16_t *mic; /* 交错 */
    uint32_t       mic_channels;
    const int16_t *ref; /* 交错；NULL = 无参考（AEC 走静音顶替的降级路径） */
    uint32_t       ref_channels;
    uint32_t       frames;      /* 要喂的帧数 */
    uint32_t       frame_count; /* 每帧每通道样本数 = sample_rate × frame_ms / 1000 */
    uint32_t       frame_ms;
} aura_probe_input_t;

/* 按帧喂入：先喂 ref[0]，此后每帧"喂 mic[i] 再补 ref[i+1]"，全程保持参考领先一步。
 * 近端 pts 交给 pipeline 自动推进，参考 pts 与近端同轴。返回首个错误。 */
aura_err_t aura_probe_feed(aura_pipeline_t *p, const aura_probe_input_t *in);

#ifdef __cplusplus
}
#endif

#endif /* AURA_TOOLS_AUDIO_DEBUG_FRONT_PROBE_H */
