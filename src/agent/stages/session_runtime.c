/* Aura — 会话/轮次运行时状态（实现） */
#include "agent/stages/session_runtime.h"

#include <stdlib.h>
#include <string.h>

#include "osal/osal.h"
#include "utils/logger/logger.h"

#define TAG "session_rt"

struct aura_session_runtime {
    aura_mutex_t *lock;
    uint32_t      next_session;
    uint32_t      session_id;
    uint32_t      turn_id;
    uint32_t      speaker_id;
    bool          awake;
    bool          turn_active;
    bool          voiceprint_ok;
    int32_t       voiceprint_score;
    uint64_t      session_started_us;
    uint64_t      turn_started_us;
};

static uint64_t now_us(void)
{
    return aura_osal_time_us();
}

static void fill_ctx_locked(const aura_session_runtime_t *rt, aura_flow_ctx_t *ctx)
{
    if (ctx == NULL) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->session_id   = rt->session_id;
    ctx->turn_id      = rt->turn_id;
    ctx->speaker_id   = rt->speaker_id;
    ctx->start_pts_us = rt->turn_started_us;
}

aura_session_runtime_t *aura_session_runtime_create(void)
{
    aura_session_runtime_t *rt = (aura_session_runtime_t *)calloc(1, sizeof(*rt));
    if (rt == NULL) {
        return NULL;
    }
    rt->lock = aura_osal_mutex_create();
    if (rt->lock == NULL) {
        free(rt);
        return NULL;
    }
    return rt;
}

void aura_session_runtime_destroy(aura_session_runtime_t *rt)
{
    if (rt == NULL) {
        return;
    }
    if (rt->lock != NULL) {
        aura_osal_mutex_destroy(rt->lock);
    }
    free(rt);
}

bool aura_session_runtime_on_wake(aura_session_runtime_t *rt, uint32_t *session_id, bool *is_new)
{
    if (rt == NULL) {
        return false;
    }

    bool is_new_session = false;
    aura_osal_mutex_lock(rt->lock);
    if (!rt->awake || rt->session_id == 0) {
        rt->session_id       = ++rt->next_session;
        rt->turn_id          = 0;
        rt->speaker_id       = 0;
        rt->voiceprint_ok    = false;
        rt->voiceprint_score = 0;
        rt->turn_active      = false;
        rt->session_started_us = now_us();
        rt->turn_started_us    = 0;
        is_new_session         = true;
    }
    rt->awake = true;
    uint32_t sid = rt->session_id;
    aura_osal_mutex_unlock(rt->lock);

    if (session_id != NULL) {
        *session_id = sid;
    }
    if (is_new != NULL) {
        *is_new = is_new_session;
    }
    if (is_new_session) {
        AURA_LOGI(TAG, "session %u started", sid);
    }
    return true;
}

bool aura_session_runtime_on_voiceprint(aura_session_runtime_t *rt, bool passed, int32_t score,
                                        uint32_t *speaker_id)
{
    if (rt == NULL) {
        return false;
    }

    aura_osal_mutex_lock(rt->lock);
    rt->voiceprint_ok    = passed;
    rt->voiceprint_score = score;
    /* 当前事件只有 passed/score，没有身份 ID：通过即记为默认用户 1。 */
    rt->speaker_id = passed ? 1u : 0u;
    uint32_t sid   = rt->speaker_id;
    aura_osal_mutex_unlock(rt->lock);

    if (speaker_id != NULL) {
        *speaker_id = sid;
    }
    return true;
}

bool aura_session_runtime_begin_turn(aura_session_runtime_t *rt, aura_flow_ctx_t *ctx)
{
    if (rt == NULL) {
        return false;
    }

    bool ok = false;
    aura_osal_mutex_lock(rt->lock);
    if (rt->awake && rt->session_id != 0) {
        rt->turn_id++;
        rt->turn_active     = true;
        rt->turn_started_us = now_us();
        fill_ctx_locked(rt, ctx);
        ok = true;
    }
    aura_osal_mutex_unlock(rt->lock);
    return ok;
}

bool aura_session_runtime_ensure_turn(aura_session_runtime_t *rt, aura_flow_ctx_t *ctx)
{
    if (rt == NULL) {
        return false;
    }

    bool ok = false;
    aura_osal_mutex_lock(rt->lock);
    if (rt->awake && rt->session_id != 0) {
        if (!rt->turn_active) {
            rt->turn_id++;
            rt->turn_active     = true;
            rt->turn_started_us = now_us();
        }
        fill_ctx_locked(rt, ctx);
        ok = true;
    }
    aura_osal_mutex_unlock(rt->lock);
    return ok;
}

bool aura_session_runtime_current(aura_session_runtime_t *rt, aura_flow_ctx_t *ctx)
{
    if (rt == NULL) {
        return false;
    }

    bool ok = false;
    aura_osal_mutex_lock(rt->lock);
    if (rt->session_id != 0) {
        fill_ctx_locked(rt, ctx);
        ok = true;
    }
    aura_osal_mutex_unlock(rt->lock);
    return ok;
}

void aura_session_runtime_on_idle(aura_session_runtime_t *rt)
{
    if (rt == NULL) {
        return;
    }
    aura_osal_mutex_lock(rt->lock);
    if (rt->awake) {
        AURA_LOGI(TAG, "session %u closed", rt->session_id);
    }
    rt->awake       = false;
    rt->turn_active = false;
    aura_osal_mutex_unlock(rt->lock);
}

bool aura_session_runtime_is_awake(aura_session_runtime_t *rt)
{
    if (rt == NULL) {
        return false;
    }
    aura_osal_mutex_lock(rt->lock);
    bool awake = rt->awake;
    aura_osal_mutex_unlock(rt->lock);
    return awake;
}

uint32_t aura_session_runtime_session_id(aura_session_runtime_t *rt)
{
    if (rt == NULL) {
        return 0;
    }
    aura_osal_mutex_lock(rt->lock);
    uint32_t sid = rt->session_id;
    aura_osal_mutex_unlock(rt->lock);
    return sid;
}

bool aura_session_runtime_on_barge_in(aura_session_runtime_t *rt, aura_flow_ctx_t *ctx)
{
    if (rt == NULL) {
        return false;
    }

    bool ok = false;
    aura_osal_mutex_lock(rt->lock);
    if (rt->awake && rt->session_id != 0 && rt->turn_active) {
        fill_ctx_locked(rt, ctx);
        rt->turn_active = false;
        ok              = true;
    }
    aura_osal_mutex_unlock(rt->lock);

    if (ok) {
        AURA_LOGI(TAG, "session %u turn %u barged in", ctx->session_id, ctx->turn_id);
    }
    return ok;
}
