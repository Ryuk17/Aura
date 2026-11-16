/* Aura — flow_chain：Agent 数据流闭环仿真（Phase B 出口验收）
 *
 * 与 host_sim 的分工：
 *   host_sim  ：mock 节点驱动 pipeline/event_bus/状态机，验证音频面闭环；
 *   flow_chain：真实 event_bus → flow_bridge → 真实 stage（session/dialogue/
 *               tool_runner/orchestrator/memory），验证文本面闭环。
 *
 * 链路：KWS_HIT → VOICEPRINT → VAD_START → ASR_FINAL
 *        → dialogue.intent → tool.call → tool.result → tts.say
 * 另外验证：trace_id、persist 落库、barge-in 取消旧 turn。
 */
#include "aura_test.h"

#include "agent/agent_core.h"
#include "agent/agent_stages.h"
#include "agent/flow/flow_stage.h"
#include "agent/flow/flow_store.h"
#include "agent_export.h"

#define REC_MAX_ENTRIES 128

typedef struct {
    char     topic[64];
    char     payload[512];
    uint32_t trace_id;
    uint32_t session_id;
    uint32_t turn_id;
} rec_entry_t;

static rec_entry_t   g_rec[REC_MAX_ENTRIES];
static uint32_t      g_rec_count;
static aura_mutex_t *g_rec_lock;

static aura_err_t rec_handler(aura_flow_msg_t *msg, void *user)
{
    (void)user;
    aura_osal_mutex_lock(g_rec_lock);
    if (g_rec_count < REC_MAX_ENTRIES) {
        rec_entry_t *e = &g_rec[g_rec_count++];
        snprintf(e->topic, sizeof(e->topic), "%s", msg->topic);
        snprintf(e->payload, sizeof(e->payload), "%.*s", (int)(sizeof(e->payload) - 1),
                 msg->payload);
        e->trace_id   = msg->ctx.trace_id;
        e->session_id = msg->ctx.session_id;
        e->turn_id    = msg->ctx.turn_id;
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

static bool rec_last(const char *topic, rec_entry_t *out)
{
    bool found = false;
    aura_osal_mutex_lock(g_rec_lock);
    for (uint32_t i = g_rec_count; i > 0; i--) {
        if (strcmp(g_rec[i - 1].topic, topic) == 0) {
            *out  = g_rec[i - 1];
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

static int g_blocking_ms;

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

/* pipeline 至少需要一个音频消费者才允许 start；flow_chain 不喂音频，
 * 用一个最小 sink 节点满足拓扑约束。 */
static aura_err_t sink_process_audio(aura_node_t *self, const aura_audio_frame_t *frame)
{
    (void)self;
    (void)frame;
    return AURA_OK;
}

static const aura_node_ops_t kSinkOps = {.process_audio = sink_process_audio};

static aura_node_t *sink_create(void)
{
    static struct {
        aura_node_t base;
    } node;
    static bool init_done = false;
    if (!init_done) {
        memset(&node, 0, sizeof(node));
        aura_node_caps_t caps;
        memset(&caps, 0, sizeof(caps));
        caps.consumes_audio = true;
        aura_node_init(&node.base, "flow_sink", &kSinkOps, &caps);
        init_done = true;
    }
    return &node.base;
}

static void post_voiceprint(aura_event_bus_t *bus, bool passed, int32_t score)
{
    aura_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type                  = AURA_EVENT_VOICEPRINT_RESULT;
    ev.u.voiceprint.passed   = passed;
    ev.u.voiceprint.score    = score;
    (void)aura_event_bus_publish(bus, &ev);
}

AURA_TEST(test_flow_chain_weather_loop)
{
    g_blocking_ms = 400;
    g_rec_lock    = aura_osal_mutex_create();
    AURA_ASSERT(g_rec_lock != NULL);
    g_rec_count = 0;

    aura_agent_config_t cfg;
    aura_agent_config_default(&cfg);
    AURA_ASSERT_EQ(aura_agent_init(&cfg), AURA_OK);

    /* 用阻塞版 set_volume 制造"工具在途时被打断"的窗口。 */
    aura_tool_registry_t *tools =
        aura_agent_stages_tools(aura_agent_stages_get());
    AURA_ASSERT(tools != NULL);
    AURA_ASSERT_EQ(aura_tool_registry_register(tools, "set_volume", "blocking demo",
                                               tool_blocking, NULL),
                   AURA_OK);
    AURA_ASSERT_EQ(aura_agent_add_node(sink_create()), AURA_OK);

    aura_flow_stage_t *recorder = aura_flow_stage_create(aura_agent_flow(), "recorder",
                                                         rec_handler, NULL, 128,
                                                         AURA_FLOW_DROP_OLDEST);
    AURA_ASSERT(recorder != NULL);
    AURA_ASSERT_EQ(aura_flow_stage_subscribe(recorder, ""), AURA_OK);
    AURA_ASSERT_EQ(aura_flow_stage_start(recorder), AURA_OK);

    AURA_ASSERT_EQ(aura_agent_start(), AURA_OK);
    aura_event_bus_t *bus = aura_agent_bus();
    AURA_ASSERT(bus != NULL);

    /* 唤醒 → 声纹 → VAD → ASR */
    AURA_ASSERT_EQ(aura_event_bus_post(bus, AURA_EVENT_KWS_HIT, 0, NULL), AURA_OK);
    AURA_ASSERT(wait_count("session.ready", 1, 1000));
    post_voiceprint(bus, true, 90);
    AURA_ASSERT(wait_count("session.speaker", 1, 1000));
    AURA_ASSERT_EQ(aura_event_bus_post(bus, AURA_EVENT_VAD_SPEECH_START, 0, NULL), AURA_OK);
    AURA_ASSERT(wait_count("session.turn.start", 1, 1000));
    AURA_ASSERT_EQ(aura_event_bus_post(bus, AURA_EVENT_ASR_FINAL, 0, "今天天气怎么样"),
                   AURA_OK);

    /* 数据流闭环 */
    AURA_ASSERT(wait_count("dialogue.intent", 1, 1500));
    AURA_ASSERT(wait_count("tool.call", 1, 1500));
    AURA_ASSERT(wait_count("tool.result", 1, 1500));
    AURA_ASSERT(wait_count("tts.say", 1, 2000));

    rec_entry_t e;
    AURA_ASSERT(rec_last("dialogue.intent", &e));
    AURA_ASSERT(strstr(e.payload, "intent=weather") != NULL);
    AURA_ASSERT(e.trace_id != 0);
    AURA_ASSERT_EQ(e.session_id, 1);
    AURA_ASSERT_EQ(e.turn_id, 1);

    AURA_ASSERT(rec_last("tts.say", &e));
    AURA_ASSERT(strstr(e.payload, "当前天气") != NULL);

    /* persist 中间件：带 ctx 的 dialogue.intent / tts.say 已落 flow_store；
     * 原始 asr.final 由 bridge 发出、无 session ctx，由 memory 路径落库。 */
    aura_flow_store_t *store = aura_agent_store();
    AURA_ASSERT(store != NULL);
    char buf[512];
    AURA_ASSERT_EQ(aura_flow_store_get(store, 1, "dialogue.intent", buf, sizeof(buf)),
                   AURA_OK);
    AURA_ASSERT(strstr(buf, "intent=weather") != NULL);
    AURA_ASSERT_EQ(aura_flow_store_get(store, 1, "tts.say", buf, sizeof(buf)), AURA_OK);
    AURA_ASSERT(strstr(buf, "当前天气") != NULL);
    AURA_ASSERT_EQ(aura_flow_store_get(store, 1, "mem.user.last", buf, sizeof(buf)), AURA_OK);
    AURA_ASSERT_STREQ(buf, "今天天气怎么样");

    /* barge-in：turn 2 的工具还在执行，旧 turn 的 tool.result 必须被丢弃。 */
    uint32_t tts_before = rec_count("tts.say");
    AURA_ASSERT_EQ(aura_event_bus_post(bus, AURA_EVENT_VAD_SPEECH_START, 0, NULL), AURA_OK);
    AURA_ASSERT(wait_count("session.turn.start", 2, 1000));
    AURA_ASSERT_EQ(aura_event_bus_post(bus, AURA_EVENT_ASR_FINAL, 0, "音量调到50"), AURA_OK);
    AURA_ASSERT(wait_count("tool.call", 2, 1500));

    AURA_ASSERT_EQ(aura_event_bus_post(bus, AURA_EVENT_BARGE_IN, 0, "flow_chain"), AURA_OK);
    aura_test_sleep_ms(900); /* 等阻塞工具返回并通过 cancel_gate */

    AURA_ASSERT_EQ(rec_count("tts.say"), tts_before);
    AURA_ASSERT_EQ(rec_count("tool.result"), 1);

    aura_flow_stage_destroy(recorder);
    AURA_ASSERT_EQ(aura_agent_stop(), AURA_OK);
    AURA_ASSERT_EQ(aura_agent_deinit(), AURA_OK);
    aura_osal_mutex_destroy(g_rec_lock);
}

AURA_TEST_MAIN()
