/* pipeline 单元测试：节点链路、pts 维护、错误上抛、帧池 */
#include "aura_test.h"

#include "core/node.h"
#include "core/pipeline/pipeline.h"

/* ---------------------------------------------- 测试节点：直通 + 终端计数 */

typedef struct {
    aura_node_t   base;
    uint64_t      frames_in;
    uint64_t      last_pts;
    uint64_t      first_pts;
    bool          have_first;      /* pts=0 是合法值，不能用 0 当"未设置"哨兵 */
    int           error_at_frame; /* -1 = 不注入错误 */
    aura_err_t    inject_err;
} sink_node_t;

static aura_err_t sink_process(aura_node_t *self, const aura_audio_frame_t *frame)
{
    sink_node_t *s = (sink_node_t *)self;
    s->frames_in++;
    if (!s->have_first) {
        s->have_first = true;
        s->first_pts  = frame->pts_us;
    }
    s->last_pts = frame->pts_us;
    if (s->error_at_frame >= 0 && s->frames_in == (uint64_t)(s->error_at_frame + 1)) {
        return s->inject_err;
    }
    return AURA_OK;
}

static const aura_node_ops_t sink_ops = {
    .process_audio = sink_process,
};

static aura_node_t *make_sink(sink_node_t *s, const char *name)
{
    memset(s, 0, sizeof(*s));
    s->error_at_frame = -1;
    aura_node_init(&s->base, name, &sink_ops,
                   &(aura_node_caps_t){.consumes_audio = true});
    return &s->base;
}

/* 直通节点：原样产出 */
typedef struct {
    aura_node_t base;
    uint64_t    passthrough;
} pass_node_t;

static aura_err_t pass_process(aura_node_t *self, const aura_audio_frame_t *frame)
{
    pass_node_t *p = (pass_node_t *)self;
    p->passthrough++;
    return aura_node_emit_audio(self, frame);
}

static const aura_node_ops_t pass_ops = {
    .process_audio = pass_process,
};

static aura_node_t *make_pass(pass_node_t *p, const char *name)
{
    memset(p, 0, sizeof(*p));
    aura_node_init(&p->base, name, &pass_ops,
                   &(aura_node_caps_t){.consumes_audio = true, .produces_audio = true});
    return &p->base;
}

/* ------------------------------------------------------------ 测试用例 */

static int16_t g_pcm[1600];

static void test_linear_chain_pts(void)
{
    pass_node_t pass;
    sink_node_t sink;
    aura_pipeline_cfg_t cfg;
    aura_pipeline_config_default(&cfg);
    aura_pipeline_t *p = aura_pipeline_create(&cfg);
    AURA_ASSERT(p != NULL);

    AURA_ASSERT(aura_pipeline_add(p, make_pass(&pass, "pass")) == AURA_OK);
    AURA_ASSERT(aura_pipeline_add(p, make_sink(&sink, "sink")) == AURA_OK);
    AURA_ASSERT(aura_pipeline_start(p) == AURA_OK);

    /* 喂 20 帧 × 10ms，pts 从 1s 起，自动推进 */
    memset(g_pcm, 0, sizeof(g_pcm));
    uint64_t pts = 1000000; /* 1s */
    for (int i = 0; i < 20; i++) {
        AURA_ASSERT(aura_pipeline_feed(p, g_pcm, 160, 1, AURA_SAMPLE_S16, pts, AURA_WAIT_FOREVER) ==
                    AURA_OK);
        pts += 10000;
    }
    AURA_ASSERT(aura_pipeline_wait_drained(p, 2000) == AURA_OK);

    AURA_ASSERT_EQ(pass.passthrough, 20);
    AURA_ASSERT_EQ(sink.frames_in, 20);
    AURA_ASSERT_EQ(sink.first_pts, 1000000);
    AURA_ASSERT_EQ(sink.last_pts, 1000000 + 19 * 10000);

    aura_pipeline_stats_t stats;
    aura_pipeline_stats(p, &stats);
    AURA_ASSERT_EQ(stats.frames_in, 20);
    AURA_ASSERT_EQ(stats.frames_out, 20);
    AURA_ASSERT_EQ(stats.frames_dropped, 0);

    aura_pipeline_stop(p);
    aura_pipeline_destroy(p);
}

static void test_pts_auto_when_zero(void)
{
    pass_node_t pass;
    sink_node_t sink;
    aura_pipeline_cfg_t cfg;
    aura_pipeline_config_default(&cfg);
    aura_pipeline_t *p = aura_pipeline_create(&cfg);
    AURA_ASSERT(p != NULL);

    AURA_ASSERT(aura_pipeline_add(p, make_pass(&pass, "pass")) == AURA_OK);
    AURA_ASSERT(aura_pipeline_add(p, make_sink(&sink, "sink")) == AURA_OK);
    AURA_ASSERT(aura_pipeline_start(p) == AURA_OK);

    /* pts=0：pipeline 从当前时刻起自动推进 */
    AURA_ASSERT(aura_pipeline_feed(p, g_pcm, 160, 1, AURA_SAMPLE_S16, 0, AURA_NO_WAIT) == AURA_OK);
    AURA_ASSERT(aura_pipeline_feed(p, g_pcm, 160, 1, AURA_SAMPLE_S16, 0, AURA_NO_WAIT) == AURA_OK);
    AURA_ASSERT(aura_pipeline_wait_drained(p, 2000) == AURA_OK);
    AURA_ASSERT_EQ(sink.frames_in, 2);
    AURA_ASSERT_EQ(sink.last_pts, sink.first_pts + 10000);

    aura_pipeline_stop(p);
    aura_pipeline_destroy(p);
}

static aura_event_t s_err_ev;
static volatile int s_err_flag = 0;

static void on_err(const aura_event_t *e, void *u)
{
    (void)u;
    s_err_ev = *e;
    s_err_flag = 1;
}

static void test_node_error_bubbles_to_event_bus(void)
{
    pass_node_t pass;
    sink_node_t sink;
    aura_pipeline_cfg_t cfg;
    aura_pipeline_config_default(&cfg);
    aura_pipeline_t *p = aura_pipeline_create(&cfg);
    AURA_ASSERT(p != NULL);

    AURA_ASSERT(aura_pipeline_add(p, make_pass(&pass, "pass")) == AURA_OK);
    /* 注意：make_sink 会重置 error_at_frame，注入配置必须在 make_sink 之后设置 */
    AURA_ASSERT(aura_pipeline_add(p, make_sink(&sink, "sink")) == AURA_OK);
    sink.error_at_frame = 4;
    sink.inject_err     = AURA_ERR_MODEL;

    /* 事件订阅必须在喂帧之前完成，否则错误事件可能先于订阅被分发。
     * 注意用回调+轮询而不是 wait()：wait() 拿不到注册前的已分发事件。 */
    s_err_flag = 0;
    aura_event_bus_t *bus = aura_pipeline_bus(p);
    AURA_ASSERT(aura_event_bus_subscribe(bus, 1u << AURA_EVENT_ERROR, on_err, NULL) == AURA_OK);

    AURA_ASSERT(aura_pipeline_start(p) == AURA_OK);

    uint64_t pts = 1000;
    for (int i = 0; i < 10; i++) {
        AURA_ASSERT(aura_pipeline_feed(p, g_pcm, 160, 1, AURA_SAMPLE_S16, pts, AURA_WAIT_FOREVER) ==
                    AURA_OK);
        pts += 10000;
    }

    /* 等 ERROR 事件出现（第 5 帧注入错误） */
    for (int i = 0; i < 400 && !s_err_flag; i++) {
        aura_test_sleep_ms(5);
    }
    AURA_ASSERT(s_err_flag == 1);
    AURA_ASSERT_EQ(s_err_ev.code, AURA_ERR_MODEL);

    aura_pipeline_wait_drained(p, 2000);
    aura_pipeline_stats_t stats;
    aura_pipeline_stats(p, &stats);
    AURA_CHECK(stats.node_errors >= 1);
    AURA_ASSERT_EQ(sink.frames_in, 10); /* 节点不因错误退出，继续处理后续帧 */

    aura_pipeline_stop(p);
    aura_pipeline_destroy(p);
}

static void test_audio_payload_integrity(void)
{
    pass_node_t pass;
    sink_node_t sink;
    aura_pipeline_cfg_t cfg;
    aura_pipeline_config_default(&cfg);
    aura_pipeline_t *p = aura_pipeline_create(&cfg);
    AURA_ASSERT(p != NULL);

    AURA_ASSERT(aura_pipeline_add(p, make_pass(&pass, "pass")) == AURA_OK);
    AURA_ASSERT(aura_pipeline_add(p, make_sink(&sink, "sink")) == AURA_OK);
    AURA_ASSERT(aura_pipeline_start(p) == AURA_OK);

    for (int i = 0; i < 1600; i++) {
        g_pcm[i] = (int16_t)(i - 800);
    }
    AURA_ASSERT(aura_pipeline_feed(p, g_pcm, 160, 1, AURA_SAMPLE_S16, 5000, AURA_NO_WAIT) ==
                AURA_OK);
    AURA_ASSERT(aura_pipeline_wait_drained(p, 2000) == AURA_OK);
    AURA_ASSERT_EQ(sink.frames_in, 1);
    AURA_ASSERT_EQ(sink.first_pts, 5000);

    aura_pipeline_stop(p);
    aura_pipeline_destroy(p);
}

static void test_selftest_and_flush(void)
{
    pass_node_t pass;
    sink_node_t sink;
    aura_pipeline_cfg_t cfg;
    aura_pipeline_config_default(&cfg);
    aura_pipeline_t *p = aura_pipeline_create(&cfg);
    AURA_ASSERT(p != NULL);

    AURA_ASSERT(aura_pipeline_add(p, make_pass(&pass, "pass")) == AURA_OK);
    AURA_ASSERT(aura_pipeline_add(p, make_sink(&sink, "sink")) == AURA_OK);
    AURA_ASSERT(aura_pipeline_selftest(p) == AURA_OK); /* 无 selftest 的节点视为通过 */
    AURA_ASSERT(aura_pipeline_start(p) == AURA_OK);
    AURA_ASSERT(aura_pipeline_flush(p) == AURA_OK); /* 空管线 flush 无害 */
    aura_pipeline_stop(p);
    aura_pipeline_destroy(p);
}

AURA_TEST(test_linear_chain_pts);
AURA_TEST(test_pts_auto_when_zero);
AURA_TEST(test_node_error_bubbles_to_event_bus);
AURA_TEST(test_audio_payload_integrity);
AURA_TEST(test_selftest_and_flush);

AURA_TEST_MAIN();
