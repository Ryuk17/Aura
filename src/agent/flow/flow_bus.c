/* Aura — Agent 数据流总线（实现） */
#include "agent/flow/flow_bus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent/flow/flow_internal.h"
#include "utils/logger/logger.h"

#define TAG "flow_bus"

aura_flow_bus_t *aura_flow_bus_create(uint32_t default_stage_depth,
                                      aura_flow_policy_t default_policy)
{
    aura_flow_bus_t *bus = (aura_flow_bus_t *)calloc(1, sizeof(*bus));
    if (bus == NULL) {
        return NULL;
    }
    bus->lock = aura_osal_mutex_create();
    if (bus->lock == NULL) {
        free(bus);
        return NULL;
    }
    bus->default_depth  = (default_stage_depth > 0) ? default_stage_depth : 32;
    bus->default_policy = default_policy;
    return bus;
}

void aura_flow_bus_destroy(aura_flow_bus_t *bus)
{
    if (bus == NULL) {
        return;
    }
    if (bus->lock != NULL) {
        aura_osal_mutex_destroy(bus->lock);
    }
    free(bus);
}

static int topic_slot_locked(aura_flow_bus_t *bus, const char *topic)
{
    for (uint32_t i = 0; i < AURA_FLOW_MAX_TOPICS; i++) {
        if (bus->topics[i].used && strcmp(bus->topics[i].topic, topic) == 0) {
            return (int)i;
        }
    }
    for (uint32_t i = 0; i < AURA_FLOW_MAX_TOPICS; i++) {
        if (!bus->topics[i].used) {
            bus->topics[i].used = true;
            snprintf(bus->topics[i].topic, sizeof(bus->topics[i].topic), "%s", topic);
            bus->topics[i].seq = 0;
            return (int)i;
        }
    }
    return -1;
}

aura_err_t aura_flow_publish(aura_flow_bus_t *bus, const aura_flow_msg_t *msg)
{
    if (bus == NULL || msg == NULL) {
        return AURA_ERR_INVALID_ARG;
    }

    aura_flow_msg_t m = *msg;
    if (m.topic[0] == '\0') {
        snprintf(m.topic, sizeof(m.topic), "%s", "event.generic");
    }
    m.ts_us = aura_osal_time_us();

    aura_flow_stage_t *targets[AURA_FLOW_MAX_SUBS];
    uint32_t           target_count = 0;

    aura_osal_mutex_lock(bus->lock);
    int slot = topic_slot_locked(bus, m.topic);
    if (slot >= 0) {
        m.seq = ++bus->topics[slot].seq;
    } else {
        m.seq = 0;
    }
    for (uint32_t i = 0; i < AURA_FLOW_MAX_SUBS; i++) {
        if (!bus->subs[i].used) {
            continue;
        }
        if (bus->subs[i].topic[0] == '\0' || strcmp(bus->subs[i].topic, m.topic) == 0) {
            targets[target_count++] = bus->subs[i].stage;
        }
    }
    bus->stats.published++;
    if (target_count == 0) {
        bus->stats.dropped++;
    }
    aura_osal_mutex_unlock(bus->lock);

    aura_err_t rc = AURA_OK;
    for (uint32_t i = 0; i < target_count; i++) {
        aura_err_t er = aura_flow_stage_enqueue(targets[i], &m);
        if (er != AURA_OK) {
            rc = er;
            aura_osal_mutex_lock(bus->lock);
            bus->stats.dropped++;
            aura_osal_mutex_unlock(bus->lock);
        }
    }
    return rc;
}

aura_err_t aura_flow_subscribe(aura_flow_bus_t *bus, const char *topic, aura_flow_stage_t *stage)
{
    if (bus == NULL || stage == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    aura_osal_mutex_lock(bus->lock);
    for (uint32_t i = 0; i < AURA_FLOW_MAX_SUBS; i++) {
        if (bus->subs[i].used && bus->subs[i].stage == stage &&
            strcmp(bus->subs[i].topic, topic ? topic : "") == 0) {
            aura_osal_mutex_unlock(bus->lock);
            return AURA_ERR_EXIST;
        }
    }
    for (uint32_t i = 0; i < AURA_FLOW_MAX_SUBS; i++) {
        if (!bus->subs[i].used) {
            bus->subs[i].used = true;
            bus->subs[i].stage = stage;
            snprintf(bus->subs[i].topic, sizeof(bus->subs[i].topic), "%s", topic ? topic : "");
            aura_osal_mutex_unlock(bus->lock);
            return AURA_OK;
        }
    }
    aura_osal_mutex_unlock(bus->lock);
    return AURA_ERR_FULL;
}

aura_err_t aura_flow_cancel_turn(aura_flow_bus_t *bus, uint32_t session_id, uint32_t turn_id)
{
    if (bus == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    aura_osal_mutex_lock(bus->lock);
    bus->cancelled_session = session_id;
    bus->cancelled_turn    = turn_id;
    aura_osal_mutex_unlock(bus->lock);
    return AURA_OK;
}

bool aura_flow_is_cancelled(const aura_flow_bus_t *bus, const aura_flow_ctx_t *ctx)
{
    if (bus == NULL || ctx == NULL) {
        return false;
    }
    aura_flow_bus_t *b = (aura_flow_bus_t *)bus;
    bool cancelled = false;
    aura_osal_mutex_lock(b->lock);
    if (b->cancelled_session != 0 && b->cancelled_session == ctx->session_id &&
        (b->cancelled_turn == 0 || b->cancelled_turn == ctx->turn_id)) {
        cancelled = true;
    }
    aura_osal_mutex_unlock(b->lock);
    return cancelled;
}

void aura_flow_stats(const aura_flow_bus_t *bus, aura_flow_stats_t *out)
{
    if (bus == NULL || out == NULL) {
        return;
    }
    aura_flow_bus_t *b = (aura_flow_bus_t *)bus;
    aura_osal_mutex_lock(b->lock);
    *out = b->stats;
    aura_osal_mutex_unlock(b->lock);
}

void aura_flow_note_dropped(aura_flow_bus_t *bus)
{
    if (bus == NULL) {
        return;
    }
    aura_osal_mutex_lock(bus->lock);
    bus->stats.dropped++;
    aura_osal_mutex_unlock(bus->lock);
}

void aura_flow_note_cancelled(aura_flow_bus_t *bus)
{
    if (bus == NULL) {
        return;
    }
    aura_osal_mutex_lock(bus->lock);
    bus->stats.cancelled++;
    aura_osal_mutex_unlock(bus->lock);
}

void aura_flow_note_error(aura_flow_bus_t *bus)
{
    if (bus == NULL) {
        return;
    }
    aura_osal_mutex_lock(bus->lock);
    bus->stats.errors++;
    aura_osal_mutex_unlock(bus->lock);
}

const char *aura_flow_kind_name(aura_flow_kind_t kind)
{
    switch (kind) {
    case AURA_FLOW_ASR_PARTIAL: return "asr.partial";
    case AURA_FLOW_ASR_FINAL: return "asr.final";
    case AURA_FLOW_TEXT: return "text";
    case AURA_FLOW_TURN: return "turn";
    case AURA_FLOW_TOOL_CALL: return "tool.call";
    case AURA_FLOW_TOOL_RESULT: return "tool.result";
    case AURA_FLOW_MEMORY_OP: return "memory.op";
    case AURA_FLOW_CONTROL: return "control";
    case AURA_FLOW_EVENT: return "event";
    case AURA_FLOW_ERROR: return "error";
    case AURA_FLOW_METRIC: return "metric";
    default: return "unknown";
    }
}

const char *aura_flow_policy_name(aura_flow_policy_t policy)
{
    switch (policy) {
    case AURA_FLOW_DROP_OLDEST: return "drop_oldest";
    case AURA_FLOW_DROP_NEWEST: return "drop_newest";
    case AURA_FLOW_BLOCK_TIMEOUT: return "block_timeout";
    case AURA_FLOW_SPILL: return "spill";
    default: return "unknown";
    }
}
