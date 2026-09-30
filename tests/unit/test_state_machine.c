/* 状态机单元测试：迁移表穷举 + 关键路径 */
#include "aura_test.h"

#include "core/state_machine/state_machine.h"

static void test_happy_path(void)
{
    aura_sm_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    aura_sm_t *sm = aura_sm_create(&cfg, NULL);
    AURA_ASSERT(sm != NULL);
    AURA_ASSERT_EQ(aura_sm_state(sm), AURA_STATE_IDLE);

    /* Idle：KWS 命中 → Listening */
    AURA_ASSERT(aura_sm_handle(sm, AURA_TRIG_START) == AURA_OK);
    AURA_ASSERT(aura_sm_handle(sm, AURA_TRIG_KWS_HIT) == AURA_OK);
    AURA_ASSERT_EQ(aura_sm_state(sm), AURA_STATE_LISTENING);

    /* Listening：声纹通过（自环）/ ASR 结束 → Thinking */
    AURA_ASSERT(aura_sm_handle(sm, AURA_TRIG_VOICEPRINT_OK) == AURA_OK);
    AURA_ASSERT(aura_sm_handle(sm, AURA_TRIG_ASR_FINAL) == AURA_OK);
    AURA_ASSERT_EQ(aura_sm_state(sm), AURA_STATE_THINKING);

    /* Thinking：TTS 首包 → Speaking */
    AURA_ASSERT(aura_sm_handle(sm, AURA_TRIG_TTS_START) == AURA_OK);
    AURA_ASSERT_EQ(aura_sm_state(sm), AURA_STATE_SPEAKING);

    /* Speaking：有效打断 → Listening（不是 Interrupted 状态） */
    AURA_ASSERT(aura_sm_handle(sm, AURA_TRIG_BARGE_IN) == AURA_OK);
    AURA_ASSERT_EQ(aura_sm_state(sm), AURA_STATE_LISTENING);

    /* Listening：静音超时 → Idle */
    AURA_ASSERT(aura_sm_handle(sm, AURA_TRIG_SILENCE_TIMEOUT) == AURA_OK);
    AURA_ASSERT_EQ(aura_sm_state(sm), AURA_STATE_IDLE);

    aura_sm_destroy(sm);
}

static void test_voiceprint_reject(void)
{
    aura_sm_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    aura_sm_t *sm = aura_sm_create(&cfg, NULL);
    AURA_ASSERT(sm != NULL);

    AURA_ASSERT(aura_sm_handle(sm, AURA_TRIG_KWS_HIT) == AURA_OK);
    AURA_ASSERT_EQ(aura_sm_state(sm), AURA_STATE_LISTENING);
    /* 声纹失败：直接退回 Idle，不进 ASR（省算力） */
    AURA_ASSERT(aura_sm_handle(sm, AURA_TRIG_VOICEPRINT_FAIL) == AURA_OK);
    AURA_ASSERT_EQ(aura_sm_state(sm), AURA_STATE_IDLE);

    aura_sm_destroy(sm);
}

static void test_error_and_auto_reset(void)
{
    aura_sm_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.auto_reset     = true;
    cfg.auto_reset_ms  = 80;
    aura_sm_t *sm = aura_sm_create(&cfg, NULL);
    AURA_ASSERT(sm != NULL);

    AURA_ASSERT(aura_sm_handle(sm, AURA_TRIG_KWS_HIT) == AURA_OK);
    /* 任意状态异常 → Error */
    AURA_ASSERT(aura_sm_handle(sm, AURA_TRIG_ERROR) == AURA_OK);
    AURA_ASSERT_EQ(aura_sm_state(sm), AURA_STATE_ERROR);

    /* 等待自动复位回 Idle */
    int waited = 0;
    while (aura_sm_state(sm) != AURA_STATE_IDLE && waited < 200) {
        aura_test_sleep_ms(10);
        waited += 10;
    }
    AURA_ASSERT_EQ(aura_sm_state(sm), AURA_STATE_IDLE);

    aura_sm_stats_t stats;
    aura_sm_stats(sm, &stats);
    AURA_CHECK(stats.errors >= 1);
    AURA_CHECK(stats.auto_resets >= 1);

    aura_sm_destroy(sm);
}

static void test_illegal_transitions_rejected(void)
{
    aura_sm_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    aura_sm_t *sm = aura_sm_create(&cfg, NULL);
    AURA_ASSERT(sm != NULL);

    /* Idle 下直接来 ASR 结果：必须拒绝 */
    AURA_ASSERT(aura_sm_handle(sm, AURA_TRIG_ASR_FINAL) == AURA_ERR_STATE);
    AURA_ASSERT_EQ(aura_sm_state(sm), AURA_STATE_IDLE);
    /* Idle 下打断：拒绝（Interrupted 不是状态，也没有对应迁移） */
    AURA_ASSERT(aura_sm_handle(sm, AURA_TRIG_BARGE_IN) == AURA_ERR_STATE);
    /* Thinking 下 KWS 再命中：拒绝（已在会话内） */
    AURA_ASSERT(aura_sm_handle(sm, AURA_TRIG_KWS_HIT) == AURA_OK);
    AURA_ASSERT(aura_sm_handle(sm, AURA_TRIG_ASR_FINAL) == AURA_OK);
    AURA_ASSERT(aura_sm_handle(sm, AURA_TRIG_KWS_HIT) == AURA_ERR_STATE);

    aura_sm_stats_t stats;
    aura_sm_stats(sm, &stats);
    AURA_CHECK(stats.rejected >= 3);

    aura_sm_destroy(sm);
}

static void test_speaking_tts_done_returns_listening(void)
{
    /* 播完 → Listening 等下一轮；静音超时才回 Idle（多轮对话需要）。 */
    aura_sm_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    aura_sm_t *sm = aura_sm_create(&cfg, NULL);
    AURA_ASSERT(sm != NULL);

    AURA_ASSERT(aura_sm_handle(sm, AURA_TRIG_KWS_HIT) == AURA_OK);
    AURA_ASSERT(aura_sm_handle(sm, AURA_TRIG_ASR_FINAL) == AURA_OK);
    AURA_ASSERT(aura_sm_handle(sm, AURA_TRIG_TTS_START) == AURA_OK);
    AURA_ASSERT_EQ(aura_sm_state(sm), AURA_STATE_SPEAKING);
    AURA_ASSERT(aura_sm_handle(sm, AURA_TRIG_TTS_DONE) == AURA_OK);
    AURA_ASSERT_EQ(aura_sm_state(sm), AURA_STATE_LISTENING);
    AURA_ASSERT(aura_sm_handle(sm, AURA_TRIG_SILENCE_TIMEOUT) == AURA_OK);
    AURA_ASSERT_EQ(aura_sm_state(sm), AURA_STATE_IDLE);
    aura_sm_destroy(sm);
}

static void test_trigger_from_event(void)
{
    aura_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = AURA_EVENT_KWS_HIT;
    AURA_ASSERT_EQ(aura_sm_trigger_from_event(&ev), AURA_TRIG_KWS_HIT);

    ev.type = AURA_EVENT_VOICEPRINT_RESULT;
    ev.u.voiceprint.passed = false;
    AURA_ASSERT_EQ(aura_sm_trigger_from_event(&ev), AURA_TRIG_VOICEPRINT_FAIL);
    ev.u.voiceprint.passed = true;
    AURA_ASSERT_EQ(aura_sm_trigger_from_event(&ev), AURA_TRIG_VOICEPRINT_OK);

    ev.type = AURA_EVENT_ERROR;
    AURA_ASSERT_EQ(aura_sm_trigger_from_event(&ev), AURA_TRIG_ERROR);

    ev.type = AURA_EVENT_LLM_TOKEN; /* 状态机不关心的事件 */
    AURA_ASSERT_EQ(aura_sm_trigger_from_event(&ev), AURA_TRIG_NONE);
}

static void test_names(void)
{
    AURA_ASSERT_STREQ(aura_state_name(AURA_STATE_SPEAKING), "Speaking");
    AURA_ASSERT_STREQ(aura_trigger_name(AURA_TRIG_BARGE_IN), "barge_in");
}

/* 迁移表自检：表里不能有重复的 (from, trig)。 */
static void test_transition_table_unique(void)
{
    uint32_t n = aura_sm_transition_count();
    AURA_CHECK(n > 0);
    for (uint32_t i = 0; i < n; i++) {
        for (uint32_t j = i + 1; j < n; j++) {
            const aura_sm_transition_t *a = &aura_sm_transition_table()[i];
            const aura_sm_transition_t *b = &aura_sm_transition_table()[j];
            AURA_CHECK(!(a->from == b->from && a->trig == b->trig));
        }
    }
}

AURA_TEST(test_happy_path);
AURA_TEST(test_voiceprint_reject);
AURA_TEST(test_error_and_auto_reset);
AURA_TEST(test_illegal_transitions_rejected);
AURA_TEST(test_speaking_tts_done_returns_listening);
AURA_TEST(test_trigger_from_event);
AURA_TEST(test_names);
AURA_TEST(test_transition_table_unique);

AURA_TEST_MAIN();
