/* Aura — Agent 工具注册表
 *
 * 工具是普通 C 回调：参数以 `arg.<name>=<value>` 文本传入，输出写入调用方缓冲。
 * 注册表在 init 期一次性建立（固定槽位，运行期不 malloc）。
 *
 * 内置演示工具：get_time / set_volume / get_weather（不触碰真实硬件，
 * 业务方通过 aura_tool_registry_register 替换为自己的实现）。
 */
#ifndef AURA_AGENT_TOOLS_TOOLS_H
#define AURA_AGENT_TOOLS_TOOLS_H

#include <stdbool.h>
#include <stdint.h>

#include "agent/stages/stage_proto.h"
#include "utils/error.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AURA_TOOL_NAME_MAX    32
#define AURA_TOOL_DESC_MAX    64
#define AURA_TOOL_OUTPUT_MAX  512
#define AURA_TOOL_MAX         8

typedef aura_err_t (*aura_tool_fn)(const aura_stage_kv_t *args, uint32_t nargs, char *out,
                                   uint32_t cap, void *user);

typedef struct aura_tool_entry {
    char          name[AURA_TOOL_NAME_MAX];
    char          description[AURA_TOOL_DESC_MAX];
    aura_tool_fn  fn;
    void         *user;
    bool          enabled;
    bool          idempotent;
    uint32_t      max_retries;
} aura_tool_entry_t;

typedef struct aura_tool_registry aura_tool_registry_t;

/* 创建注册表并注册内置演示工具。 */
aura_tool_registry_t *aura_tool_registry_create(void);
void                  aura_tool_registry_destroy(aura_tool_registry_t *reg);

/* 注册/覆盖工具（同名覆盖，便于业务替换内置实现）。 */
aura_err_t aura_tool_registry_register(aura_tool_registry_t *reg, const char *name,
                                       const char *description, aura_tool_fn fn, void *user);

const aura_tool_entry_t *aura_tool_registry_find(const aura_tool_registry_t *reg,
                                                 const char *name);

/* 读取 `<config_dir>/tools.conf` 或 `<config_dir>/agent/tools.conf`。
 * 支持的键：tool.<name>.enabled / max_retries / idempotent。 */
aura_err_t aura_tool_registry_load_config(aura_tool_registry_t *reg, const char *config_dir);

#ifdef __cplusplus
}
#endif

#endif /* AURA_AGENT_TOOLS_TOOLS_H */
