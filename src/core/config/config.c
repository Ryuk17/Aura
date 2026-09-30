#include "core/config/config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils/logger/logger.h"

#define TAG "config"

#define AURA_CONFIG_LINE_MAX 512

void aura_config_default(aura_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->sample_rate       = 16000;
    cfg->frame_ms          = 10;
    cfg->channels          = 1;
    cfg->audio_queue_depth = 8;
    cfg->text_queue_depth  = 16;
    cfg->event_queue_depth = 64;
    cfg->frame_pool_blocks = 16;
    cfg->log_level         = AURA_LOG_LVL_INFO;
    cfg->sm_auto_reset     = true;
    cfg->sm_auto_reset_ms  = 1000;
    cfg->profiler_enable   = true;
    snprintf(cfg->board, sizeof(cfg->board), "%s", "host_sim");
    snprintf(cfg->model_dir, sizeof(cfg->model_dir), "%s", "models");
    cfg->enable_kws         = true;
    cfg->enable_voiceprint  = true;
    cfg->enable_asr         = true;
    cfg->enable_llm         = true;
    cfg->enable_tts         = true;
    cfg->enable_barge_in    = true;
    cfg->enable_cloud       = false;
}

static bool parse_bool(const char *v, bool *out)
{
    if (strcmp(v, "1") == 0 || strcmp(v, "true") == 0 || strcmp(v, "on") == 0 ||
        strcmp(v, "yes") == 0) {
        *out = true;
        return true;
    }
    if (strcmp(v, "0") == 0 || strcmp(v, "false") == 0 || strcmp(v, "off") == 0 ||
        strcmp(v, "no") == 0) {
        *out = false;
        return true;
    }
    return false;
}

static bool parse_u32(const char *v, uint32_t *out)
{
    char *end = NULL;
    unsigned long n = strtoul(v, &end, 0);
    if (end == v || (end != NULL && *end != '\0')) {
        return false;
    }
    *out = (uint32_t)n;
    return true;
}

/* 去除首尾空白，返回指向内部的指针（就地写入 '\0'）。 */
static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') {
        s++;
    }
    size_t len = strlen(s);
    while (len > 0) {
        char c = s[len - 1];
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            s[--len] = '\0';
        } else {
            break;
        }
    }
    return s;
}

static aura_err_t apply_kv(aura_config_t *cfg, const char *key, const char *val)
{
    if (strcmp(key, "sample_rate") == 0) {
        return parse_u32(val, &cfg->sample_rate) ? AURA_OK : AURA_ERR_INVALID_ARG;
    }
    if (strcmp(key, "frame_ms") == 0) {
        return parse_u32(val, &cfg->frame_ms) ? AURA_OK : AURA_ERR_INVALID_ARG;
    }
    if (strcmp(key, "channels") == 0) {
        return parse_u32(val, &cfg->channels) ? AURA_OK : AURA_ERR_INVALID_ARG;
    }
    if (strcmp(key, "audio_queue_depth") == 0) {
        return parse_u32(val, &cfg->audio_queue_depth) ? AURA_OK : AURA_ERR_INVALID_ARG;
    }
    if (strcmp(key, "text_queue_depth") == 0) {
        return parse_u32(val, &cfg->text_queue_depth) ? AURA_OK : AURA_ERR_INVALID_ARG;
    }
    if (strcmp(key, "event_queue_depth") == 0) {
        return parse_u32(val, &cfg->event_queue_depth) ? AURA_OK : AURA_ERR_INVALID_ARG;
    }
    if (strcmp(key, "frame_pool_blocks") == 0) {
        return parse_u32(val, &cfg->frame_pool_blocks) ? AURA_OK : AURA_ERR_INVALID_ARG;
    }
    if (strcmp(key, "sm_auto_reset") == 0) {
        return parse_bool(val, &cfg->sm_auto_reset) ? AURA_OK : AURA_ERR_INVALID_ARG;
    }
    if (strcmp(key, "sm_auto_reset_ms") == 0) {
        return parse_u32(val, &cfg->sm_auto_reset_ms) ? AURA_OK : AURA_ERR_INVALID_ARG;
    }
    if (strcmp(key, "profiler_enable") == 0) {
        return parse_bool(val, &cfg->profiler_enable) ? AURA_OK : AURA_ERR_INVALID_ARG;
    }
    if (strcmp(key, "log_level") == 0) {
        if (strcmp(val, "error") == 0) {
            cfg->log_level = AURA_LOG_LVL_ERROR;
        } else if (strcmp(val, "warn") == 0) {
            cfg->log_level = AURA_LOG_LVL_WARN;
        } else if (strcmp(val, "info") == 0) {
            cfg->log_level = AURA_LOG_LVL_INFO;
        } else if (strcmp(val, "debug") == 0) {
            cfg->log_level = AURA_LOG_LVL_DEBUG;
        } else if (strcmp(val, "trace") == 0) {
            cfg->log_level = AURA_LOG_LVL_TRACE;
        } else {
            return AURA_ERR_INVALID_ARG;
        }
        return AURA_OK;
    }
    if (strcmp(key, "board") == 0) {
        snprintf(cfg->board, sizeof(cfg->board), "%s", val);
        return AURA_OK;
    }
    if (strcmp(key, "config_dir") == 0) {
        snprintf(cfg->config_dir, sizeof(cfg->config_dir), "%s", val);
        return AURA_OK;
    }
    if (strcmp(key, "model_dir") == 0) {
        snprintf(cfg->model_dir, sizeof(cfg->model_dir), "%s", val);
        return AURA_OK;
    }
    if (strcmp(key, "enable_kws") == 0) {
        return parse_bool(val, &cfg->enable_kws) ? AURA_OK : AURA_ERR_INVALID_ARG;
    }
    if (strcmp(key, "enable_voiceprint") == 0) {
        return parse_bool(val, &cfg->enable_voiceprint) ? AURA_OK : AURA_ERR_INVALID_ARG;
    }
    if (strcmp(key, "enable_asr") == 0) {
        return parse_bool(val, &cfg->enable_asr) ? AURA_OK : AURA_ERR_INVALID_ARG;
    }
    if (strcmp(key, "enable_llm") == 0) {
        return parse_bool(val, &cfg->enable_llm) ? AURA_OK : AURA_ERR_INVALID_ARG;
    }
    if (strcmp(key, "enable_tts") == 0) {
        return parse_bool(val, &cfg->enable_tts) ? AURA_OK : AURA_ERR_INVALID_ARG;
    }
    if (strcmp(key, "enable_barge_in") == 0) {
        return parse_bool(val, &cfg->enable_barge_in) ? AURA_OK : AURA_ERR_INVALID_ARG;
    }
    if (strcmp(key, "enable_cloud") == 0) {
        return parse_bool(val, &cfg->enable_cloud) ? AURA_OK : AURA_ERR_INVALID_ARG;
    }
    AURA_LOGW(TAG, "unknown config key '%s' ignored", key);
    return AURA_OK;
}

aura_err_t aura_config_load_file(aura_config_t *cfg, const char *path)
{
    if (cfg == NULL || path == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        AURA_LOGE(TAG, "cannot open config file: %s", path);
        return AURA_ERR_IO;
    }

    char     line[AURA_CONFIG_LINE_MAX];
    uint32_t lineno = 0;
    aura_err_t rc   = AURA_OK;

    while (fgets(line, sizeof(line), fp) != NULL) {
        lineno++;
        char *s = trim(line);
        if (*s == '\0' || *s == '#' || *s == ';') {
            continue;
        }
        char *eq = strchr(s, '=');
        if (eq == NULL) {
            AURA_LOGE(TAG, "%s:%u: missing '='", path, lineno);
            rc = AURA_ERR_INVALID_ARG;
            break;
        }
        *eq = '\0';
        char *key = trim(s);
        char *val = trim(eq + 1);
        if (*key == '\0') {
            AURA_LOGE(TAG, "%s:%u: empty key", path, lineno);
            rc = AURA_ERR_INVALID_ARG;
            break;
        }
        rc = apply_kv(cfg, key, val);
        if (rc != AURA_OK) {
            AURA_LOGE(TAG, "%s:%u: bad value for '%s': '%s'", path, lineno, key, val);
            break;
        }
    }
    fclose(fp);
    if (rc == AURA_OK) {
        AURA_LOGI(TAG, "loaded config: %s", path);
    }
    return rc;
}

void aura_config_dump(const aura_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    AURA_LOGI(TAG, "board=%s rate=%u frame=%ums ch=%u", cfg->board, cfg->sample_rate,
              cfg->frame_ms, cfg->channels);
    AURA_LOGI(TAG, "queues audio=%u text=%u event=%u frame_pool=%u", cfg->audio_queue_depth,
              cfg->text_queue_depth, cfg->event_queue_depth, cfg->frame_pool_blocks);
    AURA_LOGI(TAG, "modules kws=%d vp=%d asr=%d llm=%d tts=%d barge_in=%d cloud=%d",
              (int)cfg->enable_kws, (int)cfg->enable_voiceprint, (int)cfg->enable_asr,
              (int)cfg->enable_llm, (int)cfg->enable_tts, (int)cfg->enable_barge_in,
              (int)cfg->enable_cloud);
    AURA_LOGI(TAG, "model_dir=%s config_dir=%s", cfg->model_dir, cfg->config_dir);
}
