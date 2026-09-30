#include "inference/model_manager/model_manager.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils/logger/logger.h"

#define TAG "model_mgr"

typedef struct {
    char               name[48];
    char               path[256];
    aura_infer_form_t  form;
    bool               resident;
    uint32_t           refs;     /* acquire/release 引用计数 */
    uint64_t           weight_bytes;
    uint64_t           runtime_bytes;
    uint32_t           last_used; /* LRU 序号 */
    aura_infer_model_t *model;
} model_entry_t;

struct aura_model_mgr {
    model_entry_t entries[AURA_MODEL_MGR_MAX_MODELS];
    uint32_t      count;
    uint32_t      clock;   /* LRU 时钟 */
    uint64_t      budget_bytes;
    aura_mutex_t *lock;
};

aura_model_mgr_t *aura_model_mgr_create(void)
{
    aura_model_mgr_t *mgr = (aura_model_mgr_t *)calloc(1, sizeof(*mgr));
    if (mgr == NULL) {
        return NULL;
    }
    mgr->lock = aura_osal_mutex_create();
    if (mgr->lock == NULL) {
        free(mgr);
        return NULL;
    }
    return mgr;
}

void aura_model_mgr_destroy(aura_model_mgr_t *mgr)
{
    if (mgr == NULL) {
        return;
    }
    for (uint32_t i = 0; i < mgr->count; i++) {
        if (mgr->entries[i].model != NULL) {
            aura_infer_unload(mgr->entries[i].model);
            mgr->entries[i].model = NULL;
        }
    }
    aura_osal_mutex_destroy(mgr->lock);
    free(mgr);
}

static model_entry_t *find_entry(aura_model_mgr_t *mgr, const char *name)
{
    for (uint32_t i = 0; i < mgr->count; i++) {
        if (strcmp(mgr->entries[i].name, name) == 0) {
            return &mgr->entries[i];
        }
    }
    return NULL;
}

static void unload_entry(aura_model_mgr_t *mgr, model_entry_t *e)
{
    if (e->model != NULL) {
        AURA_LOGI(TAG, "unload %s (%s)", e->name, e->path);
        aura_infer_unload(e->model);
        e->model = NULL;
        e->weight_bytes  = 0;
        e->runtime_bytes = 0;
    }
}

aura_err_t aura_model_mgr_load(aura_model_mgr_t *mgr, const char *name, const char *path,
                               aura_infer_form_t form, bool resident)
{
    if (mgr == NULL || name == NULL || path == NULL) {
        return AURA_ERR_INVALID_ARG;
    }

    aura_infer_model_t *model = aura_infer_load(path, form);
    if (model == NULL) {
        return AURA_ERR_MODEL;
    }
    aura_model_info_t info;
    aura_model_get_info(model, &info);

    aura_osal_mutex_lock(mgr->lock);
    model_entry_t *e = find_entry(mgr, name);
    if (e == NULL) {
        if (mgr->count >= AURA_MODEL_MGR_MAX_MODELS) {
            aura_osal_mutex_unlock(mgr->lock);
            aura_infer_unload(model);
            return AURA_ERR_FULL;
        }
        e = &mgr->entries[mgr->count++];
        snprintf(e->name, sizeof(e->name), "%s", name);
        snprintf(e->path, sizeof(e->path), "%s", path);
        e->form     = form;
        e->resident = resident;
        e->refs     = 1; /* 注册即持有一份引用 */
        e->model    = model;
        e->weight_bytes  = info.weight_bytes;
        e->runtime_bytes = info.runtime_bytes;
        e->last_used     = ++mgr->clock;
    } else {
        /* 同名热更新：换掉旧模型。 */
        unload_entry(mgr, e);
        snprintf(e->path, sizeof(e->path), "%s", path);
        e->form     = form;
        e->resident = resident;
        e->refs     = 1;
        e->model    = model;
        e->weight_bytes  = info.weight_bytes;
        e->runtime_bytes = info.runtime_bytes;
        e->last_used     = ++mgr->clock;
    }
    aura_osal_mutex_unlock(mgr->lock);

    AURA_LOGI(TAG, "load %s (%s, %s, weight=%llu KB runtime=%llu KB)", e->name,
              aura_infer_form_name(form), e->path, (unsigned long long)(e->weight_bytes >> 10),
              (unsigned long long)(e->runtime_bytes >> 10));
    return AURA_OK;
}

aura_err_t aura_model_mgr_unload(aura_model_mgr_t *mgr, const char *name)
{
    if (mgr == NULL || name == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    aura_osal_mutex_lock(mgr->lock);
    model_entry_t *e = find_entry(mgr, name);
    if (e == NULL) {
        aura_osal_mutex_unlock(mgr->lock);
        return AURA_ERR_NOT_FOUND;
    }
    unload_entry(mgr, e);
    aura_osal_mutex_unlock(mgr->lock);
    return AURA_OK;
}

aura_infer_model_t *aura_model_mgr_get(aura_model_mgr_t *mgr, const char *name)
{
    if (mgr == NULL || name == NULL) {
        return NULL;
    }
    aura_osal_mutex_lock(mgr->lock);
    model_entry_t *e = find_entry(mgr, name);
    aura_infer_model_t *m = NULL;
    if (e != NULL && e->model != NULL) {
        e->last_used = ++mgr->clock;
        m = e->model;
    }
    aura_osal_mutex_unlock(mgr->lock);
    return m;
}

aura_err_t aura_model_mgr_acquire(aura_model_mgr_t *mgr, const char *name)
{
    if (mgr == NULL || name == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    aura_osal_mutex_lock(mgr->lock);
    model_entry_t *e = find_entry(mgr, name);
    if (e == NULL || e->model == NULL) {
        aura_osal_mutex_unlock(mgr->lock);
        return AURA_ERR_NOT_FOUND;
    }
    e->refs++;
    e->last_used = ++mgr->clock;
    aura_osal_mutex_unlock(mgr->lock);
    return AURA_OK;
}

aura_err_t aura_model_mgr_release(aura_model_mgr_t *mgr, const char *name)
{
    if (mgr == NULL || name == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    aura_osal_mutex_lock(mgr->lock);
    model_entry_t *e = find_entry(mgr, name);
    if (e == NULL) {
        aura_osal_mutex_unlock(mgr->lock);
        return AURA_ERR_NOT_FOUND;
    }
    if (e->refs > 1) {
        e->refs--;
    }
    aura_osal_mutex_unlock(mgr->lock);
    return AURA_OK;
}

void aura_model_mgr_set_budget(aura_model_mgr_t *mgr, uint64_t budget_bytes)
{
    if (mgr == NULL) {
        return;
    }
    aura_osal_mutex_lock(mgr->lock);
    mgr->budget_bytes = budget_bytes;
    aura_osal_mutex_unlock(mgr->lock);
}

uint64_t aura_model_mgr_get_budget(const aura_model_mgr_t *mgr)
{
    return (mgr == NULL) ? 0 : mgr->budget_bytes;
}

uint64_t aura_model_mgr_weights_bytes(const aura_model_mgr_t *mgr)
{
    if (mgr == NULL) {
        return 0;
    }
    aura_model_mgr_t *m = (aura_model_mgr_t *)mgr;
    uint64_t total = 0;
    aura_osal_mutex_lock(m->lock);
    for (uint32_t i = 0; i < m->count; i++) {
        total += m->entries[i].weight_bytes;
    }
    aura_osal_mutex_unlock(m->lock);
    return total;
}

uint64_t aura_model_mgr_runtime_bytes(const aura_model_mgr_t *mgr)
{
    if (mgr == NULL) {
        return 0;
    }
    aura_model_mgr_t *m = (aura_model_mgr_t *)mgr;
    uint64_t total = 0;
    aura_osal_mutex_lock(m->lock);
    for (uint32_t i = 0; i < m->count; i++) {
        total += m->entries[i].runtime_bytes;
    }
    aura_osal_mutex_unlock(m->lock);
    return total;
}

uint64_t aura_model_mgr_total_bytes(const aura_model_mgr_t *mgr)
{
    return aura_model_mgr_weights_bytes(mgr) + aura_model_mgr_runtime_bytes(mgr);
}

uint32_t aura_model_mgr_count(const aura_model_mgr_t *mgr)
{
    return (mgr == NULL) ? 0 : mgr->count;
}

void aura_model_mgr_dump(const aura_model_mgr_t *mgr)
{
    if (mgr == NULL) {
        return;
    }
    aura_model_mgr_t *m = (aura_model_mgr_t *)mgr;
    AURA_LOGI(TAG, "%-16s %-8s %-10s %8s %10s", "model", "form", "resident", "refs",
              "weight(MB)");
    aura_osal_mutex_lock(m->lock);
    for (uint32_t i = 0; i < m->count; i++) {
        const model_entry_t *e = &m->entries[i];
        AURA_LOGI(TAG, "%-16s %-8s %-10s %8u %10llu", e->name,
                  aura_infer_form_name(e->form), e->resident ? "yes" : "no", e->refs,
                  (unsigned long long)(e->weight_bytes >> 20));
    }
    AURA_LOGI(TAG, "total: weight=%llu MB runtime=%llu MB budget=%llu MB",
              (unsigned long long)(m->count > 0 ? aura_model_mgr_weights_bytes(mgr) >> 20 : 0),
              (unsigned long long)(aura_model_mgr_runtime_bytes(mgr) >> 20),
              (unsigned long long)(m->budget_bytes >> 20));
    aura_osal_mutex_unlock(m->lock);
}
