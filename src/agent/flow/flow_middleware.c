/* Aura — Agent 数据流中间件链（实现） */
#include "agent/flow/flow_middleware.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "agent/flow/flow_internal.h"
#include "agent/flow/flow_store.h"
#include "osal/osal.h"
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

aura_err_t aura_flow_mw_validate(aura_flow_msg_t *msg, aura_flow_next_fn next, void *user)
{
    if (msg == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    if (msg->topic[0] == '\0' || msg->len >= sizeof(msg->payload) ||
        msg->kind > (uint32_t)AURA_FLOW_METRIC) {
        AURA_LOGW("flow", "drop malformed msg: topic='%s' kind=%u len=%u", msg->topic,
                  msg->kind, msg->len);
        return AURA_ERR_INVALID_ARG;
    }
    return next(msg, user);
}

aura_err_t aura_flow_mw_deadline(aura_flow_msg_t *msg, aura_flow_next_fn next, void *user)
{
    if (msg == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    if (msg->ctx.deadline_us != 0 && aura_osal_time_us() > msg->ctx.deadline_us) {
        AURA_LOGD("flow", "drop expired msg: topic=%s deadline=%llu", msg->topic,
                  (unsigned long long)msg->ctx.deadline_us);
        return AURA_ERR_ABORTED;
    }
    return next(msg, user);
}

static void redact_key(char *s, const char *key)
{
    size_t klen = strlen(key);
    char  *p    = s;
    while ((p = strstr(p, key)) != NULL) {
        char *v = p + klen;
        while (*v != '\0' && *v != ' ' && *v != '\n' && *v != '\r' && *v != '&') {
            *v++ = '*';
        }
        p = v;
    }
}

static void redact_phone_numbers(char *s)
{
    size_t n = strlen(s);
    for (size_t i = 0; i < n;) {
        if (!isdigit((unsigned char)s[i])) {
            i++;
            continue;
        }
        size_t j = i;
        while (j < n && isdigit((unsigned char)s[j])) {
            j++;
        }
        if (j - i == 11) {
            for (size_t k = i + 3; k < i + 7; k++) {
                s[k] = '*';
            }
        }
        i = j;
    }
}

aura_err_t aura_flow_mw_redact(aura_flow_msg_t *msg, aura_flow_next_fn next, void *user)
{
    if (msg == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    char buf[AURA_FLOW_PAYLOAD_MAX + 1];
    snprintf(buf, sizeof(buf), "%.*s", (int)(sizeof(buf) - 1), msg->payload);
    redact_key(buf, "password=");
    redact_key(buf, "passwd=");
    redact_key(buf, "token=");
    redact_key(buf, "secret=");
    redact_phone_numbers(buf);
    if (strncmp(buf, msg->payload, sizeof(msg->payload)) != 0) {
        snprintf(msg->payload, sizeof(msg->payload), "%s", buf);
        msg->len = (uint32_t)strlen(msg->payload);
    }
    return next(msg, user);
}

aura_err_t aura_flow_mw_persist(aura_flow_msg_t *msg, aura_flow_next_fn next, void *user)
{
    aura_flow_store_t *store = (aura_flow_store_t *)aura_flow_mw_user(user);
    if (msg == NULL || store == NULL || msg->ctx.session_id == 0) {
        return next(msg, user);
    }
    if (strcmp(msg->topic, "asr.final") != 0 && strcmp(msg->topic, "tts.say") != 0 &&
        strcmp(msg->topic, "dialogue.intent") != 0 && strcmp(msg->topic, "tool.result") != 0) {
        return next(msg, user);
    }
    if (msg->len >= AURA_FLOW_STORE_VALUE_MAX) {
        return next(msg, user);
    }
    /* 每个 topic 在会话内保留最近一条；Phase D 换成追加日志 + 批量落盘。 */
    char key[AURA_FLOW_STORE_KEY_MAX];
    snprintf(key, sizeof(key), "%.*s", (int)(sizeof(key) - 1), msg->topic);
    (void)aura_flow_store_put(store, msg->ctx.session_id, key, msg->payload);
    return next(msg, user);
}

aura_err_t aura_flow_mw_retry(aura_flow_msg_t *msg, aura_flow_next_fn next, void *user)
{
    if (msg == NULL) {
        return next(msg, user);
    }
    aura_err_t rc = next(msg, user);
    if (rc != AURA_ERR_AGAIN) {
        return rc;
    }

    aura_flow_mw_invocation_t *inv = (aura_flow_mw_invocation_t *)user;
    mw_ctx_t                  *ctx = (mw_ctx_t *)((inv != NULL) ? inv->internal : NULL);
    aura_flow_retry_cfg_t     *cfg = (aura_flow_retry_cfg_t *)aura_flow_mw_user(user);
    if (ctx == NULL || cfg == NULL || msg->attempt >= cfg->max_retries) {
        return rc;
    }
    if ((msg->flags & AURA_FLOW_ABORT) != 0 || aura_flow_is_cancelled(ctx->bus, &msg->ctx)) {
        return rc;
    }

    msg->attempt++;
    msg->flags |= AURA_FLOW_RETRY;
    aura_err_t rr = aura_flow_publish(ctx->bus, msg);
    return (rr == AURA_OK) ? AURA_OK : rc;
}
