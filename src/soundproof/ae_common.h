/* Aura — soundproof 适配层公共设施
 *
 * 这一层只做三件事：
 *   1. AudioEngine_* 状态码 → aura_err_t；
 *   2. 模型路径解析（Aura 的 model_dir + 描述表里的相对路径）；
 *   3. 帧校验 / 产出辅助。
 *
 * **不包含任何算法实现**；算法在 soundproof 里。Aura 的节点通过 C ABI 调用它。
 */
#ifndef AURA_SOUNDPROOF_AE_COMMON_H
#define AURA_SOUNDPROOF_AE_COMMON_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "core/algorithm.h"
#include "core/node.h"
#include "utils/error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 单帧最大 int16 样本数（与 AURA_AUDIO_FRAME_MAX_BYTES 对齐）。
 * 所有节点的内部缓冲按这个上限静态分配，运行期不 malloc。 */
#define AE_MAX_SAMPLES (AURA_AUDIO_FRAME_MAX_BYTES / 2)

/* 模型根目录（由 aura_agent_init 在装配前设置）。单实例，init 期写入，
 * 运行期只读。传入 NULL/空串表示模型路径按原样使用。 */
void ae_set_model_root(const char *dir);
const char *ae_model_root(void);

/* 解析模型路径：绝对路径原样拷贝；相对路径拼到 model_root 下。
 * 返回值始终是 buf（便于链式调用）；buf 容量不足时截断并返回 NULL。 */
const char *ae_resolve_path(const char *path, char *buf, size_t cap);

/* AudioEngineStatus → aura_err_t。保留原因字符串用于日志。 */
aura_err_t ae_status_to_err(int status, const char *what);

/* 校验主音频帧：S16、通道数匹配、样本数不超过内部缓冲。 */
bool ae_frame_ok(const aura_audio_frame_t *f, uint32_t channels, const char *node);

/* 产出 S16 音频帧。pts 由 pipeline 按输入帧覆盖（声源节点自算的情况除外）。 */
aura_err_t ae_emit_audio(aura_node_t *self, const int16_t *data, uint32_t samples,
                         uint32_t sample_rate, uint32_t channels);

/* 节点名：把链上的实例名拷进节点自己的缓冲（生命周期覆盖节点）。 */
void ae_name_copy(char *dst, size_t cap, const char *src);

/* 参数读取便捷封装（缺省即默认值）。 */
int32_t     ae_param_i32(const aura_algo_params_t *p, const char *name, int32_t def);
float       ae_param_f32(const aura_algo_params_t *p, const char *name, float def);
bool        ae_param_bool(const aura_algo_params_t *p, const char *name, bool def);
const char *ae_param_str(const aura_algo_params_t *p, const char *name, const char *def);

#ifdef __cplusplus
}
#endif

#endif /* AURA_SOUNDPROOF_AE_COMMON_H */
