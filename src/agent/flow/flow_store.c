/* Aura — Agent 数据流持久化抽象（内存后端） */
#include "agent/flow/flow_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "osal/osal.h"

typedef struct store_entry {
    bool     used;
    uint32_t session_id;
    char     key[AURA_FLOW_STORE_KEY_MAX];
    char     value[AURA_FLOW_STORE_VALUE_MAX];
} store_entry_t;

struct aura_flow_store {
    aura_mutex_t *lock;
    store_entry_t slots[AURA_FLOW_STORE_SLOTS];
};

aura_flow_store_t *aura_flow_store_create(void)
{
    aura_flow_store_t *s = (aura_flow_store_t *)calloc(1, sizeof(*s));
    if (s == NULL) {
        return NULL;
    }
    s->lock = aura_osal_mutex_create();
    if (s->lock == NULL) {
        free(s);
        return NULL;
    }
    return s;
}

void aura_flow_store_destroy(aura_flow_store_t *store)
{
    if (store == NULL) {
        return;
    }
    if (store->lock != NULL) {
        aura_osal_mutex_destroy(store->lock);
    }
    free(store);
}

aura_err_t aura_flow_store_put(aura_flow_store_t *store, uint32_t session_id, const char *key,
                               const char *value)
{
    if (store == NULL || key == NULL || value == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    if (strlen(key) >= AURA_FLOW_STORE_KEY_MAX || strlen(value) >= AURA_FLOW_STORE_VALUE_MAX) {
        return AURA_ERR_INVALID_ARG;
    }
    aura_osal_mutex_lock(store->lock);
    int free_slot = -1;
    for (uint32_t i = 0; i < AURA_FLOW_STORE_SLOTS; i++) {
        if (store->slots[i].used && store->slots[i].session_id == session_id &&
            strcmp(store->slots[i].key, key) == 0) {
            snprintf(store->slots[i].value, sizeof(store->slots[i].value), "%s", value);
            aura_osal_mutex_unlock(store->lock);
            return AURA_OK;
        }
        if (!store->slots[i].used && free_slot < 0) {
            free_slot = (int)i;
        }
    }
    if (free_slot < 0) {
        aura_osal_mutex_unlock(store->lock);
        return AURA_ERR_FULL;
    }
    store->slots[free_slot].used = true;
    store->slots[free_slot].session_id = session_id;
    snprintf(store->slots[free_slot].key, sizeof(store->slots[free_slot].key), "%s", key);
    snprintf(store->slots[free_slot].value, sizeof(store->slots[free_slot].value), "%s", value);
    aura_osal_mutex_unlock(store->lock);
    return AURA_OK;
}

aura_err_t aura_flow_store_get(const aura_flow_store_t *store, uint32_t session_id,
                               const char *key, char *out, uint32_t cap)
{
    if (store == NULL || key == NULL || out == NULL || cap == 0) {
        return AURA_ERR_INVALID_ARG;
    }
    aura_flow_store_t *s = (aura_flow_store_t *)store;
    aura_err_t rc = AURA_ERR_NOT_FOUND;
    aura_osal_mutex_lock(s->lock);
    for (uint32_t i = 0; i < AURA_FLOW_STORE_SLOTS; i++) {
        if (s->slots[i].used && s->slots[i].session_id == session_id &&
            strcmp(s->slots[i].key, key) == 0) {
            snprintf(out, cap, "%s", s->slots[i].value);
            rc = AURA_OK;
            break;
        }
    }
    aura_osal_mutex_unlock(s->lock);
    return rc;
}

aura_err_t aura_flow_store_erase(aura_flow_store_t *store, uint32_t session_id, const char *key)
{
    if (store == NULL || key == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    aura_err_t rc = AURA_ERR_NOT_FOUND;
    aura_osal_mutex_lock(store->lock);
    for (uint32_t i = 0; i < AURA_FLOW_STORE_SLOTS; i++) {
        if (store->slots[i].used && store->slots[i].session_id == session_id &&
            strcmp(store->slots[i].key, key) == 0) {
            store->slots[i].used = false;
            rc = AURA_OK;
            break;
        }
    }
    aura_osal_mutex_unlock(store->lock);
    return rc;
}

void aura_flow_store_clear_session(aura_flow_store_t *store, uint32_t session_id)
{
    if (store == NULL) {
        return;
    }
    aura_osal_mutex_lock(store->lock);
    for (uint32_t i = 0; i < AURA_FLOW_STORE_SLOTS; i++) {
        if (store->slots[i].used && store->slots[i].session_id == session_id) {
            store->slots[i].used = false;
        }
    }
    aura_osal_mutex_unlock(store->lock);
}
