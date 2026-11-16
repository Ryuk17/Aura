/* Aura — Agent 记忆（实现） */
#include "agent/memory/memory.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "osal/osal.h"

typedef struct aura_memory_turn {
    char     role[AURA_MEMORY_ROLE_MAX];
    char     text[AURA_MEMORY_TEXT_MAX];
    uint64_t ts_us;
} aura_memory_turn_t;

typedef struct aura_memory_slot {
    bool            used;
    uint32_t        session_id;
    uint32_t        speaker_id;
    uint32_t        head;
    uint32_t        count;
    uint64_t        last_us;
    aura_memory_turn_t turns[AURA_MEMORY_TURNS];
} aura_memory_slot_t;

struct aura_agent_memory {
    aura_mutex_t        *lock;
    aura_memory_slot_t   slots[AURA_MEMORY_SESSION_SLOTS];
    _Atomic uint32_t     dropped;
};

aura_agent_memory_t *aura_agent_memory_create(void)
{
    aura_agent_memory_t *mem = (aura_agent_memory_t *)calloc(1, sizeof(*mem));
    if (mem == NULL) {
        return NULL;
    }
    mem->lock = aura_osal_mutex_create();
    if (mem->lock == NULL) {
        free(mem);
        return NULL;
    }
    return mem;
}

void aura_agent_memory_destroy(aura_agent_memory_t *mem)
{
    if (mem == NULL) {
        return;
    }
    if (mem->lock != NULL) {
        aura_osal_mutex_destroy(mem->lock);
    }
    free(mem);
}

static aura_memory_slot_t *find_slot(aura_agent_memory_t *mem, uint32_t session_id,
                                     uint32_t speaker_id)
{
    for (uint32_t i = 0; i < AURA_MEMORY_SESSION_SLOTS; i++) {
        if (mem->slots[i].used && mem->slots[i].session_id == session_id &&
            mem->slots[i].speaker_id == speaker_id) {
            return &mem->slots[i];
        }
    }
    return NULL;
}

static aura_memory_slot_t *alloc_slot(aura_agent_memory_t *mem)
{
    for (uint32_t i = 0; i < AURA_MEMORY_SESSION_SLOTS; i++) {
        if (!mem->slots[i].used) {
            mem->slots[i].used         = true;
            mem->slots[i].session_id   = 0;
            mem->slots[i].speaker_id   = 0;
            mem->slots[i].head         = 0;
            mem->slots[i].count        = 0;
            mem->slots[i].last_us      = 0;
            return &mem->slots[i];
        }
    }

    /* 槽位满：淘汰最久未更新的会话，保证唤醒/对话路径永远不因记忆满而失败。 */
    aura_memory_slot_t *oldest = &mem->slots[0];
    for (uint32_t i = 1; i < AURA_MEMORY_SESSION_SLOTS; i++) {
        if (mem->slots[i].last_us < oldest->last_us) {
            oldest = &mem->slots[i];
        }
    }
    atomic_fetch_add(&mem->dropped, 1);
    oldest->head    = 0;
    oldest->count   = 0;
    oldest->last_us = 0;
    return oldest;
}

aura_err_t aura_agent_memory_add(aura_agent_memory_t *mem, uint32_t session_id,
                                 uint32_t speaker_id, const char *role, const char *text)
{
    if (mem == NULL || role == NULL || text == NULL || session_id == 0) {
        return AURA_ERR_INVALID_ARG;
    }

    aura_osal_mutex_lock(mem->lock);
    aura_memory_slot_t *slot = find_slot(mem, session_id, speaker_id);
    if (slot == NULL) {
        slot              = alloc_slot(mem);
        slot->session_id  = session_id;
        slot->speaker_id  = speaker_id;
        slot->head        = 0;
        slot->count       = 0;
    }

    uint32_t idx;
    if (slot->count < AURA_MEMORY_TURNS) {
        idx = (slot->head + slot->count) % AURA_MEMORY_TURNS;
        slot->count++;
    } else {
        idx         = slot->head;
        slot->head  = (slot->head + 1) % AURA_MEMORY_TURNS;
    }

    aura_memory_turn_t *turn = &slot->turns[idx];
    snprintf(turn->role, sizeof(turn->role), "%.*s", (int)(sizeof(turn->role) - 1), role);
    snprintf(turn->text, sizeof(turn->text), "%.*s", (int)(sizeof(turn->text) - 1), text);
    turn->ts_us   = aura_osal_time_us();
    slot->last_us = turn->ts_us;
    aura_osal_mutex_unlock(mem->lock);
    return AURA_OK;
}

aura_err_t aura_agent_memory_recent(const aura_agent_memory_t *mem, uint32_t session_id,
                                    uint32_t speaker_id, char *out, uint32_t cap)
{
    if (mem == NULL || out == NULL || cap == 0 || session_id == 0) {
        return AURA_ERR_INVALID_ARG;
    }

    aura_agent_memory_t *m = (aura_agent_memory_t *)mem;
    aura_osal_mutex_lock(m->lock);
    aura_memory_slot_t *slot = find_slot(m, session_id, speaker_id);
    if (slot == NULL || slot->count == 0) {
        aura_osal_mutex_unlock(m->lock);
        return AURA_ERR_NOT_FOUND;
    }

    out[0]      = '\0';
    uint32_t off = 0;
    for (uint32_t i = 0; i < slot->count; i++) {
        uint32_t idx = (slot->head + i) % AURA_MEMORY_TURNS;
        int      n   = snprintf(out + off, (size_t)(cap - off), "%s: %s\n",
                                slot->turns[idx].role, slot->turns[idx].text);
        if (n < 0 || (uint32_t)n >= cap - off) {
            break;
        }
        off += (uint32_t)n;
        if (off + 1 >= cap) {
            break;
        }
    }
    aura_osal_mutex_unlock(m->lock);
    return (off > 0) ? AURA_OK : AURA_ERR_NOT_FOUND;
}

void aura_agent_memory_clear_session(aura_agent_memory_t *mem, uint32_t session_id)
{
    if (mem == NULL || session_id == 0) {
        return;
    }
    aura_osal_mutex_lock(mem->lock);
    for (uint32_t i = 0; i < AURA_MEMORY_SESSION_SLOTS; i++) {
        if (mem->slots[i].used && mem->slots[i].session_id == session_id) {
            mem->slots[i].used  = false;
            mem->slots[i].count = 0;
            mem->slots[i].head  = 0;
        }
    }
    aura_osal_mutex_unlock(mem->lock);
}
