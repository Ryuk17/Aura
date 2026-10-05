/* Aura — audio_debug 专用 WAV 读写（16-bit PCM，交错多通道，流式写）
 *
 * 与 tests/host_sim/wav_io 那份的分工：host_sim 的是 Phase 1 仿真约定
 * （单声道、立体声自动下混、一次性整段写），这里要的恰恰相反 ——
 *   - 读：**保留交错多通道**（链首就是 2 路麦阵列，下混等于把验证对象扔掉）；
 *   - 写：**流式**（观察者节点每来一帧就追加，停止时才回填头），
 *     否则一条 4 秒的链要把六七个节点的音频全攒在内存里。
 * 目标不同，实现也各不相同，所以各留一份而不是硬凑成一个"通用"接口。
 */
#ifndef AURA_TOOLS_AUDIO_DEBUG_WAV_IO_H
#define AURA_TOOLS_AUDIO_DEBUG_WAV_IO_H

#include <stdint.h>

#include "utils/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct wav_data {
    int16_t *samples;     /* 交错：f0c0, f0c1, f1c0, f1c1, ...（malloc，用 wav_free 释放） */
    uint32_t frames;      /* 每通道样本数（不是样本总数） */
    uint32_t channels;
    uint32_t sample_rate;
} wav_data_t;

/* 读 16-bit PCM WAV。只认 fmt=1/位深 16（工具链上不会出现别的格式，
 * 出现了就明确报错，不做静默转换）。文件不存在 → AURA_ERR_IO。 */
aura_err_t wav_read_ex(const char *path, wav_data_t *out);

/* 读两个单声道 WAV 并交织成 2 路 —— 真实阵列录音是"每个通道一个文件"的形态
 * （microphone_array 目录下的 .CH0.wav 与 .CH1.wav）。采样率与长度必须一致。 */
aura_err_t wav_read_interleave2(const char *path0, const char *path1, wav_data_t *out);

void wav_free(wav_data_t *d);

/* 确保目录存在（不存在则建，已存在也算成功，只建一层）。
 * 放在这里而不是各调用方各写一遍：落盘目录是"工具与 CTest 都要自己准备"的东西，
 * 而 wavtap 一旦写不出文件只是默默记 write_errors —— 缺目录会表现成"指标全绿、
 * 文件不在"，这种失败没有人能在第一时间想明白。 */
aura_err_t wav_ensure_dir(const char *dir);

/* ------------------------------- 流式写 ------------------------------- */
typedef struct wav_writer wav_writer_t;

/* 建文件并先写占位头（长度为 0），wav_writer_close 时回填。
 * 失败返回 NULL（目录不存在、无写权限等）。 */
wav_writer_t *wav_writer_open(const char *path, uint32_t sample_rate, uint32_t channels);

/* 追加 frames 个**每通道**样本（samples 为交错数据）。 */
aura_err_t wav_writer_write(wav_writer_t *w, const int16_t *samples, uint32_t frames);

/* 回填 RIFF/data 长度并关闭。返回后 w 失效。 */
aura_err_t wav_writer_close(wav_writer_t *w);

#ifdef __cplusplus
}
#endif

#endif /* AURA_TOOLS_AUDIO_DEBUG_WAV_IO_H */
