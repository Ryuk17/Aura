#include "utils/profiler/profiler.h"

#include <stdio.h>
#include <string.h>

#include "osal/osal.h"
#include "utils/logger/logger.h"

#define TAG "profiler"

static const char *const mark_names[AURA_PROF_MARK_COUNT] = {
    "idle_window",
    "kws_hit",
    "voiceprint_done",
    "asr_first_partial",
    "asr_final",
    "llm_first_token",
    "llm_done",
    "tts_first_chunk",
    "playback_start",
    "barge_in",
    "playback_stop",
    "error",
};

static aura_prof_session_t g_session;
static aura_mutex_t       *g_lock = NULL;

void aura_profiler_init(void)
{
    if (g_lock == NULL) {
        g_lock = aura_osal_mutex_create();
        memset(&g_session, 0, sizeof(g_session));
    }
}

void aura_profiler_reset(void)
{
    aura_profiler_init();
    aura_osal_mutex_lock(g_lock);
    memset(&g_session, 0, sizeof(g_session));
    aura_osal_mutex_unlock(g_lock);
}

void aura_profiler_begin_session(void)
{
    aura_profiler_reset();
    aura_profiler_mark(AURA_PROF_IDLE_WINDOW);
}

void aura_profiler_mark(aura_prof_mark_t mark)
{
    if (mark < 0 || mark >= AURA_PROF_MARK_COUNT) {
        return;
    }
    aura_profiler_init();
    uint64_t now = aura_osal_time_ms();
    aura_osal_mutex_lock(g_lock);
    if (!g_session.valid[mark]) { /* 同名打点只记首次，避免重复刷新掩盖真实延迟 */
        g_session.ts_ms[mark] = now;
        g_session.valid[mark] = true;
    }
    aura_osal_mutex_unlock(g_lock);
}

aura_err_t aura_profiler_snapshot(aura_prof_session_t *out)
{
    if (out == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    aura_profiler_init();
    aura_osal_mutex_lock(g_lock);
    *out = g_session;
    aura_osal_mutex_unlock(g_lock);
    return AURA_OK;
}

uint64_t aura_prof_snapshot_delta_ms(const aura_prof_session_t *s, aura_prof_mark_t from,
                                     aura_prof_mark_t to)
{
    if (s == NULL || from < 0 || from >= AURA_PROF_MARK_COUNT || to < 0 ||
        to >= AURA_PROF_MARK_COUNT) {
        return 0;
    }
    if (!s->valid[from] || !s->valid[to]) {
        return 0;
    }
    if (s->ts_ms[to] < s->ts_ms[from]) {
        return 0;
    }
    return s->ts_ms[to] - s->ts_ms[from];
}

const char *aura_prof_mark_name(aura_prof_mark_t mark)
{
    if (mark < 0 || mark >= AURA_PROF_MARK_COUNT) {
        return "?";
    }
    return mark_names[mark];
}

void aura_profiler_report(void)
{
    aura_prof_session_t s;
    if (aura_profiler_snapshot(&s) != AURA_OK) {
        return;
    }

    uint64_t wake_to_speak =
        aura_prof_snapshot_delta_ms(&s, AURA_PROF_KWS_HIT, AURA_PROF_PLAYBACK_START);
    uint64_t llm_ttft =
        aura_prof_snapshot_delta_ms(&s, AURA_PROF_ASR_FINAL, AURA_PROF_LLM_FIRST_TOKEN);
    uint64_t barge_stop =
        aura_prof_snapshot_delta_ms(&s, AURA_PROF_BARGE_IN, AURA_PROF_PLAYBACK_STOP);
    uint64_t asr_tail =
        aura_prof_snapshot_delta_ms(&s, AURA_PROF_KWS_HIT, AURA_PROF_ASR_FINAL);
    uint64_t tts_first =
        aura_prof_snapshot_delta_ms(&s, AURA_PROF_ASR_FINAL, AURA_PROF_TTS_FIRST_CHUNK);

    AURA_LOGI(TAG, "---- profiler report ----");
    AURA_LOGI(TAG, "wake -> playback start : %llu ms (target < 1000)", (unsigned long long)wake_to_speak);
    AURA_LOGI(TAG, "wake -> asr final      : %llu ms", (unsigned long long)asr_tail);
    AURA_LOGI(TAG, "asr final -> tts chunk : %llu ms", (unsigned long long)tts_first);
    AURA_LOGI(TAG, "llm first token (ttft) : %llu ms (target < 500)", (unsigned long long)llm_ttft);
    AURA_LOGI(TAG, "barge-in -> playback stop: %llu ms (target < 200)", (unsigned long long)barge_stop);
    AURA_LOGI(TAG, "-------------------------");
}
