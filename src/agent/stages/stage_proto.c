/* Aura — Agent 数据流 stage 公共消息协议（实现） */
#include "agent/stages/stage_proto.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void trim(char *s)
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

uint32_t aura_stage_kv_parse(const char *payload, aura_stage_kv_t *out, uint32_t max)
{
    if (payload == NULL || out == NULL || max == 0) {
        return 0;
    }

    uint32_t    n = 0;
    const char *p = payload;
    while (*p != '\0' && n < max) {
        const char *nl  = strchr(p, '\n');
        size_t      len = (nl != NULL) ? (size_t)(nl - p) : strlen(p);
        if (len >= AURA_STAGE_KV_KEY_MAX + AURA_STAGE_KV_VAL_MAX + 2) {
            len = AURA_STAGE_KV_KEY_MAX + AURA_STAGE_KV_VAL_MAX + 1;
        }

        char line[AURA_STAGE_KV_KEY_MAX + AURA_STAGE_KV_VAL_MAX + 2];
        memcpy(line, p, len);
        line[len] = '\0';
        trim(line);

        if (line[0] != '\0' && line[0] != '#') {
            char *eq = strchr(line, '=');
            if (eq != NULL) {
                *eq = '\0';
                char *key = line;
                char *val = eq + 1;
                trim(key);
                trim(val);
                if (key[0] != '\0') {
                    snprintf(out[n].key, sizeof(out[n].key), "%.*s",
                             (int)(sizeof(out[n].key) - 1), key);
                    snprintf(out[n].val, sizeof(out[n].val), "%.*s",
                             (int)(sizeof(out[n].val) - 1), val);
                    n++;
                }
            }
        }

        if (nl == NULL) {
            break;
        }
        p = nl + 1;
    }
    return n;
}

const char *aura_stage_kv_get(const aura_stage_kv_t *kvs, uint32_t n, const char *key)
{
    if (kvs == NULL || key == NULL) {
        return NULL;
    }
    for (uint32_t i = 0; i < n; i++) {
        if (strcmp(kvs[i].key, key) == 0) {
            return kvs[i].val;
        }
    }
    return NULL;
}

bool aura_stage_kv_get_int(const aura_stage_kv_t *kvs, uint32_t n, const char *key,
                           int32_t *out)
{
    const char *val = aura_stage_kv_get(kvs, n, key);
    if (val == NULL || val[0] == '\0') {
        return false;
    }
    char *end = NULL;
    long  v   = strtol(val, &end, 10);
    if (end == val) {
        return false;
    }
    if (out != NULL) {
        *out = (int32_t)v;
    }
    return true;
}

aura_err_t aura_stage_publish(aura_flow_bus_t *bus, const char *topic, aura_flow_kind_t kind,
                              const aura_flow_ctx_t *ctx, const char *payload)
{
    if (bus == NULL || topic == NULL) {
        return AURA_ERR_INVALID_ARG;
    }

    aura_flow_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.kind = (uint32_t)kind;
    if (ctx != NULL) {
        msg.ctx = *ctx;
    }
    snprintf(msg.topic, sizeof(msg.topic), "%s", topic);
    if (payload != NULL) {
        snprintf(msg.payload, sizeof(msg.payload), "%s", payload);
        msg.len = (uint32_t)strlen(msg.payload);
    }
    return aura_flow_publish(bus, &msg);
}
