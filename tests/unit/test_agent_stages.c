/* Aura — Phase B1 stage 单测
 *
 * 覆盖：
 *   - session runtime：唤醒/新建会话/turn/声纹/barge-in/Idle；
 *   - session stage：session.ready、session.speaker、turn.start/end；
 *   - dialogue stage：asr.final → dialogue.intent（意图/槽位）；
 *   - memory：短期环 + memory stage 落 flow_store；
 *   - barge-in：cancel_turn 后旧 turn 消息被 cancel_gate 丢弃。
 */
#include "aura_test.h"

#include "agent/agent_stages.h"
#include "agent/flow/flow_bus.h"
#include "agent/flow/flow_middleware.h"
#include "agent/flow/flow_stage.h"
#include "agent/flow/flow_store.h"
#include "agent/memory/memory.h"
#include "agent/stages/session_runtime.h"
#include "agent/stages/stage_proto.h"

/* ------------------------------------------------------------ 消息记录器 */

#define REC_MAX_ENTRIES 64
#define REC_TOPIC_MAX   64
#define REC_PAYLOAD_MAX 512

typedef struct {
    char topic[REC_TOPIC_MAX];
    char payload[REC_PAYLOAD_MAX];
} rec_entry_t;

static rec_entry_t    g_rec[REC_MAX_ENTRIES];
static uint32_t       g_rec_count;
static aura_mutex_t  *g_rec_lock;

static void rec_init(void)
{
    if (g_rec_lock == NULL) {
        g_rec_lock = aura_osal_mutex_create();
    }
}

static void rec_reset(void)
{
    rec_init();
    aura_osal_mutex_lock(g_rec_lock);
    g_rec_count = 0;
    aura_osal_mutex_unlock(g_rec_lock);
}

static aura_err_t rec_handler(aura_flow_msg_t *msg, void *user)
{
    (void)user;
    aura_osal_mutex_lock(g_rec_lock);
    if (g_rec_count < REC_MAX_ENTRIES) {
        snprintf(g_rec[g_rec_count].topic, sizeof(g_rec[g_rec_count].topic), "%s", msg->topic);
        snprintf(g_rec[g_rec_count].payload, sizeof(g_rec[g_rec_count].payload), "%.*s",
                 (int)(sizeof(g_rec[g_rec_count].payload) - 1), msg->payload);
        g_rec_count++;
    }
    aura_osal_mutex_unlock(g_rec_lock);
    return AURA_OK;
}

static uint32_t rec_count(const char *topic)
{
    uint32_t n = 0;
    aura_osal_mutex_lock(g_rec_lock);
    for (uint32_t i = 0; i < g_rec_count; i++) {
        if (strcmp(g_rec[i].topic, topic) == 0) {
            n++;
        }
    }
    aura_osal_mutex_unlock(g_rec_lock);
    return n;
}

static bool rec_last(const char *topic, char *out, size_t cap)
{
    bool found = false;
    aura_osal_mutex_lock(g_rec_lock);
    for (uint32_t i = g_rec_count; i > 0; i--) {
        if (strcmp(g_rec[i - 1].topic, topic) == 0) {
            snprintf(out, cap, "%s", g_rec[i - 1].payload);
            found = true;
            break;
        }
    }
    aura_osal_mutex_unlock(g_rec_lock);
    return found;
}

/* ------------------------------------------------------------ 测试夹具 */

static aura_err_t pub(aura_flow_bus_t *bus, const char *topic, const char *payload,
                      uint32_t session, uint32_t turn)
{
    aura_flow_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.kind            = AURA_FLOW_EVENT;
    msg.ctx.session_id  = session;
    msg.ctx.turn_id     = turn;
    snprintf(msg.topic, sizeof(msg.topic), "%s", topic);
    snprintf(msg.payload, sizeof(msg.payload), "%s", payload);
    msg.len = (uint32_t)strlen(msg.payload);
    return aura_flow_publish(bus, &msg);
}

typedef struct {
    aura_flow_bus_t        *bus;
    aura_flow_store_t      *store;
    aura_agent_stages_t    *stages;
    aura_flow_stage_t      *recorder;
} fixture_t;

static bool fixture_up(fixture_t *f)
{
    memset(f, 0, sizeof(*f));
    f->bus = aura_flow_bus_create(32, AURA_FLOW_DROP_OLDEST);
    if (f->bus == NULL) {
        return false;
    }
    if (aura_flow_mw_add(f->bus, "cancel_gate", aura_flow_mw_cancel_gate, NULL) != AURA_OK) {
        return false;
    }
    f->store = aura_flow_store_create();
    f->stages = aura_agent_stages_create(f->bus, f->store, NULL);
    if (f->store == NULL || f->stages == NULL) {
        return false;
    }

    rec_reset();
    f->recorder = aura_flow_stage_create(f->bus, "recorder", rec_handler, NULL, 64,
                                         AURA_FLOW_DROP_OLDEST);
    if (f->recorder == NULL ||
        aura_flow_stage_subscribe(f->recorder, "") != AURA_OK ||
        aura_flow_stage_start(f->recorder) != AURA_OK ||
        aura_agent_stages_start(f->stages) != AURA_OK) {
        return false;
    }
    return true;
}

static void fixture_down(fixture_t *f)
{
    aura_agent_stages_destroy(f->stages);
    aura_flow_stage_destroy(f->recorder);
    aura_flow_store_destroy(f->store);
    aura_flow_bus_destroy(f->bus);
    memset(f, 0, sizeof(*f));
}

/* ---------------------------------------------------------------- 用例 */

AURA_TEST(test_stage_kv_parse)
{
    const char *payload = "# comment\r\npassed=1\r\nscore=95\r\nempty=\r\n";
    aura_stage_kv_t kvs[AURA_STAGE_KV_MAX];
    uint32_t n = aura_stage_kv_parse(payload, kvs, AURA_STAGE_KV_MAX);
    AURA_ASSERT_EQ(n, 3);
    AURA_ASSERT_STREQ(aura_stage_kv_get(kvs, n, "passed"), "1");
    AURA_ASSERT_STREQ(aura_stage_kv_get(kvs, n, "score"), "95");
    AURA_ASSERT_STREQ(aura_stage_kv_get(kvs, n, "empty"), "");
    AURA_ASSERT(aura_stage_kv_get(kvs, n, "missing") == NULL);

    int32_t v = 0;
    AURA_ASSERT(aura_stage_kv_get_int(kvs, n, "score", &v));
    AURA_ASSERT_EQ(v, 95);
    AURA_ASSERT(!aura_stage_kv_get_int(kvs, n, "missing", &v));
}

AURA_TEST(test_session_runtime)
{
    aura_session_runtime_t *rt = aura_session_runtime_create();
    AURA_ASSERT(rt != NULL);

    aura_flow_ctx_t ctx;
    AURA_ASSERT(!aura_session_runtime_ensure_turn(rt, &ctx));

    uint32_t sid = 0;
    bool     is_new = false;
    AURA_ASSERT(aura_session_runtime_on_wake(rt, &sid, &is_new));
    AURA_ASSERT_EQ(sid, 1);
    AURA_ASSERT(is_new);

    AURA_ASSERT(aura_session_runtime_begin_turn(rt, &ctx));
    AURA_ASSERT_EQ(ctx.session_id, 1);
    AURA_ASSERT_EQ(ctx.turn_id, 1);

    uint32_t speaker = 0;
    AURA_ASSERT(aura_session_runtime_on_voiceprint(rt, true, 90, &speaker));
    AURA_ASSERT_EQ(speaker, 1);

    AURA_ASSERT(aura_session_runtime_on_barge_in(rt, &ctx));
    AURA_ASSERT_EQ(ctx.turn_id, 1);
    AURA_ASSERT(!aura_session_runtime_on_barge_in(rt, &ctx)); /* 同一 turn 只取消一次 */

    AURA_ASSERT(aura_session_runtime_begin_turn(rt, &ctx));
    AURA_ASSERT_EQ(ctx.turn_id, 2);
    AURA_ASSERT_EQ(ctx.speaker_id, 1);

    aura_session_runtime_on_idle(rt);
    AURA_ASSERT(!aura_session_runtime_is_awake(rt));
    AURA_ASSERT(aura_session_runtime_on_wake(rt, &sid, &is_new));
    AURA_ASSERT_EQ(sid, 2);
    AURA_ASSERT(is_new);

    aura_session_runtime_destroy(rt);
}

AURA_TEST(test_memory_ring)
{
    aura_agent_memory_t *mem = aura_agent_memory_create();
    AURA_ASSERT(mem != NULL);

    char buf[512];
    AURA_ASSERT_EQ(aura_agent_memory_recent(mem, 1, 1, buf, sizeof(buf)), AURA_ERR_NOT_FOUND);

    AURA_ASSERT_EQ(aura_agent_memory_add(mem, 1, 1, "user", "你好"), AURA_OK);
    AURA_ASSERT_EQ(aura_agent_memory_add(mem, 1, 1, "assistant", "你好，有什么可以帮你？"),
                   AURA_OK);
    AURA_ASSERT_EQ(aura_agent_memory_recent(mem, 1, 1, buf, sizeof(buf)), AURA_OK);
    AURA_ASSERT(strstr(buf, "user: 你好") != NULL);
    AURA_ASSERT(strstr(buf, "assistant: 你好，有什么可以帮你？") != NULL);

    /* speaker 隔离 */
    AURA_ASSERT_EQ(aura_agent_memory_recent(mem, 1, 2, buf, sizeof(buf)), AURA_ERR_NOT_FOUND);

    /* 环容量：写入超过 AURA_MEMORY_TURNS 条后仍只保留最新 N 条 */
    for (int i = 0; i < AURA_MEMORY_TURNS + 2; i++) {
        char text[32];
        snprintf(text, sizeof(text), "t%d", i);
        AURA_ASSERT_EQ(aura_agent_memory_add(mem, 1, 1, "user", text), AURA_OK);
    }
    AURA_ASSERT_EQ(aura_agent_memory_recent(mem, 1, 1, buf, sizeof(buf)), AURA_OK);
    AURA_ASSERT(strstr(buf, "t0") == NULL);
    AURA_ASSERT(strstr(buf, "t9") != NULL);

    aura_agent_memory_clear_session(mem, 1);
    AURA_ASSERT_EQ(aura_agent_memory_recent(mem, 1, 1, buf, sizeof(buf)), AURA_ERR_NOT_FOUND);
    AURA_ASSERT_EQ(aura_agent_memory_add(mem, 0, 1, "user", "x"), AURA_ERR_INVALID_ARG);

    aura_agent_memory_destroy(mem);
}

AURA_TEST(test_session_dialogue_memory_flow)
{
    fixture_t f;
    AURA_ASSERT(fixture_up(&f));
    char payload[REC_PAYLOAD_MAX];

    /* 唤醒 */
    AURA_ASSERT_EQ(pub(f.bus, AURA_TOPIC_SESSION_WAKE, "", 0, 0), AURA_OK);
    aura_test_sleep_ms(150);
    AURA_ASSERT_EQ(rec_count(AURA_TOPIC_SESSION_READY), 1);
    AURA_ASSERT(rec_last(AURA_TOPIC_SESSION_READY, payload, sizeof(payload)));
    AURA_ASSERT(strstr(payload, "new=1") != NULL);
    AURA_ASSERT_EQ(aura_session_runtime_session_id(aura_agent_stages_session(f.stages)), 1);

    /* 声纹 */
    AURA_ASSERT_EQ(pub(f.bus, AURA_TOPIC_SESSION_VOICEPRINT, "passed=1\nscore=90", 0, 0),
                   AURA_OK);
    aura_test_sleep_ms(120);
    AURA_ASSERT_EQ(rec_count(AURA_TOPIC_SESSION_SPEAKER), 1);
    AURA_ASSERT(rec_last(AURA_TOPIC_SESSION_SPEAKER, payload, sizeof(payload)));
    AURA_ASSERT(strstr(payload, "speaker=1") != NULL);
    AURA_ASSERT(strstr(payload, "passed=1") != NULL);

    /* VAD 起点 → 新 turn */
    AURA_ASSERT_EQ(pub(f.bus, AURA_TOPIC_VAD_START, "", 0, 0), AURA_OK);
    aura_test_sleep_ms(120);
    AURA_ASSERT_EQ(rec_count(AURA_TOPIC_TURN_START), 1);
    AURA_ASSERT(rec_last(AURA_TOPIC_TURN_START, payload, sizeof(payload)));
    AURA_ASSERT(strstr(payload, "turn=1") != NULL);

    /* ASR 最终结果 → 意图 */
    AURA_ASSERT_EQ(pub(f.bus, AURA_TOPIC_ASR_FINAL, "今天天气怎么样", 0, 0), AURA_OK);
    aura_test_sleep_ms(250);
    AURA_ASSERT_EQ(rec_count(AURA_TOPIC_DIALOGUE_INTENT), 1);
    AURA_ASSERT(rec_last(AURA_TOPIC_DIALOGUE_INTENT, payload, sizeof(payload)));
    AURA_ASSERT(strstr(payload, "intent=weather") != NULL);
    AURA_ASSERT(strstr(payload, "tool=get_weather") != NULL);

    /* 短期记忆 + 长期存储 */
    char membuf[512];
    AURA_ASSERT_EQ(aura_agent_memory_recent(aura_agent_stages_memory(f.stages), 1, 1, membuf,
                                            sizeof(membuf)),
                   AURA_OK);
    AURA_ASSERT(strstr(membuf, "user: 今天天气怎么样") != NULL);

    /* dialogue 写 user，orchestrator 写 assistant；这里只关心 user 已落库。 */
    AURA_ASSERT(rec_count(AURA_TOPIC_MEMORY_STORED) >= 1);
    char stored[256];
    AURA_ASSERT_EQ(aura_flow_store_get(f.store, 1, "mem.user.last", stored, sizeof(stored)),
                   AURA_OK);
    AURA_ASSERT_STREQ(stored, "今天天气怎么样");

    /* barge-in：取消 turn 1，旧 turn 的 asr.final 必须被丢弃 */
    aura_agent_stages_on_barge_in(f.stages);
    uint32_t intent_before = rec_count(AURA_TOPIC_DIALOGUE_INTENT);
    AURA_ASSERT_EQ(pub(f.bus, AURA_TOPIC_ASR_FINAL, "被打断的话", 1, 1), AURA_OK);
    aura_test_sleep_ms(200);
    AURA_ASSERT_EQ(rec_count(AURA_TOPIC_DIALOGUE_INTENT), intent_before);

    /* 下一次 VAD 起点开启新 turn（turn 2），消息恢复正常处理 */
    AURA_ASSERT_EQ(pub(f.bus, AURA_TOPIC_VAD_START, "", 0, 0), AURA_OK);
    aura_test_sleep_ms(120);
    AURA_ASSERT(rec_last(AURA_TOPIC_TURN_START, payload, sizeof(payload)));
    AURA_ASSERT(strstr(payload, "turn=2") != NULL);
    AURA_ASSERT_EQ(pub(f.bus, AURA_TOPIC_ASR_FINAL, "现在几点", 0, 0), AURA_OK);
    aura_test_sleep_ms(250);
    AURA_ASSERT(rec_last(AURA_TOPIC_DIALOGUE_INTENT, payload, sizeof(payload)));
    AURA_ASSERT(strstr(payload, "intent=time") != NULL);

    fixture_down(&f);
}

AURA_TEST(test_session_idle_closes)
{
    fixture_t f;
    AURA_ASSERT(fixture_up(&f));
    char payload[REC_PAYLOAD_MAX];

    AURA_ASSERT_EQ(pub(f.bus, AURA_TOPIC_SESSION_WAKE, "", 0, 0), AURA_OK);
    aura_test_sleep_ms(120);
    AURA_ASSERT(aura_session_runtime_is_awake(aura_agent_stages_session(f.stages)));

    AURA_ASSERT_EQ(pub(f.bus, AURA_TOPIC_SESSION_STATE, "Speaking->Idle", 0, 0), AURA_OK);
    aura_test_sleep_ms(150);
    AURA_ASSERT(!aura_session_runtime_is_awake(aura_agent_stages_session(f.stages)));
    AURA_ASSERT_EQ(rec_count(AURA_TOPIC_SESSION_CLOSED), 1);
    AURA_ASSERT(rec_last(AURA_TOPIC_SESSION_CLOSED, payload, sizeof(payload)));
    AURA_ASSERT(strstr(payload, "reason=idle") != NULL);

    fixture_down(&f);
}

AURA_TEST_MAIN()
