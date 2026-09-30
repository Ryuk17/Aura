#include "core/event_bus/event_bus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils/logger/logger.h"

#define TAG "event_bus"

#define AURA_EVENT_MAX_SUBS    16
#define AURA_EVENT_MAX_WAITERS 8

typedef struct {
    uint32_t             mask;
    aura_event_handler_t fn;
    void                *user;
    bool                 used;
} event_sub_t;

typedef struct {
    uint32_t      mask;
    aura_sem_t   *sem;
    aura_event_t  ev;
    bool          used;
} event_waiter_t;

struct aura_event_bus {
    aura_queue_t  *queue;
    aura_task_t   *task;
    aura_mutex_t  *lock;
    volatile bool  running;
    uint32_t       seq;

    event_sub_t    subs[AURA_EVENT_MAX_SUBS];
    event_waiter_t waiters[AURA_EVENT_MAX_WAITERS];
    aura_event_bus_stats_t stats;
};

static const char *const type_names[AURA_EVENT_TYPE_COUNT] = {
    "none",         "state_changed", "kws_hit",        "vad_start",
    "vad_end",      "voiceprint",    "asr_partial",    "asr_final",
    "llm_first",    "llm_token",     "llm_done",       "tts_first",
    "tts_done",     "playback_start", "playback_stop", "barge_in",
    "silence_timeout", "error",      "user",
};

const char *aura_event_type_name(aura_event_type_t type)
{
    if (type < 0 || type >= AURA_EVENT_TYPE_COUNT) {
        return "?";
    }
    return type_names[type];
}

static void event_bus_dispatch(aura_event_bus_t *bus, const aura_event_t *ev)
{
    uint32_t mask = 1u << (uint32_t)ev->type;

    /* 1) 唤醒同步等待者 */
    aura_osal_mutex_lock(bus->lock);
    for (uint32_t i = 0; i < AURA_EVENT_MAX_WAITERS; i++) {
        event_waiter_t *w = &bus->waiters[i];
        if (w->used && (w->mask & mask)) {
            w->ev = *ev;
            aura_osal_sem_post(w->sem);
        }
    }

    /* 2) 收集匹配的订阅者（锁内取引用，锁外回调，避免回调内 subscribe 死锁） */
    aura_event_handler_t fns[AURA_EVENT_MAX_SUBS];
    void                *args[AURA_EVENT_MAX_SUBS];
    uint32_t             n = 0;
    for (uint32_t i = 0; i < AURA_EVENT_MAX_SUBS && n < AURA_EVENT_MAX_SUBS; i++) {
        if (bus->subs[i].used && (bus->subs[i].mask & mask)) {
            fns[n]  = bus->subs[i].fn;
            args[n] = bus->subs[i].user;
            n++;
        }
    }
    bus->stats.dispatched++;
    bus->stats.handler_calls += n;
    aura_osal_mutex_unlock(bus->lock);

    for (uint32_t i = 0; i < n; i++) {
        fns[i](ev, args[i]);
    }
}

static void event_bus_task(void *arg)
{
    aura_event_bus_t *bus = (aura_event_bus_t *)arg;
    aura_event_t ev;
    while (bus->running) {
        if (aura_osal_queue_pop(bus->queue, &ev, 50) != AURA_OK) {
            continue;
        }
        event_bus_dispatch(bus, &ev);
    }
}

aura_event_bus_t *aura_event_bus_create(uint32_t queue_depth)
{
    if (queue_depth == 0) {
        queue_depth = 64;
    }
    aura_event_bus_t *bus = (aura_event_bus_t *)calloc(1, sizeof(*bus));
    if (bus == NULL) {
        return NULL;
    }
    bus->lock  = aura_osal_mutex_create();
    bus->queue = aura_osal_queue_create(queue_depth, (uint32_t)sizeof(aura_event_t));
    if (bus->lock == NULL || bus->queue == NULL) {
        aura_event_bus_destroy(bus);
        return NULL;
    }
    bus->running = true;
    bus->task    = aura_osal_task_create("evbus", 4096, 5, event_bus_task, bus);
    if (bus->task == NULL) {
        bus->running = false;
        aura_event_bus_destroy(bus);
        return NULL;
    }
    return bus;
}

void aura_event_bus_destroy(aura_event_bus_t *bus)
{
    if (bus == NULL) {
        return;
    }
    bus->running = false;
    if (bus->task != NULL) {
        aura_osal_task_destroy(bus->task);
    }
    for (uint32_t i = 0; i < AURA_EVENT_MAX_WAITERS; i++) {
        if (bus->waiters[i].sem != NULL) {
            aura_osal_sem_destroy(bus->waiters[i].sem);
        }
    }
    if (bus->queue != NULL) {
        aura_osal_queue_destroy(bus->queue);
    }
    if (bus->lock != NULL) {
        aura_osal_mutex_destroy(bus->lock);
    }
    free(bus);
}

aura_err_t aura_event_bus_subscribe(aura_event_bus_t *bus, uint32_t type_mask,
                                    aura_event_handler_t handler, void *user)
{
    if (bus == NULL || handler == NULL || type_mask == 0) {
        return AURA_ERR_INVALID_ARG;
    }
    aura_osal_mutex_lock(bus->lock);
    for (uint32_t i = 0; i < AURA_EVENT_MAX_SUBS; i++) {
        if (!bus->subs[i].used) {
            bus->subs[i].used = true;
            bus->subs[i].mask = type_mask;
            bus->subs[i].fn   = handler;
            bus->subs[i].user = user;
            aura_osal_mutex_unlock(bus->lock);
            return AURA_OK;
        }
    }
    aura_osal_mutex_unlock(bus->lock);
    return AURA_ERR_FULL;
}

aura_err_t aura_event_bus_unsubscribe(aura_event_bus_t *bus, aura_event_handler_t handler)
{
    if (bus == NULL || handler == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    aura_osal_mutex_lock(bus->lock);
    for (uint32_t i = 0; i < AURA_EVENT_MAX_SUBS; i++) {
        if (bus->subs[i].used && bus->subs[i].fn == handler) {
            bus->subs[i].used = false;
            bus->subs[i].fn   = NULL;
            bus->subs[i].user = NULL;
            aura_osal_mutex_unlock(bus->lock);
            return AURA_OK;
        }
    }
    aura_osal_mutex_unlock(bus->lock);
    return AURA_ERR_NOT_FOUND;
}

aura_err_t aura_event_bus_publish(aura_event_bus_t *bus, const aura_event_t *event)
{
    if (bus == NULL || event == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    aura_event_t ev = *event;
    if (ev.ts_us == 0) {
        ev.ts_us = aura_osal_time_us();
    }
    aura_osal_mutex_lock(bus->lock);
    ev.seq = ++bus->seq;
    bus->stats.published++;
    aura_osal_mutex_unlock(bus->lock);

    /* 非阻塞：音频线程上不允许因为订阅者慢而卡住。 */
    if (aura_osal_queue_push(bus->queue, &ev, AURA_NO_WAIT) != AURA_OK) {
        aura_osal_mutex_lock(bus->lock);
        bus->stats.dropped++;
        aura_osal_mutex_unlock(bus->lock);
        return AURA_ERR_FULL;
    }
    return AURA_OK;
}

aura_err_t aura_event_bus_post(aura_event_bus_t *bus, aura_event_type_t type, int32_t code,
                               const char *payload)
{
    if (bus == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    aura_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = type;
    ev.code = code;
    ev.ts_us = aura_osal_time_us();
    if (payload != NULL) {
        snprintf(ev.payload, sizeof(ev.payload), "%s", payload);
    }
    return aura_event_bus_publish(bus, &ev);
}

aura_err_t aura_event_bus_wait(aura_event_bus_t *bus, uint32_t type_mask, aura_event_t *out,
                               uint32_t timeout_ms)
{
    if (bus == NULL || out == NULL || type_mask == 0) {
        return AURA_ERR_INVALID_ARG;
    }
    aura_osal_mutex_lock(bus->lock);
    int32_t slot = -1;
    for (uint32_t i = 0; i < AURA_EVENT_MAX_WAITERS; i++) {
        if (!bus->waiters[i].used) {
            slot = (int32_t)i;
            break;
        }
    }
    if (slot < 0) {
        aura_osal_mutex_unlock(bus->lock);
        return AURA_ERR_FULL;
    }
    event_waiter_t *w = &bus->waiters[slot];
    if (w->sem == NULL) {
        w->sem = aura_osal_sem_create(0, 1);
        if (w->sem == NULL) {
            aura_osal_mutex_unlock(bus->lock);
            return AURA_ERR_NOMEM;
        }
    }
    w->mask = type_mask;
    w->used = true;
    aura_osal_mutex_unlock(bus->lock);

    aura_err_t rc = aura_osal_sem_wait(w->sem, timeout_ms);
    if (rc == AURA_OK) {
        *out = w->ev;
    }

    aura_osal_mutex_lock(bus->lock);
    w->used = false;
    w->mask = 0;
    aura_osal_mutex_unlock(bus->lock);
    return rc;
}

uint32_t aura_event_bus_flush(aura_event_bus_t *bus)
{
    if (bus == NULL) {
        return 0;
    }
    aura_event_t ev;
    uint32_t     n = 0;
    while (aura_osal_queue_pop(bus->queue, &ev, AURA_NO_WAIT) == AURA_OK) {
        n++;
    }
    return n;
}

void aura_event_bus_stats(const aura_event_bus_t *bus, aura_event_bus_stats_t *out)
{
    if (bus == NULL || out == NULL) {
        return;
    }
    aura_event_bus_t *m = (aura_event_bus_t *)bus;
    aura_osal_mutex_lock(m->lock);
    *out = m->stats;
    aura_osal_mutex_unlock(m->lock);
}
