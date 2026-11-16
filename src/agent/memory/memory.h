/* Aura — Agent 记忆
 *
 * 短期：按 (session_id, speaker_id) 隔离的固定容量轮次环，init 期一次性分配；
 * 长期：Phase B 先经 flow_store（内存后端）落盘，Phase D 换 sqlite + 批量刷写。
 *
 * 本模块只提供同步读写；flow 的 memory stage 负责把写请求异步入库。
 */
#ifndef AURA_AGENT_MEMORY_MEMORY_H
#define AURA_AGENT_MEMORY_MEMORY_H

#include <stdint.h>

#include "utils/error.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AURA_MEMORY_SESSION_SLOTS 4
#define AURA_MEMORY_TURNS          8
#define AURA_MEMORY_ROLE_MAX       12
#define AURA_MEMORY_TEXT_MAX       192

typedef struct aura_agent_memory aura_agent_memory_t;

aura_agent_memory_t *aura_agent_memory_create(void);
void                 aura_agent_memory_destroy(aura_agent_memory_t *mem);

/* 追加一条轮次记录（role: "user" / "assistant"）。满时覆盖最旧的一条。 */
aura_err_t aura_agent_memory_add(aura_agent_memory_t *mem, uint32_t session_id,
                                 uint32_t speaker_id, const char *role, const char *text);

/* 取最近轮次，按 "role: text\n" 拼接（从旧到新）。无记录返回 NOT_FOUND。 */
aura_err_t aura_agent_memory_recent(const aura_agent_memory_t *mem, uint32_t session_id,
                                    uint32_t speaker_id, char *out, uint32_t cap);

/* 清空某会话所有说话人的短期记忆。 */
void aura_agent_memory_clear_session(aura_agent_memory_t *mem, uint32_t session_id);

#ifdef __cplusplus
}
#endif

#endif /* AURA_AGENT_MEMORY_MEMORY_H */
