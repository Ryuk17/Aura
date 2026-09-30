/* event_bus 单元测试：发布/订阅/顺序/同步等待 */
#include "aura_test.h"

#include "core/event_bus/event_bus.h"

static volatile int g_cb_count[3] = {0, 0, 0};
static volatile uint32_t g_last_seq[3] = {0, 0, 0};

static void cb0(const aura_event_t *ev, void *user)
{
    (void)user;
    g_cb_count[0]++;
    g_last_seq[0] = ev->seq;
}

static void cb1(const aura_event_t *ev, void *user)
{
    (void)user;
    g_cb_count[1]++;
    g_last_seq[1] = ev->seq;
}

static void test_publish_subscribe(void)
{
    aura_event_bus_t *bus = aura_event_bus_create(64);
    AURA_ASSERT(bus != NULL);

    /* 订阅不同类型：cb0 只收 KWS_HIT，cb1 只收 ERROR */
    AURA_ASSERT(aura_event_bus_subscribe(bus, 1u << AURA_EVENT_KWS_HIT, cb0, NULL) == AURA_OK);
    AURA_ASSERT(aura_event_bus_subscribe(bus, 1u << AURA_EVENT_ERROR, cb1, NULL) == AURA_OK);

    AURA_ASSERT(aura_event_bus_post(bus, AURA_EVENT_KWS_HIT, 1, "hello") == AURA_OK);
    AURA_ASSERT(aura_event_bus_post(bus, AURA_EVENT_ERROR, AURA_ERR_MODEL, "boom") == AURA_OK);
    AURA_ASSERT(aura_event_bus_post(bus, AURA_EVENT_TTS_DONE, 0, NULL) == AURA_OK);

    /* 等分发任务处理完 */
    for (int i = 0; i < 100 && (g_cb_count[0] == 0 || g_cb_count[1] == 0); i++) {
        aura_test_sleep_ms(5);
    }
    AURA_ASSERT_EQ(g_cb_count[0], 1);
    AURA_ASSERT_EQ(g_cb_count[1], 1);
    AURA_CHECK(g_last_seq[0] < g_last_seq[1]); /* 序号单调递增 */

    /* 订阅上限：塞满后应拒绝 */
    for (int i = 0; i < 20; i++) {
        aura_event_bus_subscribe(bus, 1u << AURA_EVENT_USER, cb0, NULL);
    }
    aura_event_bus_destroy(bus);
}

static void test_wait_for_event(void)
{
    aura_event_bus_t *bus = aura_event_bus_create(64);
    AURA_ASSERT(bus != NULL);

    aura_event_t ev;
    AURA_ASSERT(aura_event_bus_post(bus, AURA_EVENT_KWS_HIT, 99, "hi") == AURA_OK);
    AURA_ASSERT(aura_event_bus_wait(bus, 1u << AURA_EVENT_KWS_HIT, &ev, 500) == AURA_OK);
    AURA_ASSERT_EQ(ev.code, 99);
    AURA_ASSERT_STREQ(ev.payload, "hi");
    AURA_CHECK(ev.seq > 0);
    AURA_CHECK(ev.ts_us > 0);

    /* 无匹配事件的等待应超时 */
    AURA_ASSERT(aura_event_bus_wait(bus, 1u << AURA_EVENT_VOICEPRINT_RESULT, &ev, 50) ==
                AURA_ERR_TIMEOUT);
    aura_event_bus_destroy(bus);
}

static void test_type_name(void)
{
    AURA_ASSERT_STREQ(aura_event_type_name(AURA_EVENT_KWS_HIT), "kws_hit");
    AURA_ASSERT_STREQ(aura_event_type_name(AURA_EVENT_ERROR), "error");
}

AURA_TEST(test_publish_subscribe);
AURA_TEST(test_wait_for_event);
AURA_TEST(test_type_name);

AURA_TEST_MAIN();
