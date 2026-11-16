/* Aura — dialogue stage（实现） */
#include "agent/stages/dialogue_stage.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent/flow/flow_stage.h"
#include "agent/stages/stage_proto.h"
#include "utils/logger/logger.h"

#define TAG "dialogue"

#define AURA_DIALOGUE_MAX_INTENTS 8
#define AURA_DIALOGUE_NAME_MAX    24
#define AURA_DIALOGUE_KW_MAX      128
#define AURA_DIALOGUE_TOOL_MAX    32

typedef struct aura_dialogue_rule {
    char name[AURA_DIALOGUE_NAME_MAX];
    char keywords[AURA_DIALOGUE_KW_MAX]; /* '|' 分隔 */
    char tool[AURA_DIALOGUE_TOOL_MAX];
} aura_dialogue_rule_t;

typedef struct aura_dialogue_config {
    char                 system_prompt[384];
    aura_dialogue_rule_t rules[AURA_DIALOGUE_MAX_INTENTS];
    uint32_t             rule_count;
} aura_dialogue_config_t;

struct aura_dialogue_stage {
    aura_flow_bus_t        *bus;
    aura_session_runtime_t *rt;
    aura_agent_memory_t    *memory;
    aura_dialogue_config_t  cfg;
    aura_flow_stage_t      *stage;
};

static const char *const kDefaultSystem =
    "你是一个端侧语音助手。回答简洁、口语化，一次只问一个问题。";

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

static aura_dialogue_rule_t *find_rule(aura_dialogue_config_t *cfg, const char *name)
{
    for (uint32_t i = 0; i < cfg->rule_count; i++) {
        if (strcmp(cfg->rules[i].name, name) == 0) {
            return &cfg->rules[i];
        }
    }
    if (cfg->rule_count >= AURA_DIALOGUE_MAX_INTENTS) {
        return NULL;
    }
    aura_dialogue_rule_t *r = &cfg->rules[cfg->rule_count++];
    memset(r, 0, sizeof(*r));
    snprintf(r->name, sizeof(r->name), "%.*s", (int)(sizeof(r->name) - 1), name);
    return r;
}

static void add_rule(aura_dialogue_config_t *cfg, const char *name, const char *keywords,
                     const char *tool)
{
    aura_dialogue_rule_t *r = find_rule(cfg, name);
    if (r == NULL) {
        return;
    }
    snprintf(r->keywords, sizeof(r->keywords), "%s", keywords);
    snprintf(r->tool, sizeof(r->tool), "%s", tool);
}

static void config_defaults(aura_dialogue_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    snprintf(cfg->system_prompt, sizeof(cfg->system_prompt), "%s", kDefaultSystem);
    add_rule(cfg, "weather", "天气|weather", "get_weather");
    add_rule(cfg, "time", "时间|几点|time", "get_time");
    add_rule(cfg, "volume", "音量|volume", "set_volume");
}

static void config_default_rules(aura_dialogue_config_t *cfg)
{
    cfg->rule_count = 0;
    add_rule(cfg, "weather", "天气|weather", "get_weather");
    add_rule(cfg, "time", "时间|几点|time", "get_time");
    add_rule(cfg, "volume", "音量|volume", "set_volume");
}

/* 依次尝试 <dir>/prompts.conf 与 <dir>/agent/prompts.conf。 */
static void config_load(aura_dialogue_config_t *cfg, const char *config_dir)
{
    const char *dir = (config_dir != NULL && config_dir[0] != '\0') ? config_dir : "configs";
    char        path[320];

    snprintf(path, sizeof(path), "%s/prompts.conf", dir);
    FILE *f = fopen(path, "r");
    if (f == NULL) {
        snprintf(path, sizeof(path), "%s/agent/prompts.conf", dir);
        f = fopen(path, "r");
    }
    if (f == NULL) {
        AURA_LOGD(TAG, "no prompts.conf under '%s' — using built-in rules", dir);
        return;
    }

    cfg->rule_count = 0;
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

        if (strcmp(key, "system") == 0) {
            snprintf(cfg->system_prompt, sizeof(cfg->system_prompt), "%.*s",
                     (int)(sizeof(cfg->system_prompt) - 1), val);
            continue;
        }
        if (strncmp(key, "intent.", 7) != 0) {
            continue;
        }
        char *rest = key + 7;
        char *dot  = strrchr(rest, '.');
        if (dot == NULL) {
            continue;
        }
        *dot = '\0';
        const char *field = dot + 1;
        aura_dialogue_rule_t *r = find_rule(cfg, rest);
        if (r == NULL) {
            AURA_LOGW(TAG, "too many intents in prompts.conf (max %d)",
                      AURA_DIALOGUE_MAX_INTENTS);
            break;
        }
        if (strcmp(field, "keywords") == 0) {
            snprintf(r->keywords, sizeof(r->keywords), "%.*s",
                     (int)(sizeof(r->keywords) - 1), val);
        } else if (strcmp(field, "tool") == 0) {
            snprintf(r->tool, sizeof(r->tool), "%.*s", (int)(sizeof(r->tool) - 1), val);
        }
    }
    fclose(f);

    if (cfg->rule_count == 0) {
        config_default_rules(cfg);
    }
    if (cfg->system_prompt[0] == '\0') {
        snprintf(cfg->system_prompt, sizeof(cfg->system_prompt), "%s", kDefaultSystem);
    }
    AURA_LOGI(TAG, "dialogue rules loaded: %u (%s)", cfg->rule_count, path);
}

/* 把 ASR 文本压成单行：payload 的 key=value 格式不能带换行。 */
static void sanitize_text(const aura_flow_msg_t *msg, char *out, size_t cap)
{
    size_t n = (msg->len < sizeof(msg->payload)) ? msg->len : sizeof(msg->payload);
    if (n >= cap) {
        n = cap - 1;
    }
    size_t j = 0;
    for (size_t i = 0; i < n && j + 1 < cap; i++) {
        char c = msg->payload[i];
        if (c == '\0') {
            break;
        }
        out[j++] = (c == '\n' || c == '\r') ? ' ' : c;
    }
    out[j] = '\0';
    trim_inplace(out);
}

static bool keyword_list_match(const char *list, const char *text)
{
    const char *p = list;
    while (p != NULL && *p != '\0') {
        const char *sep = strchr(p, '|');
        size_t      n   = (sep != NULL) ? (size_t)(sep - p) : strlen(p);
        if (n > 0) {
            char kw[64];
            if (n >= sizeof(kw)) {
                n = sizeof(kw) - 1;
            }
            memcpy(kw, p, n);
            kw[n] = '\0';
            trim_inplace(kw);
            if (kw[0] != '\0' && strstr(text, kw) != NULL) {
                return true;
            }
        }
        if (sep == NULL) {
            break;
        }
        p = sep + 1;
    }
    return false;
}

static const aura_dialogue_rule_t *match_rule(const aura_dialogue_config_t *cfg,
                                              const char *text)
{
    for (uint32_t i = 0; i < cfg->rule_count; i++) {
        const aura_dialogue_rule_t *r = &cfg->rules[i];
        if (r->keywords[0] != '\0' && keyword_list_match(r->keywords, text)) {
            return r;
        }
    }
    return NULL;
}

static void extract_first_number(const char *text, char *out, size_t cap)
{
    const char *p = text;
    while (*p != '\0' && !isdigit((unsigned char)*p)) {
        p++;
    }
    if (*p == '\0') {
        out[0] = '\0';
        return;
    }
    size_t i = 0;
    while (isdigit((unsigned char)p[i]) && i + 1 < cap) {
        out[i] = p[i];
        i++;
    }
    out[i] = '\0';
}

static aura_err_t dialogue_handler(aura_flow_msg_t *msg, void *user)
{
    aura_dialogue_stage_t *s = (aura_dialogue_stage_t *)user;
    if (s == NULL || msg == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    if (strcmp(msg->topic, AURA_TOPIC_ASR_FINAL) != 0) {
        return AURA_OK;
    }

    char text[400];
    sanitize_text(msg, text, sizeof(text));
    if (text[0] == '\0') {
        return AURA_OK;
    }

    aura_flow_ctx_t ctx;
    if (msg->ctx.session_id != 0) {
        ctx = msg->ctx;
    } else if (!aura_session_runtime_ensure_turn(s->rt, &ctx)) {
        AURA_LOGD(TAG, "ignore asr.final before wake");
        return AURA_OK;
    }

    const aura_dialogue_rule_t *rule   = match_rule(&s->cfg, text);
    const char                 *intent = (rule != NULL) ? rule->name : "chat";
    const char                 *tool   = (rule != NULL) ? rule->tool : "";

    char slot_level[16] = "";
    if (tool[0] != '\0' && strstr(tool, "volume") != NULL) {
        extract_first_number(text, slot_level, sizeof(slot_level));
    }

    char payload[640];
    int  off = snprintf(payload, sizeof(payload), "intent=%s\ntext=%s\ntool=%s\n", intent, text,
                        tool);
    if (off < 0) {
        off = 0;
    }
    if ((size_t)off < sizeof(payload) && slot_level[0] != '\0') {
        (void)snprintf(payload + off, sizeof(payload) - (size_t)off, "slot.level=%s\n",
                       slot_level);
    }

    (void)aura_stage_publish(s->bus, AURA_TOPIC_DIALOGUE_INTENT, AURA_FLOW_TURN, &ctx, payload);
    AURA_LOGD(TAG, "intent=%s tool=%s session=%u turn=%u", intent, tool, ctx.session_id,
              ctx.turn_id);

    if (s->memory != NULL) {
        (void)aura_agent_memory_add(s->memory, ctx.session_id, ctx.speaker_id, "user", text);
    }

    char mem_payload[512];
    snprintf(mem_payload, sizeof(mem_payload), "role=user\ntext=%s\n", text);
    (void)aura_stage_publish(s->bus, AURA_TOPIC_MEMORY_WRITE, AURA_FLOW_MEMORY_OP, &ctx,
                             mem_payload);

    return AURA_OK;
}

aura_dialogue_stage_t *aura_dialogue_stage_create(aura_flow_bus_t *bus,
                                                  aura_session_runtime_t *rt,
                                                  aura_agent_memory_t *memory,
                                                  const char *config_dir)
{
    if (bus == NULL || rt == NULL) {
        return NULL;
    }

    aura_dialogue_stage_t *s = (aura_dialogue_stage_t *)calloc(1, sizeof(*s));
    if (s == NULL) {
        return NULL;
    }
    s->bus    = bus;
    s->rt     = rt;
    s->memory = memory;
    config_defaults(&s->cfg);
    config_load(&s->cfg, config_dir);

    s->stage = aura_flow_stage_create(bus, "dialogue", dialogue_handler, s, 32,
                                      AURA_FLOW_DROP_OLDEST);
    if (s->stage == NULL) {
        free(s);
        return NULL;
    }
    if (aura_flow_stage_subscribe(s->stage, AURA_TOPIC_ASR_FINAL) != AURA_OK) {
        aura_flow_stage_destroy(s->stage);
        free(s);
        return NULL;
    }
    return s;
}

void aura_dialogue_stage_destroy(aura_dialogue_stage_t *stage)
{
    if (stage == NULL) {
        return;
    }
    aura_flow_stage_destroy(stage->stage);
    free(stage);
}

aura_err_t aura_dialogue_stage_start(aura_dialogue_stage_t *stage)
{
    if (stage == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    return aura_flow_stage_start(stage->stage);
}

aura_err_t aura_dialogue_stage_stop(aura_dialogue_stage_t *stage)
{
    if (stage == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    return aura_flow_stage_stop(stage->stage);
}

const char *aura_dialogue_stage_system_prompt(const aura_dialogue_stage_t *stage)
{
    return (stage != NULL) ? stage->cfg.system_prompt : "";
}
