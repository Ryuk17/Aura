/* Aura — wavtap：观察者节点（统计 + 落盘）
 *
 * 注册名 `wavtap`。能力声明是"只消费、不产出"，于是它在链上有两种合法挂法
 * （见 core/algorithm.h 的 io 注释）：
 *   - **旁路观察者**：挂在任意产音频节点的扇出上（`tee(...)` 的语义），
 *     主线继续往下走，它单独拿一份帧拷贝；
 *   - **终点**：挂在链尾，收整条链的最终输出。
 * 两种挂法在 audio_debug 里都用：每个节点后面挂一个 `<节点名>_out`，链尾再挂
 * 一个 `out`。
 *
 * 它做两件事，缺一不可：
 *   落盘 —— 每帧原样写 WAV，**惰性开文件**（第一帧才知道采样率/通道数，
 *           链中间的 SRC 会改采样率，提前建不了）；文件名 = 节点实例名。
 *   统计 —— 逐帧能量数组 + 峰值/削波/形状。CTest 的指标全从这里取，
 *           不去重新读 WAV：读回来的数据已经过一遍磁盘，出了问题分不清是
 *           "算法不对"还是"落盘写错"。
 *
 * 为什么放在 tools/ 而不是 core/：它是调试设施，不是链路的一部分。
 * 正式固件的链上不该出现它。
 */
#ifndef AURA_TOOLS_AUDIO_DEBUG_WAVTAP_H
#define AURA_TOOLS_AUDIO_DEBUG_WAVTAP_H

#include <stdbool.h>
#include <stdint.h>

#include "core/node.h"
#include "utils/error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 逐帧能量的记录上限（帧数）。4096 帧 × 10ms ≈ 40 秒，远比任何调试片段长；
 * 超出的帧只计总账不记明细，不动态扩容 —— 音频热路径上不做 malloc。 */
#define WAVTAP_MAX_FRAMES 4096

typedef struct wavtap_stats {
    /* 账面 */
    uint64_t frames_in;      /* 收到的帧数 */
    uint64_t samples_total;  /* 累计**每通道**样本数 */
    uint64_t frames_written; /* 实际落盘的帧数（未设 out_dir 时恒为 0） */
    uint64_t write_errors;

    /* 形状：首帧与最近一帧各记一份。首帧决定 WAV 头；最近一帧用来断言
     * "链中间的 SRC/BF 真的改了形状"（last_rate / last_channels）。 */
    uint32_t rate;             /* 首帧采样率 */
    uint32_t channels;         /* 首帧通道数 */
    uint32_t last_rate;        /* 最近一帧采样率 */
    uint32_t last_channels;    /* 最近一帧通道数 */
    uint32_t last_frame_count; /* 最近一帧的每通道样本数 */

    /* 信号：口径与 metrics.h 一致（每通道归一化的均方） */
    uint32_t energy_frames;                 /* frame_energy 中的有效帧数 */
    double   frame_energy[WAVTAP_MAX_FRAMES];
    int32_t  peak;    /* 全段 |x| 最大值 */
    uint64_t clipped; /* |x| ≥ 0.98×32767 的样本数 */
} wavtap_stats_t;

/* 注册 `wavtap` 到算法注册表。**幂等**：已注册则直接返回 AURA_OK。 */
aura_err_t wavtap_register(void);

/* 设置落盘目录（可为 NULL/"" = 只统计不落盘）。落地文件名为
 * `<out_dir>/<节点实例名>.wav`。目录必须已存在 —— 建目录是驱动层的事
 * （工具里一处 mkdir 胜过这里悄悄多一层路径拼装）。
 * 需在 pipeline 启动前调用；节点在第一帧时才读这个值。 */
void wavtap_set_out_dir(const char *dir);

/* 读统计。node 不是 wavtap 造的 → AURA_ERR_INVALID_ARG。 */
aura_err_t wavtap_get_stats(const aura_node_t *node, wavtap_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* AURA_TOOLS_AUDIO_DEBUG_WAVTAP_H */
