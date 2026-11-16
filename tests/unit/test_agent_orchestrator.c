/* Aura — Phase B2 单测：tool_runner + orchestrator + LLM 抽象
 *
 * 覆盖：
 *   - tool_runner：重试、call_id 幂等（重复投递不重复执行副作用）、未知工具；
 *   - orchestrator：asr.final → dialogue → tool.call → tool.result → tts.say；
 *   - LLM 请求工具 → tool 循环 → 最终回复；
 *   - barge-in：取消后旧 turn 的 tool.result 被丢弃，不再产生 tts.say。
 */
#include "aura_test.h"

#include "agent/agent_stages.h"
#include "agent/flow/flow_bus.h"
#include "agent/flow/flow_middleware.h"
#include "agent/flow/flow_stage.h"
#include "agent/flow/flow_store.h"
#include "agent/llm/llm.h"
#include "agent/stages/stage_proto.h"
#include "agent/stages/tool_runner_stage.h"

/* ------------------------------------------------------------ 记录器 */

#define REC_MAX_ENTRIES 128
#define REC_TOPIC_MAX   64
#define REC_PAYLOAD_MAX 512

typedef struct {
    char topic[REC_TOPIC_MAX];
    char payload[REC_PAYLOAD_MAX];
} rec_entry_t;

static rec_entry_t   g_rec[REC_MAX_ENTRIES];
static uint32_t      g_rec_count;
static aura_mutex_t *g_rec_lock;

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

static bool wait_count(const char *topic, uint32_t min_count, int timeout_ms)
{
    for (int waited = 0; waited < timeout_ms; waited += 10) {
        if (rec_count(topic) >= min_count) {
            return true;
        }
        aura_test_sleep_ms(10);
    }
    return rec_count(topic) >= min_count;
}

/* ------------------------------------------------------------ 工具 */

static int g_count_exec;
static int g_flaky_attempts;
static int g_blocking_ms;

static aura_err_t tool_count(const aura_stage_kv_t *args, uint32_t nargs, char *out,
                             uint32_t cap, void *user)
{
    (void)args;
    (void)nargs;
    (void)user;
    g_count_exec++;
    snprintf(out, cap, "count=%d", g_count_exec);
    return AURA_OK;
}

static aura_err_t tool_flaky(const aura_stage_kv_t *args, uint32_t nargs, char *out,
                             uint32_t cap, void *user)
{
    (void)args;
    (void)nargs;
    (void)user;
    g_flaky_attempts++;
    if (g_flaky_attempts == 1) {
        return AURA_ERR_AGAIN;
    }
    snprintf(out, cap, "ok after %d", g_flaky_attempts);
    return AURA_OK;
}

static aura_err_t tool_blocking(const aura_stage_kv_t *args, uint32_t nargs, char *out,
                                uint32_t cap, void *user)
{
    (void)args;
    (void)nargs;
    (void)user;
    aura_osal_sleep_ms((uint32_t)g_blocking_ms);
    snprintf(out, cap, "slow done");
    return AURA_OK;
}

/* ------------------------------------------------------------ 夹具 */

static aura_err_t pub(aura_flow_bus_t *bus, const char *topic, const char *payload,
                      uint32_t session, uint32_t turn)
{
    aura_flow_msg_t msg;
    memset(&msg, 0, sizeof(msg));
    msg.kind           = AURA_FLOW_EVENT;
    msg.ctx.session_id = session;
    msg.ctx.turn_id    = turn;
    snprintf(msg.topic, sizeof(msg.topic), "%s", topic);
    snprintf(msg.payload, sizeof(msg.payload), "%s", payload);
    msg.len = (uint32_t)strlen(msg.payload);
    return aura_flow_publish(bus, &msg);
}

typedef struct {
    aura_flow_bus_t     *bus;
    aura_flow_store_t   *store;
    aura_agent_stages_t *stages;
    aura_flow_stage_t   *recorder;
} fixture_t;

static bool fixture_up(fixture_t *f, const aura_llm_backend_t *backend)
{
    memset(f, 0, sizeof(*f));
    f->bus = aura_flow_bus_create(32, AURA_FLOW_DROP_OLDEST);
    if (f->bus == NULL) {
        return false;
    }
    if (aura_flow_mw_add(f->bus, "cancel_gate", aura_flow_mw_cancel_gate, NULL) != AURA_OK) {
        return false;
    }
    f->store  = aura_flow_store_create();
    f->stages = aura_agent_stages_create(f->bus, f->store, NULL);
    if (f->store == NULL || f->stages == NULL) {
        return false;
    }
    if (backend != NULL && aura_agent_stages_set_llm_backend(f->stages, backend) != AURA_OK) {
        return false;
    }

    rec_reset();
    f->recorder = aura_flow_stage_create(f->bus, "recorder", rec_handler, NULL, 128,
                                         AURA_FLOW_DROP_OLDEST);
    if (f->recorder == NULL || aura_flow_stage_subscribe(f->recorder, "") != AURA_OK ||
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

static bool start_utterance(fixture_t *f)
{
    if (pub(f->bus, AURA_TOPIC_SESSION_WAKE, "", 0, 0) != AURA_OK) {
        return false;
    }
    aura_test_sleep_ms(80);
    if (pub(f->bus, AURA_TOPIC_SESSION_VOICEPRINT, "passed=1\nscore=90", 0, 0) != AURA_OK) {
        return false;
    }
    if (pub(f->bus, AURA_TOPIC_VAD_START, "", 0, 0) != AURA_OK) {
        return false;
    }
    aura_test_sleep_ms(80);
    return true;
}

/* ---------------------------------------------------------------- 用例 */

AURA_TEST(test_tool_runner_retry_and_idempotency)
{
    g_count_exec     = 0;
    g_flaky_attempts = 0;

    aura_flow_bus_t *bus = aura_flow_bus_create(16, AURA_FLOW_DROP_OLDEST);
    AURA_ASSERT(bus != NULL);
    aura_tool_registry_t *tools = aura_tool_registry_create();
    AURA_ASSERT(tools != NULL);
    AURA_ASSERT_EQ(aura_tool_registry_register(tools, "count", "count side effects",
                                               tool_count, NULL),
                   AURA_OK);
    AURA_ASSERT_EQ(aura_tool_registry_register(tools, "flaky", "retry demo", tool_flaky, NULL),
                   AURA_OK);

    aura_tool_runner_stage_t *runner = aura_tool_runner_stage_create(bus, tools);
    AURA_ASSERT(runner != NULL);
    rec_reset();
    aura_flow_stage_t *recorder = aura_flow_stage_create(bus, "recorder", rec_handler, NULL,
                                                         64, AURA_FLOW_DROP_OLDEST);
    AURA_ASSERT(recorder != NULL);
    AURA_ASSERT_EQ(aura_flow_stage_subscribe(recorder, ""), AURA_OK);
    AURA_ASSERT_EQ(aura_flow_stage_start(recorder), AURA_OK);
    AURA_ASSERT_EQ(aura_tool_runner_stage_start(runner), AURA_OK);

    /* 首次执行 */
    AURA_ASSERT_EQ(pub(bus, AURA_TOPIC_TOOL_CALL, "name=count\ncall_id=c1\n", 1, 1), AURA_OK);
    AURA_ASSERT(wait_count(AURA_TOPIC_TOOL_RESULT, 1, 1000));
    AURA_ASSERT_EQ(g_count_exec, 1);
    char payload[REC_PAYLOAD_MAX];
    AURA_ASSERT(rec_last(AURA_TOPIC_TOOL_RESULT, payload, sizeof(payload)));
    AURA_ASSERT(strstr(payload, "status=ok") != NULL);
    AURA_ASSERT(strstr(payload, "count=1") != NULL);

    /* 同一 call_id 重复投递：命中幂等缓存，不再执行 */
    AURA_ASSERT_EQ(pub(bus, AURA_TOPIC_TOOL_CALL, "name=count\ncall_id=c1\n", 1, 1), AURA_OK);
    AURA_ASSERT(wait_count(AURA_TOPIC_TOOL_RESULT, 2, 1000));
    AURA_ASSERT_EQ(g_count_exec, 1);

    /* AURA_ERR_AGAIN：默认 max_retries=1，重试一次后成功 */
    AURA_ASSERT_EQ(pub(bus, AURA_TOPIC_TOOL_CALL, "name=flaky\ncall_id=c2\n", 1, 1), AURA_OK);
    AURA_ASSERT(wait_count(AURA_TOPIC_TOOL_RESULT, 3, 1000));
    AURA_ASSERT_EQ(g_flaky_attempts, 2);
    AURA_ASSERT(rec_last(AURA_TOPIC_TOOL_RESULT, payload, sizeof(payload)));
    AURA_ASSERT(strstr(payload, "status=ok") != NULL);
    AURA_ASSERT(strstr(payload, "retries=1") != NULL);

    /* 未知工具 */
    AURA_ASSERT_EQ(pub(bus, AURA_TOPIC_TOOL_CALL, "name=nope\ncall_id=c3\n", 1, 1), AURA_OK);
    AURA_ASSERT(wait_count(AURA_TOPIC_TOOL_RESULT, 4, 1000));
    AURA_ASSERT(rec_last(AURA_TOPIC_TOOL_RESULT, payload, sizeof(payload)));
    AURA_ASSERT(strstr(payload, "status=error") != NULL);
    AURA_ASSERT(strstr(payload, "unknown") != NULL);

    aura_tool_runner_stage_destroy(runner);
    aura_flow_stage_destroy(recorder);
    aura_tool_registry_destroy(tools);
    aura_flow_bus_destroy(bus);
}

AURA_TEST(test_orchestrator_weather_tool_loop)
{
    fixture_t f;
    AURA_ASSERT(fixture_up(&f, NULL));
    AURA_ASSERT(start_utterance(&f));

    AURA_ASSERT_EQ(pub(f.bus, AURA_TOPIC_ASR_FINAL, "今天天气怎么样", 0, 0), AURA_OK);
    AURA_ASSERT(wait_count(AURA_TOPIC_DIALOGUE_INTENT, 1, 1000));
    AURA_ASSERT(wait_count(AURA_TOPIC_TOOL_CALL, 1, 1000));
    AURA_ASSERT(wait_count(AURA_TOPIC_TOOL_RESULT, 1, 1000));
    AURA_ASSERT(wait_count(AURA_TOPIC_TTS_SAY, 1, 1500));

    char payload[REC_PAYLOAD_MAX];
    AURA_ASSERT(rec_last(AURA_TOPIC_TTS_SAY, payload, sizeof(payload)));
    AURA_ASSERT(strstr(payload, "当前天气") != NULL);
    AURA_ASSERT(strstr(payload, "晴") != NULL);

    /* assistant 轮次同时进短期记忆与长期存储 */
    char membuf[512];
    AURA_ASSERT_EQ(aura_agent_memory_recent(aura_agent_stages_memory(f.stages), 1, 1, membuf,
                                            sizeof(membuf)),
                   AURA_OK);
    AURA_ASSERT(strstr(membuf, "assistant:") != NULL);
    char stored[256];
    AURA_ASSERT_EQ(aura_flow_store_get(f.store, 1, "mem.assistant.last", stored,
                                       sizeof(stored)),
                   AURA_OK);

    /* 第二个 turn：纯聊天，不触发工具 */
    uint32_t tool_calls = rec_count(AURA_TOPIC_TOOL_CALL);
    AURA_ASSERT_EQ(pub(f.bus, AURA_TOPIC_VAD_START, "", 0, 0), AURA_OK);
    aura_test_sleep_ms(80);
    AURA_ASSERT_EQ(pub(f.bus, AURA_TOPIC_ASR_FINAL, "你好", 0, 0), AURA_OK);
    AURA_ASSERT(wait_count(AURA_TOPIC_TTS_SAY, 2, 1500));
    AURA_ASSERT_EQ(rec_count(AURA_TOPIC_TOOL_CALL), tool_calls);
    AURA_ASSERT(rec_last(AURA_TOPIC_TTS_SAY, payload, sizeof(payload)));
    AURA_ASSERT(strstr(payload, "收到：你好") != NULL);

    fixture_down(&f);
}

static int g_backend_calls;

static aura_err_t backend_requests_tool(void *user, const aura_llm_request_t *req,
                                        aura_llm_response_t *resp)
{
    (void)user;
    memset(resp, 0, sizeof(*resp));
    g_backend_calls++;
    if (req->tool_result == NULL) {
        snprintf(resp->tool_name, sizeof(resp->tool_name), "get_time");
        snprintf(resp->tool_args, sizeof(resp->tool_args), "arg.demo=1\n");
    } else {
        snprintf(resp->text, sizeof(resp->text), "时间已查：%s", req->tool_result);
    }
    return AURA_OK;
}

AURA_TEST(test_orchestrator_llm_requested_tool)
{
    g_backend_calls = 0;
    aura_llm_backend_t backend = {"custom", backend_requests_tool, NULL};
    fixture_t f;
    AURA_ASSERT(fixture_up(&f, &backend));
    AURA_ASSERT(start_utterance(&f));

    AURA_ASSERT_EQ(pub(f.bus, AURA_TOPIC_ASR_FINAL, "随便聊聊", 0, 0), AURA_OK);
    AURA_ASSERT(wait_count(AURA_TOPIC_TOOL_CALL, 1, 1000));
    AURA_ASSERT(wait_count(AURA_TOPIC_TTS_SAY, 1, 1500));
    AURA_ASSERT_EQ(g_backend_calls, 2);

    char payload[REC_PAYLOAD_MAX];
    AURA_ASSERT(rec_last(AURA_TOPIC_TTS_SAY, payload, sizeof(payload)));
    AURA_ASSERT(strstr(payload, "时间已查") != NULL);

    fixture_down(&f);
}

AURA_TEST(test_barge_in_drops_old_tool_result)
{
    g_blocking_ms = 300;
    fixture_t f;
    AURA_ASSERT(fixture_up(&f, NULL));

    /* 用阻塞工具覆盖 set_volume，制造"工具在途时被打断"的窗口。 */
    AURA_ASSERT_EQ(aura_tool_registry_register(aura_agent_stages_tools(f.stages), "set_volume",
                                               "blocking demo", tool_blocking, NULL),
                   AURA_OK);
    AURA_ASSERT(start_utterance(&f));

    AURA_ASSERT_EQ(pub(f.bus, AURA_TOPIC_ASR_FINAL, "音量调到50", 0, 0), AURA_OK);
    AURA_ASSERT(wait_count(AURA_TOPIC_TOOL_CALL, 1, 1000));

    /* 工具仍在执行时 barge-in：cancel_turn(1, 1)。 */
    aura_agent_stages_on_barge_in(f.stages);
    aura_test_sleep_ms(500); /* 等阻塞工具返回并发布 tool.result */

    AURA_ASSERT_EQ(rec_count(AURA_TOPIC_TOOL_RESULT), 0);
    AURA_ASSERT_EQ(rec_count(AURA_TOPIC_TTS_SAY), 0);

    fixture_down(&f);
}

AURA_TEST_MAIN()
