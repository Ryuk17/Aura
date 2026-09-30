#include "utils/logger/logger.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "osal/osal.h"

#define AURA_LOG_TAG_SLOTS 16
#define AURA_LOG_RATE_WINDOW_MS 1000u

typedef struct {
    const char *tag;      /* 以指针标识 tag，tag 必须是静态字符串 */
    uint32_t    window_start_ms;
    uint32_t    emitted;
    uint32_t    suppressed;
} tag_slot_t;

static aura_log_level_t g_level   = AURA_LOG_LVL_INFO;
static bool             g_inited  = false;
static tag_slot_t       g_slots[AURA_LOG_TAG_SLOTS];
static aura_mutex_t    *g_lock    = NULL;
static uint64_t         g_suppressed_total = 0;

static const char level_char[] = {'E', 'W', 'I', 'D', 'T', 'N'};

void aura_log_init(void)
{
    if (g_inited) {
        return;
    }
    g_lock = aura_osal_mutex_create();
    memset(g_slots, 0, sizeof(g_slots));
    g_inited = true;
}

void aura_log_set_level(aura_log_level_t level)
{
    aura_log_init();
    aura_osal_mutex_lock(g_lock);
    g_level = level;
    aura_osal_mutex_unlock(g_lock);
}

aura_log_level_t aura_log_get_level(void)
{
    return g_level;
}

bool aura_log_enabled(aura_log_level_t level)
{
    return (level <= g_level);
}

uint64_t aura_log_suppressed_count(void)
{
    return g_suppressed_total;
}

/* 返回 true 表示本次允许输出；被限速时返回 false。调用方持锁。 */
static bool rate_limit_check(const char *tag, uint32_t now_ms, bool *flush_suppressed,
                             uint32_t *suppressed_count)
{
    *flush_suppressed  = false;
    *suppressed_count  = 0;
    if (AURA_LOG_RATE_LIMIT == 0) {
        return true;
    }
    tag_slot_t *slot = NULL;
    tag_slot_t *lru  = &g_slots[0];
    for (int i = 0; i < AURA_LOG_TAG_SLOTS; i++) {
        if (g_slots[i].tag == tag) {
            slot = &g_slots[i];
            break;
        }
        if (g_slots[i].tag == NULL) {
            slot = &g_slots[i];
            slot->tag = tag;
            slot->window_start_ms = now_ms;
            break;
        }
        if (g_slots[i].window_start_ms < lru->window_start_ms) {
            lru = &g_slots[i];
        }
    }
    if (slot == NULL) {
        /* 槽位耗尽：直接复用最旧的一个，保证限速表本身不无限增长。 */
        slot = lru;
        slot->tag             = tag;
        slot->window_start_ms = now_ms;
        slot->emitted         = 0;
        slot->suppressed      = 0;
    }

    if ((uint32_t)(now_ms - slot->window_start_ms) >= AURA_LOG_RATE_WINDOW_MS) {
        if (slot->suppressed > 0) {
            *flush_suppressed = true;
            *suppressed_count = slot->suppressed;
        }
        slot->window_start_ms = now_ms;
        slot->emitted         = 0;
        slot->suppressed      = 0;
    }

    if (slot->emitted >= AURA_LOG_RATE_LIMIT) {
        slot->suppressed++;
        g_suppressed_total++;
        return false;
    }
    slot->emitted++;
    return true;
}

void aura_log_emit(aura_log_level_t level, const char *tag, const char *file, int line,
                   const char *fmt, ...)
{
    if (!g_inited) {
        aura_log_init();
    }

    char body[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    const char *base = file;
    if (base != NULL) {
        const char *slash = strrchr(base, '/');
        const char *bslash = strrchr(base, '\\');
        if (bslash != NULL && (slash == NULL || bslash > slash)) {
            slash = bslash;
        }
        if (slash != NULL) {
            base = slash + 1;
        }
    } else {
        base = "?";
    }

    char line_buf[640];
    int  n = 0;

    aura_osal_mutex_lock(g_lock);
    uint32_t now_ms = (uint32_t)aura_osal_time_ms();

    bool     flush_suppressed = false;
    uint32_t suppressed       = 0;
    bool     allowed = rate_limit_check((tag != NULL) ? tag : "-", now_ms, &flush_suppressed,
                                        &suppressed);
    if (flush_suppressed) {
        n += snprintf(line_buf + n, sizeof(line_buf) - (size_t)n,
                      "[%8llu][%c][%s] ... %u message(s) suppressed\n",
                      (unsigned long long)now_ms, 'N', (tag != NULL) ? tag : "-", suppressed);
        aura_osal_console_write(line_buf, (size_t)n);
        n = 0;
    }
    if (allowed) {
        char lc = level_char[(level >= 0 && level <= AURA_LOG_LVL_NONE) ? level : AURA_LOG_LVL_ERROR];
        n += snprintf(line_buf + n, sizeof(line_buf) - (size_t)n, "[%8llu][%c][%s] %s (%s:%d)\n",
                      (unsigned long long)now_ms, lc, (tag != NULL) ? tag : "-", body, base, line);
        aura_osal_console_write(line_buf, (size_t)n);
    }
    aura_osal_mutex_unlock(g_lock);
}
