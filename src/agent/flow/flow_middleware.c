/* Aura — Agent 数据流中间件链（实现） */
#include "agent/flow/flow_middleware.h"

#include <stdio.h>
#include <string.h>

#include "agent/flow/flow_internal.h"
#include "utils/logger/logger.h"

typedef struct mw_ctx {
    aura_flow_bus_t   *bus;
    aura_flow_stage_t *stage;
    uint32_t           idx;
} mw_ctx_t;

static aura_err_t mw_invoke(mw_ctx_t *ctx, aura_flow_msg_t *msg);

void *aura_flow_mw_user(void *invocation)
{
    aura_flow_mw_invocation_t *inv = (aura_flow_mw_invocation_t *)invocation;
    return (inv != NULL) ? inv->user : NULL;
}

static aura_err_t mw_next(aura_flow_msg_t *msg, void *user)
{
    aura_flow_mw_invocation_t *inv = (aura_flow_mw_invocation_t *)user;
    if (inv == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    return mw_invoke((mw_ctx_t *)inv->internal, msg);
}

static aura_err_t mw_invoke(mw_ctx_t *ctx, aura_flow_msg_t *msg)
{
    if (ctx->idx >= ctx->bus->mw_count) {
        return aura_flow_stage_deliver(ctx->stage, msg);
    }
    const aura_flow_mw_entry_t *e = &ctx->bus->mw[ctx->idx];
    ctx->idx++;
    aura_flow_mw_invocation_t inv = {e->user, ctx};
    return e->fn(msg, mw_next, &inv);
}

aura_err_t aura_flow_mw_run(aura_flow_bus_t *bus, aura_flow_stage_t *stage,
                            aura_flow_msg_t *msg)
{
    if (bus == NULL || stage == NULL || msg == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    mw_ctx_t ctx = {bus, stage, 0};
    return mw_invoke(&ctx, msg);
}

aura_err_t aura_flow_mw_add(aura_flow_bus_t *bus, const char *name, aura_flow_mw_fn fn,
                            void *user)
{
    if (bus == NULL || fn == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    if (bus->mw_count >= AURA_FLOW_MW_MAX) {
        return AURA_ERR_FULL;
    }
    aura_flow_mw_entry_t *e = &bus->mw[bus->mw_count++];
    snprintf(e->name, sizeof(e->name), "%s", (name != NULL) ? name : "mw");
    e->fn   = fn;
    e->user = user;
    return AURA_OK;
}

uint32_t aura_flow_mw_count(const aura_flow_bus_t *bus)
{
    return (bus != NULL) ? bus->mw_count : 0;
}

const aura_flow_mw_entry_t *aura_flow_mw_at(const aura_flow_bus_t *bus, uint32_t idx)
{
    if (bus == NULL || idx >= bus->mw_count) {
        return NULL;
    }
    return &bus->mw[idx];
}

aura_err_t aura_flow_mw_trace(aura_flow_msg_t *msg, aura_flow_next_fn next, void *user)
{
    if (msg->ctx.trace_id == 0) {
        msg->ctx.trace_id = (uint32_t)(msg->seq ^ msg->ts_us ^ (uint64_t)(uintptr_t)msg);
        if (msg->ctx.trace_id == 0) {
            msg->ctx.trace_id = 1;
        }
    }
    return next(msg, user);
}

aura_err_t aura_flow_mw_cancel_gate(aura_flow_msg_t *msg, aura_flow_next_fn next, void *user)
{
    aura_flow_mw_invocation_t *inv = (aura_flow_mw_invocation_t *)user;
    mw_ctx_t                  *ctx = (mw_ctx_t *)((inv != NULL) ? inv->internal : NULL);
    if (ctx == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    if ((msg->flags & AURA_FLOW_ABORT) != 0 || aura_flow_is_cancelled(ctx->bus, &msg->ctx)) {
        return AURA_ERR_ABORTED;
    }
    return next(msg, user);
}

aura_err_t aura_flow_mw_metrics(aura_flow_msg_t *msg, aura_flow_next_fn next, void *user)
{
    (void)msg;
    return next(msg, user);
}

aura_err_t aura_flow_mw_log(aura_flow_msg_t *msg, aura_flow_next_fn next, void *user)
{
    AURA_LOGD("flow", "topic=%s kind=%s seq=%llu session=%u turn=%u len=%u", msg->topic,
              aura_flow_kind_name((aura_flow_kind_t)msg->kind),
              (unsigned long long)msg->seq, msg->ctx.session_id, msg->ctx.turn_id, msg->len);
    return next(msg, user);
}
