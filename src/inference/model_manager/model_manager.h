/* Aura — 模型管理器（常驻 / 按需策略的执行者，todo.md 4.4）
 *
 * 策略由调用方（agent_core）声明，本模块只负责记账与执行：
 *   - load：加载模型并登记（resident=true 表示常驻，不可被 LRU 卸载）；
 *   - unload：显式卸载；on-demand 模型在内存预算超限时按 LRU 淘汰；
 *   - 内存账：权重 + 运行时内存分开记账，供 memory_budget.md 复核；
 *   - 运行期不产生 malloc/free：表是静态数组。
 *
 * 注意：本层不关心模型是 KWS 还是 ASR —— 那是由算法模块通过
 * model_manager 拿 session 后自己解释的；本层只管理"加载/卸载/引用"。
 */
#ifndef AURA_INFERENCE_MODEL_MANAGER_MODEL_MANAGER_H
#define AURA_INFERENCE_MODEL_MANAGER_MODEL_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#include "inference/inference.h"
#include "utils/error.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef AURA_MODEL_MGR_MAX_MODELS
#define AURA_MODEL_MGR_MAX_MODELS 16
#endif

typedef struct aura_model_mgr aura_model_mgr_t;

aura_model_mgr_t *aura_model_mgr_create(void);
void              aura_model_mgr_destroy(aura_model_mgr_t *mgr);

/* 加载并登记。name 是引用名（如 "vad"/"kws"）；resident 模型不会被 LRU 淘汰。
 * 已存在同名模型时先卸载旧的（允许热更新模型文件）。 */
aura_err_t aura_model_mgr_load(aura_model_mgr_t *mgr, const char *name, const char *path,
                               aura_infer_form_t form, bool resident);

/* 卸载并注销。 */
aura_err_t aura_model_mgr_unload(aura_model_mgr_t *mgr, const char *name);

/* 取模型句柄（不转移所有权）。不存在返回 NULL。 */
aura_infer_model_t *aura_model_mgr_get(aura_model_mgr_t *mgr, const char *name);

/* 引用计数（常驻模型外的多引用方管理生命周期）。 */
aura_err_t aura_model_mgr_acquire(aura_model_mgr_t *mgr, const char *name);
aura_err_t aura_model_mgr_release(aura_model_mgr_t *mgr, const char *name);

/* 内存预算（字节）。设为 0 表示不限制。 */
void     aura_model_mgr_set_budget(aura_model_mgr_t *mgr, uint64_t budget_bytes);
uint64_t aura_model_mgr_get_budget(const aura_model_mgr_t *mgr);

/* 当前内存账。 */
uint64_t aura_model_mgr_weights_bytes(const aura_model_mgr_t *mgr);
uint64_t aura_model_mgr_runtime_bytes(const aura_model_mgr_t *mgr);
uint64_t aura_model_mgr_total_bytes(const aura_model_mgr_t *mgr);
uint32_t aura_model_mgr_count(const aura_model_mgr_t *mgr);

/* 打印当前常驻/按需模型清单与内存账（memory_budget.md 复核工具）。 */
void aura_model_mgr_dump(const aura_model_mgr_t *mgr);

#ifdef __cplusplus
}
#endif

#endif /* AURA_INFERENCE_MODEL_MANAGER_MODEL_MANAGER_H */
