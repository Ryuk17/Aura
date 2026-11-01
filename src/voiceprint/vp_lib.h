/* Aura — 声纹库文件 + 离线 embedding（注册与推理分离）
 *
 * 架构（用户拍板）：**注册离线完成** —— 离线工具（tools/voiceprint_enroll）把
 * 音频算成 embedding 存进库文件；声纹节点初始化时读文件；推理时只做余弦比对。
 * 注册与运行中的链路零共享状态（这也是全链段错误的修复方向）。
 *
 * 文件格式（小端、定长条目）：
 *   magic "AVPL" (4B) | version=1 (4B) | count (4B)
 *   条目 × count：{ char id[32]; float emb[192]; }（emb 已 L2 归一化）
 */
#ifndef AURA_VOICEPRINT_VP_LIB_H
#define AURA_VOICEPRINT_VP_LIB_H

#include <stdint.h>

#include "utils/error.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AURA_VP_EMB_DIM    192
#define AURA_VP_SPK_ID_MAX 32
#define AURA_VP_LIB_MAX    16

typedef struct {
    char  id[AURA_VP_SPK_ID_MAX];
    float emb[AURA_VP_EMB_DIM];
} aura_vp_entry_t;

/* 读库文件（不存在返回 NOT_FOUND 并置 count=0 —— 空库是合法状态）。 */
aura_err_t aura_vp_lib_load(const char *path, aura_vp_entry_t *entries, int32_t max,
                            int32_t *count);

/* 写库文件（count 可为 0，写空库）。 */
aura_err_t aura_vp_lib_save(const char *path, const aura_vp_entry_t *entries,
                            int32_t count);

/* 离线 embedding：PCM（int16 单声道，n 个采样）→ fbank → 模型前向 → L2 归一化
 * 的 192 维向量。**只使用自有资源**（临时特征 + 独立模型实例），不碰任何
 * 运行中的状态 —— 可在任意线程、任意时刻调用。 */
aura_err_t aura_vp_embed_offline(const int16_t *pcm, uint32_t n, uint32_t sample_rate,
                                 const char *model_path, float emb_out[AURA_VP_EMB_DIM]);

/* 能量窗口：PCM 里能量最高的连续 ≤max_s 秒窗口的起点（采样数）。
 * 注册与验证共用同一口径（只取"有效语音"算 embedding）。 */
uint32_t aura_vp_energy_window(const int16_t *pcm, uint32_t n, uint32_t rate,
                               float max_s);

#ifdef __cplusplus
}
#endif

#endif /* AURA_VOICEPRINT_VP_LIB_H */
