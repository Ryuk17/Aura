/* NN 适配模板单测：用**无模型算法**驱动适配层。
 *
 * desc 不声明 model_file → 适配层不加载模型（algo_init 收到的 model 为 NULL），
 * 于是滑窗/透传/旁路/边沿事件/控制面/统计这些**与引擎无关**的部分全部可测，
 * 不需要 MNN、不需要模型文件。真模型（silero VAD）的接入由 host_sim 覆盖 ——
 * 两边合起来才等于"NN 算法接进来了"。
 */
#include "aura_test.h"

#include "algorithm/aura_nn_adapter.h"
#include "core/algorithm.h"
#include "core/pipeline/pipeline.h"

#define NEAR_EPS 1e-7f

static bool near(float a, float b)
{
    float d = a - b;
    return (d < NEAR_EPS) && (d > -NEAR_EPS);
}

/* ============================================================ 假算法 */

typedef struct {
    aura_nn_window_t win;
    int              init_calls;
    int              deinit_calls;
    int              reset_calls;
    int              frame_in_calls;
    int              infer_out_calls;
    int              events_seen;
    bool             saw_model_null;
    int              hit; /* infer_out 里 report 出去的值 */
} fake_nn_state_t;

/* 窗口口径由 extra 参数给（与真实算法一致：silero 是 64+512）——
 * 顺带验证 params 快照在 algo_init 里确实可用。 */
static aura_err_t fake_algo_init(aura_nn_node_t *self, aura_infer_model_t *model)
{
    fake_nn_state_t *s = aura_nn_state(self);
    s->init_calls++;
    s->saw_model_null = (model == NULL);

    uint32_t ctx = (uint32_t)aura_algo_params_i32(&self->params, "ctx_samples", 64);
    uint32_t nw  = (uint32_t)aura_algo_params_i32(&self->params, "new_samples", 512);
    return aura_nn_window_init(&s->win, ctx, nw);
}

static bool fake_frame_in(aura_nn_node_t *self, const aura_audio_frame_t *frame)
{
    fake_nn_state_t *s = aura_nn_state(self);
    s->frame_in_calls++;
    return aura_nn_window_push(&s->win, (const int16_t *)frame->data, frame->frame_count) != NULL;
}

static aura_err_t fake_algo_reset(aura_nn_node_t *self)
{
    fake_nn_state_t *s = aura_nn_state(self);
    s->reset_calls++;
    aura_nn_window_reset(&s->win);
    return AURA_OK;
}

static void fake_algo_deinit(aura_nn_node_t *self)
{
    ((fake_nn_state_t *)aura_nn_state(self))->deinit_calls++;
}

/* 不判定：只消费窗口（透传类算法的最小形态） */
static aura_err_t fake_infer_out_plain(aura_nn_node_t *self)
{
    fake_nn_state_t *s = aura_nn_state(self);
    s->infer_out_calls++;
    aura_nn_window_consume(&s->win);
    return AURA_OK;
}

/* 判定：report 一行就是全部（边沿检测/发事件在适配层） */
static aura_err_t fake_infer_out_report(aura_nn_node_t *self)
{
    fake_nn_state_t *s = aura_nn_state(self);
    s->infer_out_calls++;
    aura_nn_report(self, s->hit != 0);
    aura_nn_window_consume(&s->win);
    return AURA_OK;
}

/* 事件回调：框架只把事件转交过来，语义由算法定 —— 这里把 BARGE_IN 当成
 * "判定为有人说话"，用于验证事件确实送到了算法手里。 */
static void fake_algo_on_event(aura_nn_node_t *self, const aura_event_t *ev)
{
    if (ev->type != AURA_EVENT_BARGE_IN) {
        return;
    }
    ((fake_nn_state_t *)aura_nn_state(self))->events_seen++;
    aura_nn_report(self, true); /* 0→1 → 适配层发 rise 事件 */
}

static const aura_algo_param_spec_t WIN_SPECS[] = {
    { "ctx_samples", AURA_ALGO_PARAM_I32, 0.f, 2048.f, "上文样本数" },
    { "new_samples", AURA_ALGO_PARAM_I32, 1.f, 4096.f, "每次前向的新样本数" },
};

static aura_nn_desc_t g_nn_descs[] = {
    {
        .name       = "fake_nn_pass",
        .kind       = AURA_ALGO_KIND_NN_SYNC,
        .io_kind    = AURA_NN_IO_PASSTHROUGH,
        .state_size = sizeof(fake_nn_state_t),
        .algo_init  = fake_algo_init,
        .frame_in   = fake_frame_in,
        .infer_out  = fake_infer_out_plain,
        .algo_reset = fake_algo_reset,
        .algo_deinit = fake_algo_deinit,
        .param_specs = WIN_SPECS,
        .param_spec_count = sizeof(WIN_SPECS) / sizeof(WIN_SPECS[0]),
    },
    {
        .name       = "fake_nn_obs",
        .kind       = AURA_ALGO_KIND_NN_SYNC,
        .io_kind    = AURA_NN_IO_OBSERVER,
        .state_size = sizeof(fake_nn_state_t),
        .algo_init  = fake_algo_init,
        .frame_in   = fake_frame_in,
        .infer_out  = fake_infer_out_plain,
        .algo_reset = fake_algo_reset,
        .algo_deinit = fake_algo_deinit,
        .param_specs = WIN_SPECS,
        .param_spec_count = sizeof(WIN_SPECS) / sizeof(WIN_SPECS[0]),
    },
    {
        /* VAD 形态：判定 + 边沿事件 */
        .name       = "fake_nn_flag",
        .kind       = AURA_ALGO_KIND_NN_SYNC,
        .io_kind    = AURA_NN_IO_PASSTHROUGH,
        .state_size = sizeof(fake_nn_state_t),
        .algo_init  = fake_algo_init,
        .frame_in   = fake_frame_in,
        .infer_out  = fake_infer_out_report,
        .algo_reset = fake_algo_reset,
        .algo_deinit = fake_algo_deinit,
        .flag_rise_type = AURA_EVENT_VAD_SPEECH_START,
        .flag_fall_type = AURA_EVENT_VAD_SPEECH_END,
        .param_specs = WIN_SPECS,
        .param_spec_count = sizeof(WIN_SPECS) / sizeof(WIN_SPECS[0]),
    },
    {
        /* 事件驱动形态：收到 BARGE_IN 就判定为"有人说话"（播放期间抬高阈值 /
         * 立刻打断这类策略的最小演示）。 */
        .name         = "fake_nn_event",
        .kind         = AURA_ALGO_KIND_NN_SYNC,
        .io_kind      = AURA_NN_IO_PASSTHROUGH,
        .wants_events = true,
        .state_size   = sizeof(fake_nn_state_t),
        .algo_init    = fake_algo_init,
        .frame_in     = fake_frame_in,
        .infer_out    = fake_infer_out_plain,
        .algo_reset   = fake_algo_reset,
        .algo_deinit  = fake_algo_deinit,
        .on_event     = fake_algo_on_event,
        .flag_rise_type = AURA_EVENT_VAD_SPEECH_START,
        .flag_fall_type = AURA_EVENT_VAD_SPEECH_END,
        .param_specs  = WIN_SPECS,
        .param_spec_count = sizeof(WIN_SPECS) / sizeof(WIN_SPECS[0]),
    },
    {
        /* 无状态、无回调：纯透传节点（框架也得接受） */
        .name    = "fake_nn_bare",
        .kind    = AURA_ALGO_KIND_NN_SYNC,
        .io_kind = AURA_NN_IO_PASSTHROUGH,
    },
};

#define NN_DESC_COUNT (sizeof(g_nn_descs) / sizeof(g_nn_descs[0]))

/* ============================== 观测节点（pts 追踪） ============================== */

#define TAP_MAX 64
typedef struct {
    uint64_t pts[TAP_MAX];
    uint32_t rate[TAP_MAX];
    uint32_t frames[TAP_MAX];
    uint32_t count;
} tap_trace_t;

static tap_trace_t g_trace;

static aura_err_t tap_process_audio(aura_node_t *self, const aura_audio_frame_t *f)
{
    (void)self;
    if (g_trace.count < TAP_MAX) {
        uint32_t i        = g_trace.count++;
        g_trace.pts[i]    = f->pts_us;
        g_trace.rate[i]   = f->sample_rate;
        g_trace.frames[i] = f->frame_count;
    }
    return AURA_OK;
}

static const aura_node_ops_t TAP_OPS = {
    .process_audio = tap_process_audio,
};

static aura_node_t *tap_create(const aura_algo_params_t *p, aura_err_t *err)
{
    (void)p;
    aura_node_t *n = (aura_node_t *)calloc(1, sizeof(*n));
    if (n == NULL) {
        if (err != NULL) {
            *err = AURA_ERR_NOMEM;
        }
        return NULL;
    }
    aura_node_caps_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.consumes_audio = true;
    aura_node_init(n, "tap", &TAP_OPS, &caps);
    return n;
}

static const aura_algo_desc_t TAP_DESC = {
    .name    = "tap",
    .version = 1,
    .kind    = AURA_ALGO_KIND_NN_SYNC,
    .io      = { .consumes_audio = true, .produces_audio = false },
    .create  = tap_create,
};

/* ============================== 脚手架 ============================== */

static void setup(void)
{
    aura_algo_reset();
    memset(&g_trace, 0, sizeof(g_trace));
    AURA_ASSERT_EQ(aura_nn_register_all(g_nn_descs, (uint32_t)NN_DESC_COUNT), AURA_OK);
    AURA_ASSERT_EQ(aura_algo_register(&TAP_DESC), AURA_OK);
}

static aura_pipeline_t *build_and_start(const char *syntax, aura_chain_t *c, aura_node_t **nodes,
                                        uint32_t *n)
{
    aura_pipeline_t *p = aura_pipeline_create(NULL);
    AURA_ASSERT(p != NULL);
    AURA_ASSERT_EQ(aura_chain_parse(syntax, c), AURA_OK);
    AURA_ASSERT_EQ(aura_chain_build(p, c, nodes, n), AURA_OK);
    AURA_ASSERT_EQ(aura_pipeline_start(p), AURA_OK);
    return p;
}

/* 按固定 pts 投喂 count 帧（每帧 160 样本 @16k）。 */
static void feed_pts(aura_pipeline_t *p, int count, uint64_t pts_us)
{
    static int16_t pcm[160];
    for (int i = 0; i < 160; i++) {
        pcm[i] = (int16_t)((i % 100) + 1);
    }
    for (int k = 0; k < count; k++) {
        AURA_ASSERT_EQ(aura_pipeline_feed(p, pcm, 160, 1, AURA_SAMPLE_S16, pts_us, 200), AURA_OK);
    }
}

static void teardown(aura_pipeline_t *p, aura_node_t **nodes, uint32_t n)
{
    aura_pipeline_stop(p);
    aura_pipeline_destroy(p);
    aura_chain_destroy_nodes(nodes, n);
}

static fake_nn_state_t *state_of(aura_node_t *n)
{
    return (fake_nn_state_t *)aura_nn_state((const aura_nn_node_t *)n);
}

/* ============================== 用例 ============================== */

/* 滑窗：任意帧长都不丢样本。这是整个 NN 模板里最容易写错的一处 ——
 * "攒够就返回"的写法会把跨窗那一帧的尾巴丢掉（160 样本的帧 vs 512 的窗，
 * 每 3.2 帧丢 128 个样本），表现为模型判定缓慢漂移且不报任何错。 */
AURA_TEST(test_window_no_loss)
{
    aura_nn_window_t w;
    AURA_ASSERT_EQ(aura_nn_window_init(&w, 2, 0), AURA_ERR_INVALID_ARG);
    /* 缓冲要留得下一整帧的余量 */
    AURA_ASSERT_EQ(aura_nn_window_init(&w, AURA_NN_WINDOW_BUF_MAX, 8), AURA_ERR_INVALID_ARG);
    AURA_ASSERT_EQ(aura_nn_window_init(&w, 2, 3), AURA_OK);

    /* 首窗：上文补零（与 silero 参考实现一致） */
    int16_t a[3] = { 1000, 2000, 3000 };
    const float *win = aura_nn_window_push(&w, a, 3);
    AURA_ASSERT(win != NULL);
    AURA_ASSERT(near(win[0], 0.f) && near(win[1], 0.f));
    AURA_ASSERT(near(win[2], 1000.f / 32768.f));
    AURA_ASSERT(near(win[4], 3000.f / 32768.f));
    aura_nn_window_consume(&w);
    AURA_ASSERT_EQ(w.len, 2); /* 只剩上文 */

    /* 跨窗的一帧：4 个样本里 3 个进本窗、1 个（7000）留给下一窗 —— 一个都不能丢 */
    int16_t b[4] = { 4000, 5000, 6000, 7000 };
    win          = aura_nn_window_push(&w, b, 4);
    AURA_ASSERT(win != NULL);
    AURA_ASSERT(near(win[0], 2000.f / 32768.f)); /* 上文 = 上一窗末尾两个 */
    AURA_ASSERT(near(win[1], 3000.f / 32768.f));
    AURA_ASSERT(near(win[4], 6000.f / 32768.f));
    aura_nn_window_consume(&w);
    /* 消费后：上文两个 + 没进窗的 7000，共 3 个 */
    AURA_ASSERT_EQ(w.len, 3);

    /* 7000 之后的样本还接得上 —— 丢掉跨窗尾巴的实现正是从这里开始错位 */
    int16_t c[1] = { 8000 };
    AURA_ASSERT(aura_nn_window_push(&w, c, 1) == NULL); /* 4 个，差 1 个 */
    int16_t d[1] = { 9000 };
    win          = aura_nn_window_push(&w, d, 1);
    AURA_ASSERT(win != NULL);
    AURA_ASSERT(near(win[0], 5000.f / 32768.f));
    AURA_ASSERT(near(win[1], 6000.f / 32768.f));
    AURA_ASSERT(near(win[2], 7000.f / 32768.f)); /* ← 跨窗那个样本必须在这里 */
    AURA_ASSERT(near(win[4], 9000.f / 32768.f));
    aura_nn_window_consume(&w);

    aura_nn_window_reset(&w);
    AURA_ASSERT_EQ(w.len, 2); /* 回到"首窗"状态：上文重新补零 */
    AURA_ASSERT(near(w.buf[0], 0.f));
    AURA_ASSERT_EQ(w.new_samples, 3); /* 配置不丢 */
}

AURA_TEST(test_passthrough_and_stats)
{
    setup();
    aura_chain_t     c;
    aura_node_t     *nodes[AURA_CHAIN_MAX_LINKS];
    uint32_t         n = 0;
    aura_pipeline_t *p = build_and_start("fake_nn_pass, tap", &c, nodes, &n);
    AURA_ASSERT_EQ(n, 2);

    /* 40 帧 × 160 = 6400 样本；窗 64+512 → 首窗在第 576 个样本处，其后每 512 一个 */
    feed_pts(p, 40, 0);
    AURA_ASSERT_EQ(aura_pipeline_wait_drained(p, 2000), AURA_OK);

    /* 透传：下游拿到全部 40 帧，一帧不少、帧长不变 */
    AURA_ASSERT_EQ(g_trace.count, 40);
    AURA_ASSERT_EQ(g_trace.frames[0], 160);
    AURA_ASSERT_EQ(g_trace.rate[0], 16000);

    aura_nn_stats_t st;
    AURA_ASSERT_EQ(aura_nn_get_stats(nodes[0], &st), AURA_OK);
    AURA_ASSERT_EQ(st.frames_in, 40);
    AURA_ASSERT_EQ(st.windows, 12);
    AURA_ASSERT_EQ(st.dropped, 0);
    AURA_ASSERT_EQ(st.errors, 0);

    fake_nn_state_t *s = state_of(nodes[0]);
    AURA_ASSERT_EQ(s->init_calls, 1);
    AURA_ASSERT(s->saw_model_null); /* 无模型算法：algo_init 收到 NULL 而不是野指针 */
    AURA_ASSERT_EQ(s->frame_in_calls, 40);
    AURA_ASSERT_EQ(s->infer_out_calls, 12);

    /* 能力声明确实来自 desc：透传节点产出音频 */
    AURA_ASSERT(nodes[0]->caps.consumes_audio);
    AURA_ASSERT(nodes[0]->caps.produces_audio);

    /* 认亲：拿别的族的节点来问 NN 统计要拒绝，而不是按错误的结构体读内存 */
    aura_nn_stats_t bad;
    AURA_ASSERT_EQ(aura_nn_get_stats(nodes[1], &bad), AURA_ERR_INVALID_ARG);

    teardown(p, nodes, n);
}

AURA_TEST(test_observer_no_starve)
{
    setup();
    aura_chain_t     c;
    aura_node_t     *nodes[AURA_CHAIN_MAX_LINKS];
    uint32_t         n = 0;
    /* 观察者挂在旁路：主线继续走到第二个 pass */
    aura_pipeline_t *p = build_and_start("fake_nn_pass, tee(fake_nn_obs), fake_nn_pass", &c, nodes,
                                         &n);
    AURA_ASSERT_EQ(n, 3);
    AURA_ASSERT_EQ(nodes[0]->next_audio_count, 2);

    feed_pts(p, 30, 0);
    AURA_ASSERT_EQ(aura_pipeline_wait_drained(p, 2000), AURA_OK);

    aura_nn_stats_t obs, last;
    AURA_ASSERT_EQ(aura_nn_get_stats(nodes[1], &obs), AURA_OK);
    AURA_ASSERT_EQ(aura_nn_get_stats(nodes[2], &last), AURA_OK);
    AURA_ASSERT_EQ(obs.frames_in, 30);  /* 旁路看到的是同一批帧 */
    AURA_ASSERT_EQ(last.frames_in, 30); /* 观察者不截断主线 */

    /* 观察者不产出音频：caps 说的是真话，故没有下游 */
    AURA_ASSERT(!nodes[1]->caps.produces_audio);
    AURA_ASSERT_EQ(nodes[1]->next_audio_count, 0);

    teardown(p, nodes, n);
}

AURA_TEST(test_flag_edges_and_flush)
{
    setup();
    aura_chain_t     c;
    aura_node_t     *nodes[AURA_CHAIN_MAX_LINKS];
    uint32_t         n = 0;

    /* 每帧一窗（ctx=0, new=160）：这样"帧数 = 判定次数"，边沿数才好数。
     * 参数在 build 前填 —— 工厂只认链描述里的 extra，改晚了不生效。 */
    aura_pipeline_t *q = aura_pipeline_create(NULL);
    AURA_ASSERT(q != NULL);
    AURA_ASSERT_EQ(aura_chain_parse("fake_nn_flag, tap", &c), AURA_OK);
    AURA_ASSERT_EQ(aura_algo_params_set_i32(aura_chain_params(&c, 0), "ctx_samples", 0), AURA_OK);
    AURA_ASSERT_EQ(aura_algo_params_set_i32(aura_chain_params(&c, 0), "new_samples", 160), AURA_OK);
    AURA_ASSERT_EQ(aura_chain_build(q, &c, nodes, &n), AURA_OK);
    AURA_ASSERT_EQ(aura_pipeline_start(q), AURA_OK);
    AURA_ASSERT_EQ(n, 2);

    aura_event_bus_t *bus = aura_pipeline_bus(q);
    AURA_ASSERT(bus != NULL);
    const uint32_t mask =
        (1u << AURA_EVENT_VAD_SPEECH_START) | (1u << AURA_EVENT_VAD_SPEECH_END);

    fake_nn_state_t *s = state_of(nodes[0]);
    aura_event_t     ev;

    /* 连续 3 帧命中：只发 1 次上升沿（每帧发事件会刷爆总线） */
    s->hit = 1;
    feed_pts(q, 3, 0);
    AURA_ASSERT_EQ(aura_event_bus_wait(bus, mask, &ev, 2000), AURA_OK);
    AURA_ASSERT_EQ(ev.type, AURA_EVENT_VAD_SPEECH_START);

    aura_nn_stats_t st;
    AURA_ASSERT_EQ(aura_nn_get_stats(nodes[0], &st), AURA_OK);
    AURA_ASSERT_EQ(st.windows, 3);
    AURA_ASSERT_EQ(st.flag_rise, 1);
    AURA_ASSERT_EQ(st.flag_fall, 0);
    AURA_ASSERT(aura_nn_flag((const aura_nn_node_t *)nodes[0]));

    /* 转静音 → 下降沿 */
    s->hit = 0;
    feed_pts(q, 2, 0);
    AURA_ASSERT_EQ(aura_event_bus_wait(bus, mask, &ev, 2000), AURA_OK);
    AURA_ASSERT_EQ(ev.type, AURA_EVENT_VAD_SPEECH_END);
    AURA_ASSERT_EQ(aura_nn_get_stats(nodes[0], &st), AURA_OK);
    AURA_ASSERT_EQ(st.flag_rise, 1);
    AURA_ASSERT_EQ(st.flag_fall, 1);
    AURA_ASSERT(!aura_nn_flag((const aura_nn_node_t *)nodes[0]));

    /* FLUSH 清判定状态：不清的话 last_flag 还停在 0，下一帧的 0→1 会被
     * 边沿检测当成"没变化"吞掉 —— 打断后必然发生，且表现为"唤醒失效" */
    AURA_ASSERT_EQ(aura_pipeline_control_node(q, nodes[0], AURA_CMD_FLUSH, NULL), AURA_OK);
    AURA_ASSERT_EQ(s->reset_calls, 1);
    AURA_ASSERT_EQ(s->win.len, 0);

    s->hit = 1;
    feed_pts(q, 1, 0);
    AURA_ASSERT_EQ(aura_event_bus_wait(bus, mask, &ev, 2000), AURA_OK);
    AURA_ASSERT_EQ(ev.type, AURA_EVENT_VAD_SPEECH_START);
    AURA_ASSERT_EQ(aura_nn_get_stats(nodes[0], &st), AURA_OK);
    AURA_ASSERT_EQ(st.flag_rise, 2);

    /* 透传照旧：3+2+1 帧全到下游 */
    AURA_ASSERT_EQ(aura_pipeline_wait_drained(q, 2000), AURA_OK);
    AURA_ASSERT_EQ(g_trace.count, 6);

    teardown(q, nodes, n);
}

/* 无回调的纯透传节点：适配层不该因为 frame_in/infer_out 为空就出错 */
AURA_TEST(test_bare_passthrough)
{
    setup();
    aura_chain_t     c;
    aura_node_t     *nodes[AURA_CHAIN_MAX_LINKS];
    uint32_t         n = 0;
    aura_pipeline_t *p = build_and_start("fake_nn_bare, tap", &c, nodes, &n);

    feed_pts(p, 5, 0);
    AURA_ASSERT_EQ(aura_pipeline_wait_drained(p, 2000), AURA_OK);
    AURA_ASSERT_EQ(g_trace.count, 5);

    aura_nn_stats_t st;
    AURA_ASSERT_EQ(aura_nn_get_stats(nodes[0], &st), AURA_OK);
    AURA_ASSERT_EQ(st.frames_in, 5);
    AURA_ASSERT_EQ(st.windows, 0);

    teardown(p, nodes, n);
}

/* 未实现的形态在这里被拒绝，而不是等运行期静默丢结果 */
AURA_TEST(test_finalize_rejects)
{
    static aura_nn_desc_t d;

    memset(&d, 0, sizeof(d));
    d.name    = "nn_ok";
    d.kind    = AURA_ALGO_KIND_NN_SYNC;
    d.io_kind = AURA_NN_IO_OBSERVER;
    AURA_ASSERT_EQ(aura_nn_finalize(&d), AURA_OK);
    AURA_ASSERT_STREQ(d.algo.name, "nn_ok");
    AURA_ASSERT_EQ(d.algo.kind, AURA_ALGO_KIND_NN_SYNC);
    AURA_ASSERT(d.algo.io.consumes_audio);
    AURA_ASSERT(!d.algo.io.produces_audio); /* OBSERVER 不产出 */
    AURA_ASSERT(d.algo.create != NULL);

    memset(&d, 0, sizeof(d));
    d.name    = "nn_async";
    d.kind    = AURA_ALGO_KIND_NN_ASYNC;
    d.io_kind = AURA_NN_IO_OBSERVER;
    AURA_ASSERT_EQ(aura_nn_finalize(&d), AURA_ERR_UNSUPPORTED);

    memset(&d, 0, sizeof(d));
    d.name    = "nn_tts";
    d.kind    = AURA_ALGO_KIND_NN_SYNC;
    d.io_kind = AURA_NN_IO_ASYNC_SOURCE;
    AURA_ASSERT_EQ(aura_nn_finalize(&d), AURA_ERR_UNSUPPORTED);

    memset(&d, 0, sizeof(d));
    d.name     = "nn_nostate";
    d.kind     = AURA_ALGO_KIND_NN_SYNC;
    d.io_kind  = AURA_NN_IO_PASSTHROUGH;
    d.frame_in = fake_frame_in; /* 有逐帧回调却没给 state_size */
    AURA_ASSERT_EQ(aura_nn_finalize(&d), AURA_ERR_INVALID_ARG);

    memset(&d, 0, sizeof(d));
    d.name    = "nn_dsp";
    d.kind    = AURA_ALGO_KIND_DSP; /* 走 dsp 适配层的算法别在这注册 */
    d.io_kind = AURA_NN_IO_OBSERVER;
    AURA_ASSERT_EQ(aura_nn_finalize(&d), AURA_ERR_INVALID_ARG);

    memset(&d, 0, sizeof(d));
    d.name         = "nn_event_nohandler"; /* 订阅了事件却没给 on_event */
    d.kind         = AURA_ALGO_KIND_NN_SYNC;
    d.io_kind      = AURA_NN_IO_PASSTHROUGH;
    d.wants_events = true;
    AURA_ASSERT_EQ(aura_nn_finalize(&d), AURA_ERR_INVALID_ARG);
}

/* 事件送到算法手里：bus 上的 BARGE_IN → desc.on_event（策略钩子）；
 * 算法在回调里 report 出的边沿再由适配层转成 bus 事件。 */
AURA_TEST(test_event_dispatch)
{
    setup();
    aura_chain_t     c;
    aura_node_t     *nodes[AURA_CHAIN_MAX_LINKS];
    uint32_t         n = 0;
    aura_pipeline_t *p = build_and_start("fake_nn_event, tap", &c, nodes, &n);

    fake_nn_state_t *st = (fake_nn_state_t *)aura_nn_state((aura_nn_node_t *)nodes[0]);
    AURA_ASSERT_EQ(st->events_seen, 0);

    aura_event_bus_t *bus = aura_pipeline_bus(p);
    AURA_ASSERT(bus != NULL);
    /* 别的节点发的无关事件不该触发回调 */
    AURA_ASSERT_EQ(aura_event_bus_post(bus, AURA_EVENT_LLM_DONE, 0, "x"), AURA_OK);
    AURA_ASSERT_EQ(aura_event_bus_post(bus, AURA_EVENT_BARGE_IN, 0, "test"), AURA_OK);

    aura_event_t ev;
    AURA_ASSERT_EQ(aura_event_bus_wait(bus, 1u << AURA_EVENT_VAD_SPEECH_START, &ev, 2000),
                   AURA_OK);
    AURA_ASSERT_EQ(st->events_seen, 1);

    teardown(p, nodes, n);
}

AURA_TEST_MAIN()
