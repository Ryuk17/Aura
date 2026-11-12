/* Aura — Agent 数据流持久化抽象
 *
 * 当前只有内存后端（会话级 KV）。Phase D 接 sqlite / flash 时保持同一 API：
 * persist 中间件只认识 put/get，不关心后端；所有写走缓冲 + 批量落盘。
 */
#ifndef AURA_AGENT_FLOW_FLOW_STORE_H
#define AURA_AGENT_FLOW_FLOW_STORE_H

#include <stdint.h>

#include "utils/error.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AURA_FLOW_STORE_KEY_MAX   64
#define AURA_FLOW_STORE_VALUE_MAX 512
#define AURA_FLOW_STORE_SLOTS     32

typedef struct aura_flow_store aura_flow_store_t;

aura_flow_store_t *aura_flow_store_create(void);
void               aura_flow_store_destroy(aura_flow_store_t *store);

aura_err_t aura_flow_store_put(aura_flow_store_t *store, uint32_t session_id, const char *key,
                               const char *value);
aura_err_t aura_flow_store_get(const aura_flow_store_t *store, uint32_t session_id,
                               const char *key, char *out, uint32_t cap);
aura_err_t aura_flow_store_erase(aura_flow_store_t *store, uint32_t session_id, const char *key);
void       aura_flow_store_clear_session(aura_flow_store_t *store, uint32_t session_id);

#ifdef __cplusplus
}
#endif

#endif /* AURA_AGENT_FLOW_FLOW_STORE_H */
