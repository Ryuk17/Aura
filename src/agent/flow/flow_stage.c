/* Aura — Agent 数据流 stage（实现） */
#include "agent/flow/flow_stage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent/flow/flow_internal.h"
#include "agent/flow/flow_middleware.h"
#include "utils/logger/logger.h"

#define TAG "flow_stage"

static void stage_worker(void *arg)
{
    aura_flow_stage_t *stage = (aura_flow_stage_t *)arg;
    aura_flow_msg_t   msg;

    while (stage->running) {
        if (aura_osal_queue_pop(stage->q, &msg, 100) != AURA_OK) {
            continue;
        }
        aura_err_t rc = AURA_OK;
        if (stage->bus != NULL) {
            rc = aura_flow_mw_run(stage->bus, stage, &msg);
        } else if (stage->handler != NULL) {
            rc = stage->handler(&msg, stage->user);
        }
        if (rc == AURA_ERR_ABORTED) {
            aura_flow_note_cancelled(stage->bus);
        } else if (rc != AURA_OK) {
            atomic_fetch_add(&stage->errors, 1);
            aura_flow_note_error(stage->bus);
        } else {
            atomic_fetch_add(&stage->delivered, 1);
            aura_osal_mutex_lock(((aura_flow_bus_t *)stage->bus)->lock);
            ((aura_flow_bus_t *)stage->bus)->stats.dispatched++;
            aura_osal_mutex_unlock(((aura_flow_bus_t *)stage->bus)->lock);
        }
    }
}

aura_flow_stage_t *aura_flow_stage_create(aura_flow_bus_t *bus, const char *name,
                                          aura_flow_handler_t handler, void *user,
                                          uint32_t queue_depth, aura_flow_policy_t policy)
{
    if (bus == NULL || handler == NULL) {
        return NULL;
    }
    aura_flow_stage_t *stage = (aura_flow_stage_t *)calloc(1, sizeof(*stage));
    if (stage == NULL) {
        return NULL;
    }
    stage->bus     = bus;
    stage->handler = handler;
    stage->user    = user;
    stage->depth   = (queue_depth > 0) ? queue_depth : bus->default_depth;
    stage->policy  = policy;
    snprintf(stage->name, sizeof(stage->name), "%s", (name != NULL) ? name : "stage");
    stage->q = aura_osal_queue_create(stage->depth, sizeof(aura_flow_msg_t));
    if (stage->q == NULL) {
        free(stage);
        return NULL;
    }
    return stage;
}

static void stage_unsubscribe_all(aura_flow_stage_t *stage)
{
    if (stage->bus == NULL) {
        return;
    }
    aura_osal_mutex_lock(stage->bus->lock);
    for (uint32_t i = 0; i < AURA_FLOW_MAX_SUBS; i++) {
        if (stage->bus->subs[i].used && stage->bus->subs[i].stage == stage) {
            stage->bus->subs[i].used = false;
            stage->bus->subs[i].stage = NULL;
            stage->bus->subs[i].topic[0] = '\0';
        }
    }
    aura_osal_mutex_unlock(stage->bus->lock);
}

void aura_flow_stage_destroy(aura_flow_stage_t *stage)
{
    if (stage == NULL) {
        return;
    }
    (void)aura_flow_stage_stop(stage);
    stage_unsubscribe_all(stage);
    if (stage->q != NULL) {
        aura_osal_queue_destroy(stage->q);
        stage->q = NULL;
    }
    free(stage);
}

aura_err_t aura_flow_stage_subscribe(aura_flow_stage_t *stage, const char *topic)
{
    if (stage == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    return aura_flow_subscribe(stage->bus, topic, stage);
}

aura_err_t aura_flow_stage_start(aura_flow_stage_t *stage)
{
    if (stage == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    if (stage->task != NULL) {
        return AURA_OK;
    }
    stage->running = true;
    stage->task = aura_osal_task_create(stage->name, 16384, 2, stage_worker, stage);
    if (stage->task == NULL) {
        stage->running = false;
        return AURA_ERR_NOMEM;
    }
    return AURA_OK;
}

aura_err_t aura_flow_stage_stop(aura_flow_stage_t *stage)
{
    if (stage == NULL || stage->task == NULL) {
        return AURA_OK;
    }
    stage->running = false;
    (void)aura_osal_task_join(stage->task, 2000);
    aura_osal_task_destroy(stage->task);
    stage->task = NULL;
    return AURA_OK;
}

const char *aura_flow_stage_name(const aura_flow_stage_t *stage)
{
    return (stage != NULL) ? stage->name : "";
}

void aura_flow_stage_stats(const aura_flow_stage_t *stage, aura_flow_stage_stats_t *out)
{
    if (stage == NULL || out == NULL) {
        return;
    }
    out->received  = atomic_load(&stage->received);
    out->dropped   = atomic_load(&stage->dropped);
    out->delivered = atomic_load(&stage->delivered);
    out->errors    = atomic_load(&stage->errors);
}

aura_err_t aura_flow_stage_enqueue(aura_flow_stage_t *stage, const aura_flow_msg_t *msg)
{
    if (stage == NULL || msg == NULL || stage->q == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    atomic_fetch_add(&stage->received, 1);

    if (aura_osal_queue_push(stage->q, msg, AURA_NO_WAIT) == AURA_OK) {
        return AURA_OK;
    }

    switch (stage->policy) {
    case AURA_FLOW_DROP_OLDEST:
    case AURA_FLOW_SPILL: {
        aura_flow_msg_t old;
        (void)aura_osal_queue_pop(stage->q, &old, AURA_NO_WAIT);
        atomic_fetch_add(&stage->dropped, 1);
        return (aura_osal_queue_push(stage->q, msg, AURA_NO_WAIT) == AURA_OK) ? AURA_OK
                                                                              : AURA_ERR_FULL;
    }
    case AURA_FLOW_BLOCK_TIMEOUT:
        /* publish 可能来自音频线程；最多等 5ms，绝不无限阻塞。 */
        if (aura_osal_queue_push(stage->q, msg, 5) == AURA_OK) {
            return AURA_OK;
        }
        atomic_fetch_add(&stage->dropped, 1);
        return AURA_ERR_FULL;
    case AURA_FLOW_DROP_NEWEST:
    default:
        atomic_fetch_add(&stage->dropped, 1);
        return AURA_ERR_FULL;
    }
}

aura_err_t aura_flow_stage_deliver(aura_flow_stage_t *stage, aura_flow_msg_t *msg)
{
    if (stage == NULL || msg == NULL || stage->handler == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    return stage->handler(msg, stage->user);
}
