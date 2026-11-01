/* Aura — 声纹节点（注册入口 + SET_CONFIG 载荷）
 *
 * NN 算法族由上层在 agent init 之前注册（与 kws/asr 同向）。
 * 链路写法：chain = ..., voiceprint（OBSERVER，挂主线旁路）。
 * 注册/删除/清库走 aura_pipeline_control_node(SET_CONFIG, &cfg)，
 * cfg 结构见下 —— agent 对外 API（agent_export.h）在 M4 包这一层。
 */
#ifndef AURA_VOICEPRINT_AURA_VOICEPRINT_H
#define AURA_VOICEPRINT_AURA_VOICEPRINT_H

#include <stdint.h>

#include "utils/error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 注册 "voiceprint" 算法到全局注册表。幂等：已注册时直接返回 AURA_OK。 */
aura_err_t aura_voiceprint_register(void);

/* SET_CONFIG 载荷。注册（ENROLL）已**离线化**：tools/voiceprint_enroll 生成
 * 库文件，节点初始化时读 —— 这里只保留运行期的库管理（内存态）。 */
enum {
    AURA_VP_OP_REMOVE = 0, /* 删除说话人（内存态） */
    AURA_VP_OP_CLEAR,      /* 清库（内存态） */
};

typedef struct {
    int32_t     op;
    const char *spk_id; /* REMOVE 用 */
} aura_vp_set_config_t;

#ifdef __cplusplus
}
#endif

#endif /* AURA_VOICEPRINT_AURA_VOICEPRINT_H */
