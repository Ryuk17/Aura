/* Aura — Agent 数据流中间件单测
 *
 * 覆盖：topic 路由、stage 队列策略、中间件顺序、取消门、会话/存储、event_bus 桥。
 */
#include "aura_test.h"

#include <stdatomic.h>

#include "agent/flow/flow_bridge.h"
#include "agent/flow/flow_bus.h"
#include "agent/flow/flow_middleware.h"
#include "agent/flow/flow_session.h"
#include "agent/flow/flow_stage.h"
#include "agent/flow/flow_store.h"
#include "core/event_bus/event_bus.h"

static _Atomic int g_retry_calls;
static _Atomic int g_capture_count;
static char        g_capture[1024];

static aura_err_t handler_capture(aura_flow_msg_t *msg, void *user)
{
    (void)user;
    snprintf(g_capture, sizeof(g_capture), "%s", msg->payload);
    atomic_fetch_add(&g_capture_count, 1);
    return AURA_OK;
}

static aura_err_t handler_flaky(aura_flow_msg_t *msg, void *user)
{
    (void)msg;
    (void)user;
    int n = atomic_fetch_add(&g_retry_calls, 1);
    return (n == 0) ? AURA_ERR_AGAIN : AURA_OK;
}

static _Atomic int g_count_a;
static _Atomic int g_count_b;
static _Atomic int g_count_h;
static char        g_order[16];
static int         g_order_len;

static aura_err_t handler_a(aura_flow_msg_t *msg, void *user)
{
    (void)msg;
    (void)user;
    atomic_fetch_add(&g_count_a, 1);
    return AURA_OK;
}

static aura_err_t handler_b(aura_flow_msg_t *msg, void *user)
{
    (void)msg;
    (void)user;
    atomic_fetch_add(&g_count_b, 1);
    return AURA_OK;
}

static aura_err_t handler_order(aura_flow_msg_t *msg, void *user)
{
    (void)msg;
    (void)user;
    atomic_fetch_add(&g_count_h, 1);
    if (g_order_len < (int)sizeof(g_order) - 1) {
        g_order[g_order_len++] = 'H';
        g_order[g_order_len]   = '\0';
    }
    return AURA_OK;
}

static aura_err_t mw_letter(aura_flow_msg_t *msg, aura_flow_next_fn next, void *user)
{
    char c = *(const char *)aura_flow_mw_user(user);
    if (g_order_len < (int)sizeof(g_order) - 1) {
        g_order[g_order_len++] = c;
        g_order[g_order_len]   = '\0';
    }
    return next(msg, user);
}

static aura_err_t publish_text_raw(aura_flow_bus_t *bus, const char *topic, const char *text,
                                    uint32_t session, uint32_t turn)
{
    aura_flow_msg_t m;
    memset(&m, 0, sizeof(m));
    m.kind = AURA_FLOW_TEXT;
    m.ctx.session_id = session;
    m.ctx.turn_id    = turn;
    snprintf(m.topic, sizeof(m.topic), "%s", topic);
    snprintf(m.payload, sizeof(m.payload), "%s", text);
    m.len = (uint32_t)strlen(m.payload);
    return aura_flow_publish(bus, &m);
}

static void publish_text(aura_flow_bus_t *bus, const char *topic, const char *text,
                         uint32_t session, uint32_t turn)
{
    AURA_ASSERT_EQ(publish_text_raw(bus, topic, text, session, turn), AURA_OK);
}

AURA_TEST(test_flow_routing)
{
    atomic_store(&g_count_a, 0);
    atomic_store(&g_count_b, 0);
    aura_flow_bus_t *bus = aura_flow_bus_create(8, AURA_FLOW_DROP_OLDEST);
    AURA_ASSERT(bus != NULL);
    aura_flow_stage_t *a = aura_flow_stage_create(bus, "a", handler_a, NULL, 8,
                                                   AURA_FLOW_DROP_OLDEST);
    aura_flow_stage_t *b = aura_flow_stage_create(bus, "b", handler_b, NULL, 8,
                                                   AURA_FLOW_DROP_OLDEST);
    AURA_ASSERT(a != NULL && b != NULL);
    AURA_ASSERT_EQ(aura_flow_stage_subscribe(a, "asr.final"), AURA_OK);
    AURA_ASSERT_EQ(aura_flow_stage_subscribe(b, "llm.token"), AURA_OK);
    AURA_ASSERT_EQ(aura_flow_stage_start(a), AURA_OK);
    AURA_ASSERT_EQ(aura_flow_stage_start(b), AURA_OK);

    publish_text(bus, "asr.final", "hello", 1, 1);
    publish_text(bus, "asr.final", "world", 1, 1);
    publish_text(bus, "llm.token", "tok", 1, 1);
    aura_test_sleep_ms(200);

    AURA_ASSERT_EQ(atomic_load(&g_count_a), 2);
    AURA_ASSERT_EQ(atomic_load(&g_count_b), 1);

    aura_flow_stage_destroy(a);
    aura_flow_stage_destroy(b);
    aura_flow_bus_destroy(bus);
}

AURA_TEST(test_flow_backpressure)
{
    atomic_store(&g_count_b, 0);
    aura_flow_bus_t *bus = aura_flow_bus_create(1, AURA_FLOW_DROP_NEWEST);
    aura_flow_stage_t *b = aura_flow_stage_create(bus, "b", handler_b, NULL, 1,
                                                   AURA_FLOW_DROP_NEWEST);
    AURA_ASSERT(b != NULL);
    AURA_ASSERT_EQ(aura_flow_stage_subscribe(b, "t"), AURA_OK);
    /* 不 start：队列填满后第二条按 DROP_NEWEST 丢弃。 */
    AURA_ASSERT_EQ(publish_text_raw(bus, "t", "1", 0, 0), AURA_OK);
    AURA_ASSERT_EQ(publish_text_raw(bus, "t", "2", 0, 0), AURA_ERR_FULL);
    aura_flow_stage_stats_t st;
    aura_flow_stage_stats(b, &st);
    AURA_ASSERT_EQ(st.received, 2);
    AURA_ASSERT_EQ(st.dropped, 1);
    aura_flow_stage_destroy(b);
    aura_flow_bus_destroy(bus);
}

AURA_TEST(test_flow_cancel_gate)
{
    atomic_store(&g_count_a, 0);
    aura_flow_bus_t *bus = aura_flow_bus_create(8, AURA_FLOW_DROP_OLDEST);
    AURA_ASSERT_EQ(aura_flow_mw_add(bus, "cancel", aura_flow_mw_cancel_gate, NULL), AURA_OK);
    aura_flow_stage_t *a = aura_flow_stage_create(bus, "a", handler_a, NULL, 8,
                                                   AURA_FLOW_DROP_OLDEST);
    AURA_ASSERT_EQ(aura_flow_stage_subscribe(a, "t"), AURA_OK);
    AURA_ASSERT_EQ(aura_flow_stage_start(a), AURA_OK);

    AURA_ASSERT_EQ(aura_flow_cancel_turn(bus, 7, 1), AURA_OK);
    publish_text(bus, "t", "cancelled", 7, 1);
    publish_text(bus, "t", "alive", 7, 2);
    aura_test_sleep_ms(200);

    AURA_ASSERT_EQ(atomic_load(&g_count_a), 1);
    aura_flow_stats_t st;
    aura_flow_stats(bus, &st);
    AURA_ASSERT(st.cancelled >= 1);

    aura_flow_stage_destroy(a);
    aura_flow_bus_destroy(bus);
}

AURA_TEST(test_flow_middleware_order)
{
    g_order_len = 0;
    g_order[0]  = '\0';
    atomic_store(&g_count_h, 0);
    aura_flow_bus_t *bus = aura_flow_bus_create(8, AURA_FLOW_DROP_OLDEST);
    static char mw_a = 'A';
    static char mw_b = 'B';
    AURA_ASSERT_EQ(aura_flow_mw_add(bus, "A", mw_letter, &mw_a), AURA_OK);
    AURA_ASSERT_EQ(aura_flow_mw_add(bus, "B", mw_letter, &mw_b), AURA_OK);
    aura_flow_stage_t *s = aura_flow_stage_create(bus, "s", handler_order, NULL, 8,
                                                   AURA_FLOW_DROP_OLDEST);
    AURA_ASSERT_EQ(aura_flow_stage_subscribe(s, "t"), AURA_OK);
    AURA_ASSERT_EQ(aura_flow_stage_start(s), AURA_OK);

    publish_text(bus, "t", "x", 1, 1);
    aura_test_sleep_ms(200);

    AURA_ASSERT_EQ(atomic_load(&g_count_h), 1);
    AURA_ASSERT_STREQ(g_order, "ABH");

    aura_flow_stage_destroy(s);
    aura_flow_bus_destroy(bus);
}

AURA_TEST(test_flow_session)
{
    aura_flow_session_t s;
    aura_flow_session_init(&s, 42);
    AURA_ASSERT_EQ(s.id, 42);
    AURA_ASSERT_EQ(s.turn_id, 0);
    aura_flow_session_begin_turn(&s);
    AURA_ASSERT_EQ(s.turn_id, 1);
    aura_flow_session_cancel_turn(&s);
    AURA_ASSERT(aura_flow_session_turn_cancelled(&s));
    aura_flow_session_begin_turn(&s);
    AURA_ASSERT_EQ(s.turn_id, 2);
    AURA_ASSERT(!aura_flow_session_turn_cancelled(&s));
}

AURA_TEST(test_flow_store)
{
    aura_flow_store_t *store = aura_flow_store_create();
    AURA_ASSERT(store != NULL);
    AURA_ASSERT_EQ(aura_flow_store_put(store, 1, "k", "v1"), AURA_OK);
    char buf[64];
    AURA_ASSERT_EQ(aura_flow_store_get(store, 1, "k", buf, sizeof(buf)), AURA_OK);
    AURA_ASSERT_STREQ(buf, "v1");
    AURA_ASSERT_EQ(aura_flow_store_put(store, 1, "k", "v2"), AURA_OK);
    AURA_ASSERT_EQ(aura_flow_store_get(store, 1, "k", buf, sizeof(buf)), AURA_OK);
    AURA_ASSERT_STREQ(buf, "v2");
    AURA_ASSERT_EQ(aura_flow_store_get(store, 2, "k", buf, sizeof(buf)), AURA_ERR_NOT_FOUND);
    aura_flow_store_clear_session(store, 1);
    AURA_ASSERT_EQ(aura_flow_store_get(store, 1, "k", buf, sizeof(buf)), AURA_ERR_NOT_FOUND);
    aura_flow_store_destroy(store);
}

AURA_TEST(test_flow_bridge)
{
    atomic_store(&g_count_a, 0);
    aura_flow_bus_t *flow = aura_flow_bus_create(8, AURA_FLOW_DROP_OLDEST);
    aura_event_bus_t *ev = aura_event_bus_create(16);
    AURA_ASSERT(flow != NULL && ev != NULL);
    aura_flow_bridge_t *bridge = aura_flow_bridge_create(flow, ev);
    AURA_ASSERT(bridge != NULL);

    aura_flow_stage_t *a = aura_flow_stage_create(flow, "asr", handler_a, NULL, 8,
                                                   AURA_FLOW_DROP_OLDEST);
    AURA_ASSERT_EQ(aura_flow_stage_subscribe(a, "asr.final"), AURA_OK);
    AURA_ASSERT_EQ(aura_flow_stage_start(a), AURA_OK);

    AURA_ASSERT_EQ(aura_event_bus_post(ev, AURA_EVENT_ASR_FINAL, 0, "hello world"), AURA_OK);
    aura_test_sleep_ms(300);
    AURA_ASSERT_EQ(atomic_load(&g_count_a), 1);

    aura_flow_stage_destroy(a);
    aura_flow_bridge_destroy(bridge);
    aura_event_bus_destroy(ev);
    aura_flow_bus_destroy(flow);
}

AURA_TEST(test_flow_mw_validate_deadline_redact)
{
    atomic_store(&g_capture_count, 0);
    g_capture[0] = '\0';
    aura_flow_bus_t *bus = aura_flow_bus_create(8, AURA_FLOW_DROP_OLDEST);
    AURA_ASSERT(bus != NULL);
    AURA_ASSERT_EQ(aura_flow_mw_add(bus, "validate", aura_flow_mw_validate, NULL), AURA_OK);
    AURA_ASSERT_EQ(aura_flow_mw_add(bus, "deadline", aura_flow_mw_deadline, NULL), AURA_OK);
    AURA_ASSERT_EQ(aura_flow_mw_add(bus, "redact", aura_flow_mw_redact, NULL), AURA_OK);
    aura_flow_stage_t *s = aura_flow_stage_create(bus, "capture", handler_capture, NULL, 8,
                                                   AURA_FLOW_DROP_OLDEST);
    AURA_ASSERT(s != NULL);
    AURA_ASSERT_EQ(aura_flow_stage_subscribe(s, "t"), AURA_OK);
    AURA_ASSERT_EQ(aura_flow_stage_start(s), AURA_OK);

    /* 过期消息 */
    aura_flow_msg_t m;
    memset(&m, 0, sizeof(m));
    m.kind = AURA_FLOW_TEXT;
    snprintf(m.topic, sizeof(m.topic), "t");
    snprintf(m.payload, sizeof(m.payload), "expired");
    m.len = (uint32_t)strlen(m.payload);
    m.ctx.session_id = 1;
    m.ctx.turn_id = 1;
    m.ctx.deadline_us = aura_osal_time_us() - 1;
    AURA_ASSERT_EQ(aura_flow_publish(bus, &m), AURA_OK);

    /* 脱敏 */
    AURA_ASSERT_EQ(publish_text_raw(bus, "t", "password=hunter2 token=abc 13812345678", 1, 1),
                   AURA_OK);

    /* 非法长度 */
    memset(&m, 0, sizeof(m));
    m.kind = AURA_FLOW_TEXT;
    snprintf(m.topic, sizeof(m.topic), "t");
    m.len = AURA_FLOW_PAYLOAD_MAX;
    AURA_ASSERT_EQ(aura_flow_publish(bus, &m), AURA_OK);

    aura_test_sleep_ms(250);
    AURA_ASSERT_EQ(atomic_load(&g_capture_count), 1);
    AURA_ASSERT(strstr(g_capture, "password=*******") != NULL);
    AURA_ASSERT(strstr(g_capture, "token=***") != NULL);
    AURA_ASSERT(strstr(g_capture, "138****5678") != NULL);
    AURA_ASSERT(strstr(g_capture, "hunter2") == NULL);

    aura_flow_stage_destroy(s);
    aura_flow_bus_destroy(bus);
}

AURA_TEST(test_flow_mw_persist)
{
    aura_flow_bus_t *bus = aura_flow_bus_create(8, AURA_FLOW_DROP_OLDEST);
    aura_flow_store_t *store = aura_flow_store_create();
    AURA_ASSERT(bus != NULL && store != NULL);
    AURA_ASSERT_EQ(aura_flow_mw_add(bus, "persist", aura_flow_mw_persist, store), AURA_OK);
    aura_flow_stage_t *s = aura_flow_stage_create(bus, "noop", handler_b, NULL, 8,
                                                   AURA_FLOW_DROP_OLDEST);
    AURA_ASSERT(s != NULL);
    AURA_ASSERT_EQ(aura_flow_stage_subscribe(s, "asr.final"), AURA_OK);
    AURA_ASSERT_EQ(aura_flow_stage_start(s), AURA_OK);

    AURA_ASSERT_EQ(publish_text_raw(bus, "asr.final", "hello", 5, 1), AURA_OK);
    aura_test_sleep_ms(200);

    char buf[64];
    AURA_ASSERT_EQ(aura_flow_store_get(store, 5, "asr.final", buf, sizeof(buf)), AURA_OK);
    AURA_ASSERT_STREQ(buf, "hello");

    aura_flow_stage_destroy(s);
    aura_flow_store_destroy(store);
    aura_flow_bus_destroy(bus);
}

AURA_TEST(test_flow_mw_retry)
{
    atomic_store(&g_retry_calls, 0);
    aura_flow_bus_t *bus = aura_flow_bus_create(8, AURA_FLOW_DROP_OLDEST);
    AURA_ASSERT(bus != NULL);
    aura_flow_retry_cfg_t cfg = {2};
    AURA_ASSERT_EQ(aura_flow_mw_add(bus, "retry", aura_flow_mw_retry, &cfg), AURA_OK);
    aura_flow_stage_t *s = aura_flow_stage_create(bus, "flaky", handler_flaky, NULL, 8,
                                                   AURA_FLOW_DROP_OLDEST);
    AURA_ASSERT(s != NULL);
    AURA_ASSERT_EQ(aura_flow_stage_subscribe(s, "r"), AURA_OK);
    AURA_ASSERT_EQ(aura_flow_stage_start(s), AURA_OK);

    AURA_ASSERT_EQ(publish_text_raw(bus, "r", "x", 1, 1), AURA_OK);
    aura_test_sleep_ms(300);
    AURA_ASSERT_EQ(atomic_load(&g_retry_calls), 2);

    aura_flow_stage_destroy(s);
    aura_flow_bus_destroy(bus);
}

AURA_TEST_MAIN()
