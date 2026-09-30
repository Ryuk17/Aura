#include "core/state_machine/state_machine.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils/logger/logger.h"

#define TAG "sm"

/* ------------------------------------------------------------ 迁移表（数据）
 *
 * 用表而非 if-else 的目的：迁移规则可被测试穷举、可被打印、可被评审。
 * 未列出的 (状态, 触发器) 组合一律视为非法迁移。
 */
static const aura_sm_transition_t k_transitions[] = {
    {AURA_STATE_IDLE, AURA_TRIG_START, AURA_STATE_IDLE, "start"},

    /* 唤醒：KWS 命中 → Listening；声纹校验在 Listening 内完成，
     * 失败直接退回 Idle（不进 ASR，省算力，todo.md 4.7）。 */
    {AURA_STATE_IDLE, AURA_TRIG_KWS_HIT, AURA_STATE_LISTENING, "kws hit"},
    {AURA_STATE_LISTENING, AURA_TRIG_VOICEPRINT_FAIL, AURA_STATE_IDLE, "voiceprint reject"},
    {AURA_STATE_LISTENING, AURA_TRIG_VOICEPRINT_OK, AURA_STATE_LISTENING, "voiceprint accept"},

    {AURA_STATE_LISTENING, AURA_TRIG_SILENCE_TIMEOUT, AURA_STATE_IDLE, "silence timeout"},
    {AURA_STATE_LISTENING, AURA_TRIG_ASR_FINAL, AURA_STATE_THINKING, "asr final"},

    /* Thinking：只等 token 流；有输出即进入 Speaking（首包优先）。 */
    {AURA_STATE_THINKING, AURA_TRIG_TTS_START, AURA_STATE_SPEAKING, "tts first chunk"},
    {AURA_STATE_THINKING, AURA_TRIG_SILENCE_TIMEOUT, AURA_STATE_IDLE, "llm timeout/no speech"},

    /* Speaking：TTS 播放中。打断落回 Listening（不是 Interrupted 状态）。
     * 播完则回 Listening 等下一轮（静音超时再回 Idle，见 turn_taking）。 */
    {AURA_STATE_SPEAKING, AURA_TRIG_TTS_DONE, AURA_STATE_LISTENING, "tts done → await next turn"},
    {AURA_STATE_SPEAKING, AURA_TRIG_BARGE_IN, AURA_STATE_LISTENING, "barge-in accepted"},

    /* 任意状态异常 → Error（由 handle 统一兜底，不必逐状态列出）。 */
    {AURA_STATE_ERROR, AURA_TRIG_RESET, AURA_STATE_IDLE, "auto/manual reset"},
};

#define TRANSITION_COUNT ((uint32_t)(sizeof(k_transitions) / sizeof(k_transitions[0])))

uint32_t aura_sm_transition_count(void)
{
    return TRANSITION_COUNT;
}

const aura_sm_transition_t *aura_sm_transition_table(void)
{
    return k_transitions;
}

static const char *const state_names[AURA_STATE_COUNT] = {
    "Idle", "Listening", "Thinking", "Speaking", "Error",
};

static const char *const trigger_names[AURA_TRIG_COUNT] = {
    "none",           "start",         "kws_hit",      "voiceprint_ok", "voiceprint_fail",
    "asr_final",      "tts_start",     "tts_done",     "barge_in",      "silence_timeout",
    "error",          "reset",
};

const char *aura_state_name(aura_state_t state)
{
    if (state < 0 || state >= AURA_STATE_COUNT) {
        return "?";
    }
    return state_names[state];
}

const char *aura_trigger_name(aura_trigger_t trig)
{
    if (trig < 0 || trig >= AURA_TRIG_COUNT) {
        return "?";
    }
    return trigger_names[trig];
}

struct aura_sm {
    aura_state_t       state;
    aura_sm_cfg_t      cfg;
    aura_event_bus_t  *bus;
    aura_mutex_t      *lock;
    aura_sm_change_fn  change_cb;
    void              *change_user;
    aura_timer_t      *reset_timer;
    bool               reset_timer_armed;
    aura_sm_stats_t    stats;
};

aura_sm_t *aura_sm_create(const aura_sm_cfg_t *cfg, aura_event_bus_t *bus)
{
    aura_sm_t *sm = (aura_sm_t *)calloc(1, sizeof(*sm));
    if (sm == NULL) {
        return NULL;
    }
    sm->lock = aura_osal_mutex_create();
    if (sm->lock == NULL) {
        free(sm);
        return NULL;
    }
    sm->state = AURA_STATE_IDLE;
    sm->bus   = bus;
    if (cfg != NULL) {
        sm->cfg = *cfg;
    }
    return sm;
}

static void auto_reset_fired(void *arg);

void aura_sm_destroy(aura_sm_t *sm)
{
    if (sm == NULL) {
        return;
    }
    if (sm->reset_timer != NULL) {
        aura_osal_timer_destroy(sm->reset_timer);
        sm->reset_timer = NULL;
    }
    if (sm->lock != NULL) {
        aura_osal_mutex_destroy(sm->lock);
    }
    free(sm);
}

void aura_sm_set_change_cb(aura_sm_t *sm, aura_sm_change_fn cb, void *user)
{
    if (sm == NULL) {
        return;
    }
    aura_osal_mutex_lock(sm->lock);
    sm->change_cb   = cb;
    sm->change_user = user;
    aura_osal_mutex_unlock(sm->lock);
}

aura_state_t aura_sm_state(const aura_sm_t *sm)
{
    return (sm == NULL) ? AURA_STATE_ERROR : sm->state;
}

static void auto_reset_fired(void *arg)
{
    aura_sm_t *sm = (aura_sm_t *)arg;
    AURA_LOGW(TAG, "auto reset from Error -> Idle");
    aura_osal_mutex_lock(sm->lock);
    sm->stats.auto_resets++;
    sm->reset_timer_armed = false;
    aura_osal_mutex_unlock(sm->lock);
    (void)aura_sm_handle(sm, AURA_TRIG_RESET);
}

static void arm_auto_reset(aura_sm_t *sm)
{
    uint32_t delay = (sm->cfg.auto_reset_ms > 0) ? sm->cfg.auto_reset_ms : 1000u;
    if (sm->reset_timer == NULL) {
        sm->reset_timer = aura_osal_timer_create("smrst", delay, false, auto_reset_fired, sm);
    }
    if (sm->reset_timer != NULL) {
        aura_osal_timer_start(sm->reset_timer);
        sm->reset_timer_armed = true;
    }
}

aura_err_t aura_sm_handle(aura_sm_t *sm, aura_trigger_t trig)
{
    if (sm == NULL || trig <= AURA_TRIG_NONE || trig >= AURA_TRIG_COUNT) {
        return AURA_ERR_INVALID_ARG;
    }

    aura_osal_mutex_lock(sm->lock);
    aura_state_t from = sm->state;

    /* 任意非 Error 状态收到 ERROR → 进 Error（由代码兜底，避免表里写 N 行）。 */
    aura_state_t to = from;
    const char  *desc = NULL;
    bool         found = false;

    if (trig == AURA_TRIG_ERROR && from != AURA_STATE_ERROR) {
        to    = AURA_STATE_ERROR;
        desc  = "error";
        found = true;
    } else {
        for (uint32_t i = 0; i < TRANSITION_COUNT; i++) {
            if (k_transitions[i].from == from && k_transitions[i].trig == trig) {
                to    = k_transitions[i].to;
                desc  = k_transitions[i].desc;
                found = true;
                break;
            }
        }
    }

    if (!found) {
        sm->stats.rejected++;
        aura_osal_mutex_unlock(sm->lock);
        AURA_LOGD(TAG, "reject %s in %s", aura_trigger_name(trig), aura_state_name(from));
        return AURA_ERR_STATE;
    }

    aura_sm_change_fn cb   = sm->change_cb;
    void             *user = sm->change_user;
    bool              changed = (to != from);

    if (changed) {
        sm->state = to;
        sm->stats.transitions++;
        if (to == AURA_STATE_ERROR) {
            sm->stats.errors++;
        }
    }
    aura_osal_mutex_unlock(sm->lock);

    if (changed) {
        AURA_LOGI(TAG, "state %s -> %s (%s)", aura_state_name(from), aura_state_name(to),
                  (desc != NULL) ? desc : aura_trigger_name(trig));
        if (sm->bus != NULL && sm->cfg.publish_events) {
            aura_event_t ev;
            memset(&ev, 0, sizeof(ev));
            ev.type        = AURA_EVENT_STATE_CHANGED;
            ev.code        = (int32_t)to;
            ev.u.state.from = (int32_t)from;
            ev.u.state.to   = (int32_t)to;
            snprintf(ev.payload, sizeof(ev.payload), "%s->%s", aura_state_name(from),
                     aura_state_name(to));
            aura_event_bus_publish(sm->bus, &ev);
        }
        if (cb != NULL) {
            cb(sm, from, to, trig, user);
        }
        if (to == AURA_STATE_ERROR && sm->cfg.auto_reset) {
            arm_auto_reset(sm);
        }
    }
    return AURA_OK;
}

aura_trigger_t aura_sm_trigger_from_event(const aura_event_t *event)
{
    if (event == NULL) {
        return AURA_TRIG_NONE;
    }
    switch (event->type) {
    case AURA_EVENT_KWS_HIT:
        return AURA_TRIG_KWS_HIT;
    case AURA_EVENT_VOICEPRINT_RESULT:
        return event->u.voiceprint.passed ? AURA_TRIG_VOICEPRINT_OK : AURA_TRIG_VOICEPRINT_FAIL;
    case AURA_EVENT_ASR_FINAL:
        return AURA_TRIG_ASR_FINAL;
    case AURA_EVENT_TTS_FIRST_CHUNK:
    case AURA_EVENT_PLAYBACK_START:
        return AURA_TRIG_TTS_START;
    case AURA_EVENT_TTS_DONE:
    case AURA_EVENT_PLAYBACK_STOP:
        return AURA_TRIG_TTS_DONE;
    case AURA_EVENT_BARGE_IN:
        return AURA_TRIG_BARGE_IN;
    case AURA_EVENT_SILENCE_TIMEOUT:
        return AURA_TRIG_SILENCE_TIMEOUT;
    case AURA_EVENT_ERROR:
        return AURA_TRIG_ERROR;
    default:
        return AURA_TRIG_NONE;
    }
}

void aura_sm_stats(const aura_sm_t *sm, aura_sm_stats_t *out)
{
    if (sm == NULL || out == NULL) {
        return;
    }
    aura_sm_t *m = (aura_sm_t *)sm;
    aura_osal_mutex_lock(m->lock);
    *out = m->stats;
    aura_osal_mutex_unlock(m->lock);
}
