/* Aura — Agent 数据流中间件链
 *
 * 中间件在**stage worker** 里执行（不在 publish 线程），顺序可配：
 *   trace → metrics → log/redact → validate → persist → retry → cancel_gate
 *
 * 每个中间件拿 next 回调；不调用 next 即短路（消息终止），返回错误则计入
 * stage 的 errors。
 */
#ifndef AURA_AGENT_FLOW_FLOW_MIDDLEWARE_H
#define AURA_AGENT_FLOW_FLOW_MIDDLEWARE_H

#include <stdint.h>

#include "agent/flow/flow_types.h"
#include "utils/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aura_flow_bus   aura_flow_bus_t;
typedef struct aura_flow_stage aura_flow_stage_t;

typedef aura_err_t (*aura_flow_next_fn)(aura_flow_msg_t *msg, void *user);

/* 中间件拿到的 user 是*本次调用*的上下文，不是注册时的私有 user。
 * 取注册 user：aura_flow_mw_user(user)。 */
typedef struct aura_flow_mw_invocation {
    void *user;     /* 注册时传给 aura_flow_mw_add 的 user */
    void *internal; /* 链内部上下文，中间件不要直接碰 */
} aura_flow_mw_invocation_t;

typedef aura_err_t (*aura_flow_mw_fn)(aura_flow_msg_t *msg, aura_flow_next_fn next, void *user);

/* 从中间件回调的 user 里取回注册时的私有 user。 */
void *aura_flow_mw_user(void *invocation);

#define AURA_FLOW_MW_MAX 8

typedef struct aura_flow_mw_entry {
    char             name[AURA_FLOW_NAME_MAX];
    aura_flow_mw_fn  fn;
    void            *user;
} aura_flow_mw_entry_t;

/* 追加中间件（按调用顺序执行）。 */
aura_err_t aura_flow_mw_add(aura_flow_bus_t *bus, const char *name, aura_flow_mw_fn fn,
                            void *user);

uint32_t                          aura_flow_mw_count(const aura_flow_bus_t *bus);
const aura_flow_mw_entry_t       *aura_flow_mw_at(const aura_flow_bus_t *bus, uint32_t idx);

/* 内置中间件。 */
aura_err_t aura_flow_mw_trace(aura_flow_msg_t *msg, aura_flow_next_fn next, void *user);
aura_err_t aura_flow_mw_cancel_gate(aura_flow_msg_t *msg, aura_flow_next_fn next, void *user);
aura_err_t aura_flow_mw_metrics(aura_flow_msg_t *msg, aura_flow_next_fn next, void *user);
aura_err_t aura_flow_mw_log(aura_flow_msg_t *msg, aura_flow_next_fn next, void *user);

/* stage worker 调用：从第 0 个中间件开始，最后落到 stage handler。 */
aura_err_t aura_flow_mw_run(aura_flow_bus_t *bus, aura_flow_stage_t *stage,
                            aura_flow_msg_t *msg);

#ifdef __cplusplus
}
#endif

#endif /* AURA_AGENT_FLOW_FLOW_MIDDLEWARE_H */
