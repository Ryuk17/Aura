/* Aura — Agent 工具注册表（实现） */
#include "agent/tools/tools.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "utils/logger/logger.h"

#define TAG "tools"

struct aura_tool_registry {
    aura_tool_entry_t entries[AURA_TOOL_MAX];
    uint32_t          count;
};

static void trim_inplace(char *s)
{
    if (s == NULL) {
        return;
    }
    char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') {
        p++;
    }
    if (p != s) {
        memmove(s, p, strlen(p) + 1);
    }
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' ||
                     s[n - 1] == '\n')) {
        s[--n] = '\0';
    }
}

/* ------------------------------------------------------------ 内置工具 */

static aura_err_t tool_get_time(const aura_stage_kv_t *args, uint32_t nargs, char *out,
                                uint32_t cap, void *user)
{
    (void)args;
    (void)nargs;
    (void)user;
    time_t    now = time(NULL);
    struct tm tm_buf;
#if defined(_WIN32)
    struct tm *tmp = localtime(&now);
    if (tmp == NULL) {
        return AURA_ERR_FAIL;
    }
    tm_buf = *tmp;
#else
    if (localtime_r(&now, &tm_buf) == NULL) {
        return AURA_ERR_FAIL;
    }
#endif
    if (strftime(out, cap, "%Y-%m-%d %H:%M", &tm_buf) == 0) {
        return AURA_ERR_FAIL;
    }
    return AURA_OK;
}

static aura_err_t tool_set_volume(const aura_stage_kv_t *args, uint32_t nargs, char *out,
                                  uint32_t cap, void *user)
{
    (void)user;
    int32_t level = 50;
    (void)aura_stage_kv_get_int(args, nargs, "arg.level", &level);
    if (level < 0 || level > 100) {
        snprintf(out, cap, "level out of range: %d", (int)level);
        return AURA_ERR_INVALID_ARG;
    }
    /* 演示实现：真实业务在这里调用 HAL/音频服务。 */
    snprintf(out, cap, "volume set to %d", (int)level);
    return AURA_OK;
}

static aura_err_t tool_get_weather(const aura_stage_kv_t *args, uint32_t nargs, char *out,
                                   uint32_t cap, void *user)
{
    (void)args;
    (void)nargs;
    (void)user;
    snprintf(out, cap, "晴，22°C（演示数据）");
    return AURA_OK;
}

/* ------------------------------------------------------------ 注册表 */

aura_tool_registry_t *aura_tool_registry_create(void)
{
    aura_tool_registry_t *reg = (aura_tool_registry_t *)calloc(1, sizeof(*reg));
    if (reg == NULL) {
        return NULL;
    }
    (void)aura_tool_registry_register(reg, "get_time", "查询当前时间", tool_get_time, NULL);
    (void)aura_tool_registry_register(reg, "set_volume", "设置音量 0-100", tool_set_volume,
                                      NULL);
    (void)aura_tool_registry_register(reg, "get_weather", "查询天气（演示）", tool_get_weather,
                                      NULL);
    return reg;
}

void aura_tool_registry_destroy(aura_tool_registry_t *reg)
{
    free(reg);
}

aura_err_t aura_tool_registry_register(aura_tool_registry_t *reg, const char *name,
                                       const char *description, aura_tool_fn fn, void *user)
{
    if (reg == NULL || name == NULL || name[0] == '\0' || fn == NULL ||
        strlen(name) >= AURA_TOOL_NAME_MAX) {
        return AURA_ERR_INVALID_ARG;
    }

    aura_tool_entry_t *entry = NULL;
    for (uint32_t i = 0; i < reg->count; i++) {
        if (strcmp(reg->entries[i].name, name) == 0) {
            entry = &reg->entries[i];
            break;
        }
    }
    if (entry == NULL) {
        if (reg->count >= AURA_TOOL_MAX) {
            return AURA_ERR_FULL;
        }
        entry = &reg->entries[reg->count++];
        memset(entry, 0, sizeof(*entry));
        snprintf(entry->name, sizeof(entry->name), "%s", name);
        entry->enabled     = true;
        entry->idempotent  = true;
        entry->max_retries = 1;
    }
    snprintf(entry->description, sizeof(entry->description), "%s",
             (description != NULL) ? description : "");
    entry->fn   = fn;
    entry->user = user;
    return AURA_OK;
}

const aura_tool_entry_t *aura_tool_registry_find(const aura_tool_registry_t *reg,
                                                 const char *name)
{
    if (reg == NULL || name == NULL) {
        return NULL;
    }
    for (uint32_t i = 0; i < reg->count; i++) {
        if (strcmp(reg->entries[i].name, name) == 0) {
            return &reg->entries[i];
        }
    }
    return NULL;
}

aura_err_t aura_tool_registry_load_config(aura_tool_registry_t *reg, const char *config_dir)
{
    if (reg == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    const char *dir = (config_dir != NULL && config_dir[0] != '\0') ? config_dir : "configs";
    char        path[320];

    snprintf(path, sizeof(path), "%s/tools.conf", dir);
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        snprintf(path, sizeof(path), "%s/agent/tools.conf", dir);
        f = fopen(path, "r");
    }
    if (f == NULL) {
        return AURA_OK; /* 配置可选：缺省 = 全部内置工具启用 */
    }

    char line[512];
    while (fgets(line, sizeof(line), f) != NULL) {
        trim_inplace(line);
        if (line[0] == '\0' || line[0] == '#') {
            continue;
        }
        char *eq = strchr(line, '=');
        if (eq == NULL) {
            continue;
        }
        *eq = '\0';
        char *key = line;
        char *val = eq + 1;
        trim_inplace(key);
        trim_inplace(val);

        if (strncmp(key, "tool.", 5) != 0) {
            continue;
        }
        char *rest = key + 5;
        char *dot  = strrchr(rest, '.');
        if (dot == NULL) {
            continue;
        }
        *dot = '\0';
        const char *field = dot + 1;
        const aura_tool_entry_t *found = aura_tool_registry_find(reg, rest);
        if (found == NULL) {
            AURA_LOGW(TAG, "tools.conf references unknown tool '%s'", rest);
            continue;
        }
        /* find 返回 const；配置只允许在 init 期调用，这里去掉 const 写入。 */
        aura_tool_entry_t *entry = (aura_tool_entry_t *)found;
        if (strcmp(field, "enabled") == 0) {
            entry->enabled = (atoi(val) != 0);
        } else if (strcmp(field, "idempotent") == 0) {
            entry->idempotent = (atoi(val) != 0);
        } else if (strcmp(field, "max_retries") == 0) {
            int n = atoi(val);
            entry->max_retries = (n < 0) ? 0u : (uint32_t)n;
        }
    }
    fclose(f);
    AURA_LOGI(TAG, "tools config loaded: %s", path);
    return AURA_OK;
}
