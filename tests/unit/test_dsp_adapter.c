/* DSP 适配模板单测：用**假引擎描述表**驱动适配层。
 *
 * 不链接 TrickRoom：适配层的主体（攒帧/参考对齐/emit/边沿事件/错误降级）与具体
 * 引擎无关，用假引擎才能精确构造边界输入（帧长不足、参考缺失、Process 失败）。
 * 真实 AEC/NS 的接入是"填一张描述表"的事，正确性由"表 + 本文件的验证"共同保证。
 */
#include "aura_test.h"

#include "core/algorithm.h"
#include "core/pipeline/pipeline.h"
#include "dsp/aura_dsp_adapter.h"

/* ============================================================ 假引擎 */

typedef struct {
    int inited;
    int process_calls;
    int deinit_calls;
    int reset_calls;
    int destroy_calls;
    int last_in_samples;  /* 引擎收到的 in_samples */
    int last_max_out;     /* 引擎收到的 max_out_samples */
    int last_ref_nonnull; /* AEC：参考指针是否非空 */
    int fail_status;      /* 非 0 = Process 返回该状态码 */
    int vad_flag;
    int gain;
    /* 假引擎的"采样口径"：多通道用例里它得知道一帧有多少路，
     * 才能按交错总数读写（真实引擎是在 Init 时被告知的）。 */
    int channels;
    int total_mode; /* 1 = 按交错总数写 out_samples（AEC 口径） */
} fake_engine_t;

/* 每个节点一个引擎实例。按创建顺序登记 —— pipeline_start 依注册顺序 init，
 * 所以索引 i 就是链上第 i 个节点的引擎。 */
#define FAKE_MAX_ENGINES 8
static fake_engine_t *g_engines[FAKE_MAX_ENGINES];
static uint32_t       g_engine_count;

static fake_engine_t *engine_at(uint32_t i)
{
    return (i < g_engine_count) ? g_engines[i] : NULL;
}

static void *fake_create(void)
{
    if (g_engine_count >= FAKE_MAX_ENGINES) {
        return NULL;
    }
    fake_engine_t *e = (fake_engine_t *)calloc(1, sizeof(*e));
    if (e == NULL) {
        return NULL;
    }
    e->gain                     = 1;
    e->channels                 = 1;
    g_engines[g_engine_count++] = e;
    return e;
}

static int fake_destroy(void *h)
{
    if (h != NULL) {
        ((fake_engine_t *)h)->destroy_calls++;
    }
    /* 不 free：用例末尾要读计数（真实实现当然要释放）。 */
    return 0;
}

static int fake_init(void *h, const void *cfg)
{
    (void)cfg;
    ((fake_engine_t *)h)->inited = 1;
    return 0;
}

static int fake_set_param(void *h, const void *cfg)
{
    (void)h;
    (void)cfg;
    return 0;
}

static int fake_deinit(void *h)
{
    ((fake_engine_t *)h)->deinit_calls++;
    return 0;
}

static int fake_reset(void *h)
{
    ((fake_engine_t *)h)->reset_calls++;
    return 0;
}

/* 1→1：按 gain 缩放。多通道时 in_samples 仍是**每通道**样本数（NS/AGC 口径），
 * 交错总数 = in_samples × channels —— 这正是"口径不一致"容易踩的地方：
 * max_out 必须容下交错总数，而写回的 out_samples 是每通道数。 */
static int fake_1to1_process(void *h, const int16_t *in, const int16_t *ref, int in_samples,
                             int16_t *out, int max_out, int *out_samples, int *flag)
{
    (void)ref;
    (void)flag;
    fake_engine_t *e = (fake_engine_t *)h;
    e->process_calls++;
    e->last_in_samples = in_samples;
    e->last_max_out    = max_out;
    if (e->fail_status != 0) {
        return e->fail_status;
    }
    const int total = in_samples * e->channels;
    if (out == NULL || max_out < total) {
        return 1; /* 引擎侧参数错误 */
    }
    for (int i = 0; i < total; i++) {
        out[i] = (int16_t)(in[i] * e->gain);
    }
    *out_samples = in_samples;
    return 0;
}

/* VAD：只出标志 */
static int fake_vad_process(void *h, const int16_t *in, const int16_t *ref, int in_samples,
                            int16_t *out, int max_out, int *out_samples, int *flag)
{
    (void)in;
    (void)ref;
    (void)out;
    (void)max_out;
    (void)out_samples;
    fake_engine_t *e = (fake_engine_t *)h;
    e->process_calls++;
    e->last_in_samples = in_samples;
    if (e->fail_status != 0) {
        return e->fail_status;
    }
    if (flag != NULL) {
        *flag = e->vad_flag;
    }
    return 0;
}

/* 重采样：2:1 抽取（16k → 8k） */
static int fake_resample_process(void *h, const int16_t *in, const int16_t *ref, int in_samples,
                                 int16_t *out, int max_out, int *out_samples, int *flag)
{
    (void)ref;
    (void)flag;
    fake_engine_t *e = (fake_engine_t *)h;
    e->process_calls++;
    e->last_in_samples = in_samples;
    e->last_max_out    = max_out;
    if (e->fail_status != 0) {
        return e->fail_status;
    }
    int n = in_samples / 2;
    if (n > max_out) {
        return 1;
    }
    for (int i = 0; i < n; i++) {
        out[i] = in[i * 2];
    }
    *out_samples = n;
    return 0;
}

/* AEC：记录参考是否到位，输出 = 近端（假装完美消除）。
 * **交错总数口径**（与真实 AudioEngine_Aec_Process 一致）：in_samples 与写回的
 * out_samples 都是 rate/100 × 通道数，max_out 也必须 ≥ 它。 */
static int fake_aec_process(void *h, const int16_t *in, const int16_t *ref, int in_samples,
                            int16_t *out, int max_out, int *out_samples, int *flag)
{
    (void)flag;
    fake_engine_t *e = (fake_engine_t *)h;
    e->process_calls++;
    e->last_in_samples  = in_samples;
    e->last_max_out     = max_out;
    e->last_ref_nonnull = (ref != NULL);
    if (e->fail_status != 0) {
        return e->fail_status;
    }
    if (max_out < in_samples) {
        return 1;
    }
    memcpy(out, in, (size_t)in_samples * sizeof(int16_t));
    *out_samples = in_samples;
    return 0;
}

/* 引擎状态码 → aura_err_t。由引擎侧提供（状态码是引擎的 ABI，适配层不硬编码一份
 * 等价枚举 —— 引擎改了值，硬编码的那份只会安静地分类错）。 */
static aura_err_t fake_map_status(int status)
{
    switch (status) {
    case 6:  return AURA_ERR_DSP;         /* PROCESS_FAILED */
    case 5:  return AURA_ERR_STATE;       /* NOT_INITIALIZED */
    case 7:  return AURA_ERR_INVALID_ARG; /* INVALID_PARAM */
    default: return AURA_ERR_FAIL;
    }
}

/* ============================== 观测节点（pts 追踪） ==============================
 * 直接注册一个普通算法（不走 DSP 适配层）：既能断言"链上产出的 pts 是什么"，
 * 也顺带验证了注册表里 DSP/NN 两族可以混在一条链上。 */
#define TAP_MAX 16
typedef struct {
    uint64_t pts[TAP_MAX];
    uint32_t rate[TAP_MAX];
    uint32_t frames[TAP_MAX];
    uint32_t channels[TAP_MAX];
    uint32_t count;
} tap_trace_t;

static tap_trace_t g_trace;

static aura_err_t tap_process_audio(aura_node_t *self, const aura_audio_frame_t *f)
{
    (void)self;
    if (g_trace.count < TAP_MAX) {
        uint32_t i         = g_trace.count++;
        g_trace.pts[i]     = f->pts_us;
        g_trace.rate[i]    = f->sample_rate;
        g_trace.frames[i]  = f->frame_count;
        g_trace.channels[i] = f->channels;
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
    caps.consumes_audio = true; /* 纯消费者 = 链终点 */
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

/* ============================== 描述表 ============================== */

static aura_dsp_desc_t g_fake_descs[] = {
    {
        .name          = "fake_ns",
        .io_kind       = AURA_DSP_IO_1TO1,
        .map_status    = fake_map_status,
        .create        = fake_create,
        .destroy       = fake_destroy,
        .init          = fake_init,
        .set_param     = fake_set_param,
        .deinit        = fake_deinit,
        .reset         = fake_reset,
        .process       = fake_1to1_process,
        .init_cfg_size = 16,
        .rt_cfg_size   = 16,
    },
    {
        .name       = "fake_vad",
        .io_kind    = AURA_DSP_IO_1TO0,
        .map_status = fake_map_status,
        .create     = fake_create,
        .destroy    = fake_destroy,
        .init       = fake_init,
        .deinit     = fake_deinit,
        .reset      = fake_reset,
        .process    = fake_vad_process,
        /* 标志边沿 → 事件：与真实 VAD 的接法一致 */
        .flag_rise_type = AURA_EVENT_VAD_SPEECH_START,
        .flag_fall_type = AURA_EVENT_VAD_SPEECH_END,
        .init_cfg_size  = 16,
    },
    {
        .name          = "fake_src",
        .io_kind       = AURA_DSP_IO_RESAMPLE,
        .map_status    = fake_map_status,
        .create        = fake_create,
        .destroy       = fake_destroy,
        .init          = fake_init,
        .deinit        = fake_deinit,
        .reset         = fake_reset,
        .process       = fake_resample_process,
        .init_cfg_size = 16,
    },
    {
        .name          = "fake_aec",
        .io_kind       = AURA_DSP_IO_2TO1,
        .map_status    = fake_map_status,
        .create        = fake_create,
        .destroy       = fake_destroy,
        .init          = fake_init,
        .deinit        = fake_deinit,
        .reset         = fake_reset,
        .process       = fake_aec_process,
        /* 与真实 aec3 一致：Process 用交错总数口径 */
        .interleaved_total = true,
        .init_cfg_size = 16,
    },
};

#define FAKE_DESC_COUNT (sizeof(g_fake_descs) / sizeof(g_fake_descs[0]))

static void setup(void)
{
    aura_algo_reset();
    g_engine_count = 0;
    memset(g_engines, 0, sizeof(g_engines));
    memset(&g_trace, 0, sizeof(g_trace));
    AURA_ASSERT(aura_dsp_register_all(g_fake_descs, (uint32_t)FAKE_DESC_COUNT) == AURA_OK);
    AURA_ASSERT(aura_algo_register(&TAP_DESC) == AURA_OK);
}

/* 建一条链并启动；nodes 用于后续取节点句柄。 */
static aura_pipeline_t *build_and_start(const char *syntax, aura_chain_t *c, aura_node_t **nodes,
                                        uint32_t *n)
{
    aura_pipeline_t *p = aura_pipeline_create(NULL);
    AURA_ASSERT(p != NULL);
    AURA_ASSERT(aura_chain_parse(syntax, c) == AURA_OK);
    AURA_ASSERT_EQ(aura_chain_build(p, c, nodes, n), AURA_OK);
    AURA_ASSERT_EQ(aura_pipeline_start(p), AURA_OK);
    return p;
}

/* 按固定 pts 投喂 count 帧（每帧 160 样本 @16k）。 */
static void feed_pts(aura_pipeline_t *p, int count, uint64_t pts_us)
{
    static int16_t pcm[160];
    for (int i = 0; i < 160; i++) {
        pcm[i] = (int16_t)(i + 1);
    }
    for (int k = 0; k < count; k++) {
        AURA_ASSERT_EQ(aura_pipeline_feed(p, pcm, 160, 1, AURA_SAMPLE_S16, pts_us, 200), AURA_OK);
    }
}

/* 多通道投喂：一帧 ch 路、每路 160 样本（16k / 10ms）。 */
static void feed_multi(aura_pipeline_t *p, int count, uint32_t ch, uint64_t pts_us)
{
    static int16_t pcm[160 * 2];
    for (int i = 0; i < 160 * 2; i++) {
        pcm[i] = (int16_t)(i + 1);
    }
    for (int k = 0; k < count; k++) {
        AURA_ASSERT_EQ(aura_pipeline_feed(p, pcm, 160, ch, AURA_SAMPLE_S16, pts_us, 200), AURA_OK);
    }
}

/* 投喂 count 个**部分帧**（samples 个样本）。 */
static void feed_part(aura_pipeline_t *p, int count, uint32_t samples)
{
    static int16_t pcm[160];
    memset(pcm, 0, sizeof(pcm));
    for (int k = 0; k < count; k++) {
        AURA_ASSERT_EQ(aura_pipeline_feed(p, pcm, samples, 1, AURA_SAMPLE_S16, 0, 200), AURA_OK);
    }
}

static void teardown(aura_pipeline_t *p, aura_node_t **nodes, uint32_t n)
{
    aura_pipeline_stop(p);
    aura_pipeline_destroy(p);
    aura_chain_destroy_nodes(nodes, n);
}

/* ============================== 用例 ============================== */

static void test_1to1_passthrough(void)
{
    setup();
    aura_chain_t     c;
    aura_node_t     *nodes[AURA_CHAIN_MAX_LINKS];
    uint32_t         n = 0;
    aura_pipeline_t *p = build_and_start("fake_ns, fake_ns", &c, nodes, &n);
    AURA_ASSERT_EQ(n, 2);

    engine_at(1)->gain = 2; /* 只有第二级放大，用来区分两级各自的计数 */
    feed_pts(p, 3, 0);
    AURA_ASSERT_EQ(aura_pipeline_wait_drained(p, 2000), AURA_OK);

    AURA_ASSERT_EQ(engine_at(0)->process_calls, 3);
    AURA_ASSERT_EQ(engine_at(1)->process_calls, 3);
    /* 帧长按 rate/100 重切（16k → 160 样本/帧） */
    AURA_ASSERT_EQ(engine_at(0)->last_in_samples, 160);
    AURA_ASSERT(engine_at(0)->inited);

    aura_dsp_stats_t s0, s1;
    AURA_ASSERT_EQ(aura_dsp_get_stats(nodes[0], &s0), AURA_OK);
    AURA_ASSERT_EQ(aura_dsp_get_stats(nodes[1], &s1), AURA_OK);
    AURA_ASSERT_EQ(s0.frames_in, 3);
    AURA_ASSERT_EQ(s0.frames_out, 3);
    AURA_ASSERT_EQ(s1.frames_in, 3);
    AURA_ASSERT_EQ(s1.process_errors, 0);

    teardown(p, nodes, n);
}

static void test_accumulate_partial_frames(void)
{
    setup();
    aura_chain_t     c;
    aura_node_t     *nodes[AURA_CHAIN_MAX_LINKS];
    uint32_t         n = 0;
    aura_pipeline_t *p = build_and_start("fake_ns", &c, nodes, &n);
    AURA_ASSERT_EQ(n, 1);

    /* 引擎要求 160 样本/帧，按 100 样本投喂：三次（100+100+100）
     * 恰好凑出 1 帧，余 140 留在缓冲里。 */
    feed_part(p, 3, 100);
    AURA_ASSERT_EQ(aura_pipeline_wait_drained(p, 2000), AURA_OK);

    AURA_ASSERT_EQ(engine_at(0)->process_calls, 1);
    AURA_ASSERT_EQ(engine_at(0)->last_in_samples, 160);

    aura_dsp_stats_t s;
    AURA_ASSERT_EQ(aura_dsp_get_stats(nodes[0], &s), AURA_OK);
    AURA_ASSERT_EQ(s.frames_in, 1);
    AURA_ASSERT_EQ(s.samples_in, 160);

    teardown(p, nodes, n);
}

static void test_vad_flag_edges(void)
{
    setup();
    aura_chain_t     c;
    aura_node_t     *nodes[AURA_CHAIN_MAX_LINKS];
    uint32_t         n = 0;

    /* vad 是纯消费者 → 挂在主线旁路，主线继续走到后面的 fake_ns */
    aura_pipeline_t *p = build_and_start("fake_ns, tee(fake_vad), fake_ns", &c, nodes, &n);
    AURA_ASSERT_EQ(n, 3);
    /* 拓扑：ns[0] 扇出到 vad[1] 与 ns[2]（观察者先接，主线后接） */
    AURA_ASSERT_EQ(nodes[0]->next_audio_count, 2);
    AURA_ASSERT(nodes[0]->next_audio[0] == nodes[1]);
    AURA_ASSERT(nodes[0]->next_audio[1] == nodes[2]);

    aura_event_bus_t *bus = aura_pipeline_bus(p);
    AURA_ASSERT(bus != NULL);
    const uint32_t vad_mask =
        (1u << AURA_EVENT_VAD_SPEECH_START) | (1u << AURA_EVENT_VAD_SPEECH_END);

    /* 连续 3 帧"有语音"：只应产生 1 次上升沿 + 1 个事件（只发边沿，不发每帧） */
    engine_at(1)->vad_flag = 1;
    feed_pts(p, 3, 0);
    aura_event_t ev;
    AURA_ASSERT_EQ(aura_event_bus_wait(bus, vad_mask, &ev, 2000), AURA_OK);
    AURA_ASSERT_EQ(ev.type, AURA_EVENT_VAD_SPEECH_START);

    aura_dsp_stats_t sv;
    AURA_ASSERT_EQ(aura_dsp_get_stats(nodes[1], &sv), AURA_OK);
    AURA_ASSERT_EQ(sv.frames_in, 3);
    AURA_ASSERT_EQ(sv.flag_rise, 1);
    AURA_ASSERT_EQ(sv.flag_fall, 0);
    AURA_ASSERT_EQ(sv.frames_out, 0); /* VAD 不产出音频，不该被算作产出帧 */

    /* 转为静音 → 下降沿 */
    engine_at(1)->vad_flag = 0;
    feed_pts(p, 2, 0);
    AURA_ASSERT_EQ(aura_event_bus_wait(bus, vad_mask, &ev, 2000), AURA_OK);
    AURA_ASSERT_EQ(ev.type, AURA_EVENT_VAD_SPEECH_END);
    AURA_ASSERT_EQ(aura_dsp_get_stats(nodes[1], &sv), AURA_OK);
    AURA_ASSERT_EQ(sv.flag_fall, 1);
    AURA_ASSERT_EQ(sv.flag_rise, 1);

    /* 观察者不截断主线：主线照样收到全部 5 帧 */
    AURA_ASSERT_EQ(aura_pipeline_wait_drained(p, 2000), AURA_OK);
    aura_dsp_stats_t s2;
    AURA_ASSERT_EQ(aura_dsp_get_stats(nodes[2], &s2), AURA_OK);
    AURA_ASSERT_EQ(s2.frames_in, 5);

    teardown(p, nodes, n);
}

static void test_resample_shape(void)
{
    setup();
    aura_chain_t     c;
    aura_node_t     *nodes[AURA_CHAIN_MAX_LINKS];
    uint32_t         n = 0;
    aura_pipeline_t *p = aura_pipeline_create(NULL);
    AURA_ASSERT(p != NULL);
    AURA_ASSERT(aura_chain_parse("fake_src, fake_ns", &c) == AURA_OK);
    /* 16k → 8k：链形状必须传播到下游（下游按 80 样本/帧收，而不是 160） */
    AURA_ASSERT(aura_algo_params_set_i32(aura_chain_params(&c, 0), "out_sample_rate", 8000) ==
                AURA_OK);
    AURA_ASSERT_EQ(aura_chain_build(p, &c, nodes, &n), AURA_OK);
    AURA_ASSERT_EQ(aura_pipeline_start(p), AURA_OK);
    AURA_ASSERT_EQ(n, 2);

    feed_pts(p, 3, 0);
    AURA_ASSERT_EQ(aura_pipeline_wait_drained(p, 2000), AURA_OK);

    aura_dsp_stats_t ss, sn;
    AURA_ASSERT_EQ(aura_dsp_get_stats(nodes[0], &ss), AURA_OK);
    AURA_ASSERT_EQ(aura_dsp_get_stats(nodes[1], &sn), AURA_OK);

    /* src 收 3 帧 × 160 样本，产出 3 帧 × 80 样本 */
    AURA_ASSERT_EQ(engine_at(0)->last_in_samples, 160);
    AURA_ASSERT_EQ(ss.frames_in, 3);
    AURA_ASSERT_EQ(ss.frames_out, 3);
    /* 下游以 80 样本/帧收（8000/100），总样本数守恒 */
    AURA_ASSERT_EQ(sn.frames_in, 3);
    AURA_ASSERT_EQ(sn.samples_in, 240);

    teardown(p, nodes, n);
}

/* pts 契约的两半：普通节点被 pipeline 纠正，变帧率节点不被纠正。
 * 用"输入 pts 恒定不变"作探针 —— 若被纠正，输出 pts 就是一串常数。 */
static void test_pts_correction(void)
{
    setup();
    aura_chain_t  c;
    aura_node_t  *nodes[AURA_CHAIN_MAX_LINKS];
    uint32_t      n = 0;

    /* ① 普通节点：产出 pts 被改写为输入 pts（500000 恒定） */
    aura_pipeline_t *p1 = build_and_start("fake_ns, tap", &c, nodes, &n);
    feed_pts(p1, 3, 500000);
    AURA_ASSERT_EQ(aura_pipeline_wait_drained(p1, 2000), AURA_OK);
    AURA_ASSERT_EQ(g_trace.count, 3);
    for (uint32_t i = 0; i < g_trace.count; i++) {
        AURA_ASSERT_EQ(g_trace.pts[i], 500000);
    }
    teardown(p1, nodes, n);

    /* ② 变帧率节点：输出是一条自算的单调时间轴（500000, 510000, 520000）。
     * 输入 pts 抖动不该让输出时间轴跟着跳 —— 覆盖回输入时基会让重采样后的
     * 帧（80 样本 @8k）配上一个按 16k 走的 pts，下游对齐随之错乱。 */
    setup();
    aura_pipeline_t *p2 = aura_pipeline_create(NULL);
    AURA_ASSERT(p2 != NULL);
    AURA_ASSERT(aura_chain_parse("fake_src, tap", &c) == AURA_OK);
    AURA_ASSERT(aura_algo_params_set_i32(aura_chain_params(&c, 0), "out_sample_rate", 8000) ==
                AURA_OK);
    AURA_ASSERT_EQ(aura_chain_build(p2, &c, nodes, &n), AURA_OK);
    AURA_ASSERT_EQ(aura_pipeline_start(p2), AURA_OK);

    feed_pts(p2, 3, 500000);
    AURA_ASSERT_EQ(aura_pipeline_wait_drained(p2, 2000), AURA_OK);
    AURA_ASSERT_EQ(g_trace.count, 3);
    AURA_ASSERT_EQ(g_trace.pts[0], 500000);          /* 首帧锚定输入 pts（对齐时刻） */
    AURA_ASSERT_EQ(g_trace.pts[1], 500000 + 10000);  /* 其后按 80/8000 = 10ms 自算 */
    AURA_ASSERT_EQ(g_trace.pts[2], 500000 + 20000);
    /* 输出帧头必须带**输出**采样率，否则下游按 16k 去解释 8k 数据 */
    AURA_ASSERT_EQ(g_trace.rate[0], 8000);
    AURA_ASSERT_EQ(g_trace.frames[0], 80);

    teardown(p2, nodes, n);
}

static void test_aec_ref_missing(void)
{
    setup();
    aura_chain_t     c;
    aura_node_t     *nodes[AURA_CHAIN_MAX_LINKS];
    uint32_t         n = 0;
    aura_pipeline_t *p = build_and_start("fake_aec, fake_ns", &c, nodes, &n);
    AURA_ASSERT_EQ(n, 2);

    /* aec 声明了 consumes_ref → pipeline 给它建了参考队列 */
    AURA_ASSERT(nodes[0]->in_ref != NULL);

    /* 只喂近端不喂参考：每帧都该记一次 ref_missed，且引擎必须拿到非空参考指针
     * （喂静音而不是 NULL —— 引擎不该为框架的降级负责）。 */
    feed_pts(p, 3, 0);
    AURA_ASSERT_EQ(aura_pipeline_wait_drained(p, 2000), AURA_OK);

    aura_dsp_stats_t s;
    AURA_ASSERT_EQ(aura_dsp_get_stats(nodes[0], &s), AURA_OK);
    AURA_ASSERT_EQ(s.frames_in, 3);
    AURA_ASSERT_EQ(s.ref_missed, 3);
    AURA_ASSERT(engine_at(0)->last_ref_nonnull);

    /* 补上参考后不再缺失：3 帧参考（各 160 样本）恰被随后 3 帧近端取走。
     * 配对按 **pts**：参考的 pts 必须是"对应近端帧将要到达的时刻"。上面 3 帧近端
     * 已占掉 [0,30ms]，故参考从 30000 起；喂成过去时刻的参考会被判过期丢弃
     * （计入 pipeline.ref_stale），AEC 反而还是拿不到 —— 这正是 ref_stale 要暴露的事。 */
    static int16_t ref[160];
    memset(ref, 0, sizeof(ref));
    for (int k = 0; k < 3; k++) {
        AURA_ASSERT_EQ(aura_pipeline_feed_ref(p, ref, 160, 1, AURA_SAMPLE_S16,
                                              (uint64_t)(30000 + k * 10000), 200),
                       AURA_OK);
    }
    /* 参考不能单独等 drain：没配到近端的参考会停在节点配对槽里等伙伴。 */
    feed_pts(p, 3, 0); /* pts 自动推进：30000 / 40000 / 50000 */
    AURA_ASSERT_EQ(aura_pipeline_wait_drained(p, 2000), AURA_OK);

    AURA_ASSERT_EQ(aura_dsp_get_stats(nodes[0], &s), AURA_OK);
    AURA_ASSERT_EQ(s.frames_in, 6);
    AURA_ASSERT_EQ(s.ref_missed, 3); /* 有参考的 3 帧不该再计缺失 */

    aura_pipeline_stats_t ps;
    aura_pipeline_stats(p, &ps);
    AURA_ASSERT_EQ(ps.ref_in, 3);
    AURA_ASSERT_EQ(ps.frames_in, 6); /* 参考帧不混进主链的 frames_in */

    teardown(p, nodes, n);
}

/* 多通道口径：AEC 用"交错总数"（in/max_out/out_samples 都是 rate/100×通道数），
 * NS 那一类用"单通道数"作 in/out、用交错总数作 max_out。
 * 单通道时两者数值相同，所以这个 bug 只在多麦场景暴露 —— 而多麦恰恰是本项目的
 * 主场景（aec3 → bf 之前就是 2 路）。 */
static void test_multichannel_sample_convention(void)
{
    setup();
    aura_chain_t     c;
    aura_node_t     *nodes[AURA_CHAIN_MAX_LINKS];
    uint32_t         n = 0;

    /* 链首 2 路：形状来自 chain 描述（与 .conf 的 channels 同一条路） */
    aura_pipeline_t *p = aura_pipeline_create(NULL);
    AURA_ASSERT(p != NULL);
    AURA_ASSERT(aura_chain_parse("fake_aec, tap", &c) == AURA_OK);
    c.channels = 2;
    AURA_ASSERT_EQ(aura_chain_build(p, &c, nodes, &n), AURA_OK);
    AURA_ASSERT_EQ(aura_pipeline_start(p), AURA_OK);

    engine_at(0)->channels = 2;
    engine_at(0)->gain     = 2;

    /* 每帧 160 样本/路 → 交错 320；参考也必须是 2 路（引擎按 capture 的通道数读 far） */
    static int16_t ref[160 * 2];
    memset(ref, 0, sizeof(ref));
    for (int k = 0; k < 3; k++) {
        AURA_ASSERT_EQ(aura_pipeline_feed_ref(p, ref, 160, 2, AURA_SAMPLE_S16,
                                              (uint64_t)k * 10000, 200),
                       AURA_OK);
    }
    feed_multi(p, 3, 2, 0);
    AURA_ASSERT_EQ(aura_pipeline_wait_drained(p, 2000), AURA_OK);

    /* AEC 拿到的是交错总数（320），max_out 也得是 320 —— 传 160 会被引擎按
     * "invalid in_samples/max_out" 每帧拒绝，而 aec3 是 drop_on_error，
     * 表现就是"链上没声音"而不是报错。 */
    AURA_ASSERT_EQ(engine_at(0)->last_in_samples, 320);
    AURA_ASSERT_EQ(engine_at(0)->last_max_out, 320);
    AURA_ASSERT(engine_at(0)->last_ref_nonnull);

    aura_dsp_stats_t s;
    AURA_ASSERT_EQ(aura_dsp_get_stats(nodes[0], &s), AURA_OK);
    AURA_ASSERT_EQ(s.process_errors, 0);
    AURA_ASSERT_EQ(s.ref_missed, 0); /* 2 路参考按交错总数取走后恰好一帧一帧对上 */
    AURA_ASSERT_EQ(s.frames_out, 3);

    /* 下游收到的仍是 2 路 × 160 样本（引擎写回的交错总数被换算回每通道帧数） */
    AURA_ASSERT_EQ(g_trace.count, 3);
    AURA_ASSERT_EQ(g_trace.frames[0], 160);
    AURA_ASSERT_EQ(g_trace.channels[0], 2);
    AURA_ASSERT_EQ(g_trace.rate[0], 16000);

    teardown(p, nodes, n);
}

/* 多通道 NS 那一类的口径：in_samples = 每通道 160，max_out = 交错 320，
 * 写回的 out_samples = 每通道 160。传小了引擎报参数错误（§test_multichannel 的镜像）。 */
static void test_1to1_multichannel_maxout(void)
{
    setup();
    aura_chain_t     c;
    aura_node_t     *nodes[AURA_CHAIN_MAX_LINKS];
    uint32_t         n = 0;
    aura_pipeline_t *p = aura_pipeline_create(NULL);
    AURA_ASSERT(p != NULL);
    AURA_ASSERT(aura_chain_parse("fake_ns, tap", &c) == AURA_OK);
    c.channels = 2;
    AURA_ASSERT_EQ(aura_chain_build(p, &c, nodes, &n), AURA_OK);
    AURA_ASSERT_EQ(aura_pipeline_start(p), AURA_OK);

    engine_at(0)->channels = 2;
    feed_multi(p, 2, 2, 0);
    AURA_ASSERT_EQ(aura_pipeline_wait_drained(p, 2000), AURA_OK);

    AURA_ASSERT_EQ(engine_at(0)->last_in_samples, 160); /* 每通道 */
    AURA_ASSERT_EQ(engine_at(0)->last_max_out, 320);    /* 交错容量 */

    aura_dsp_stats_t s;
    AURA_ASSERT_EQ(aura_dsp_get_stats(nodes[0], &s), AURA_OK);
    AURA_ASSERT_EQ(s.process_errors, 0);
    AURA_ASSERT_EQ(s.frames_out, 2);
    AURA_ASSERT_EQ(g_trace.count, 2);
    AURA_ASSERT_EQ(g_trace.frames[0], 160);
    AURA_ASSERT_EQ(g_trace.channels[0], 2);

    teardown(p, nodes, n);
}

/* 参考缓冲的多通道配对：2 路参考按交错总数入队、按帧取走，不该堆积也不该误判缺失。 */
static void test_aec_ref_2ch_pairing(void)
{
    setup();
    aura_chain_t     c;
    aura_node_t     *nodes[AURA_CHAIN_MAX_LINKS];
    uint32_t         n = 0;
    aura_pipeline_t *p = aura_pipeline_create(NULL);
    AURA_ASSERT(p != NULL);
    AURA_ASSERT(aura_chain_parse("fake_aec", &c) == AURA_OK);
    c.channels = 2;
    AURA_ASSERT_EQ(aura_chain_build(p, &c, nodes, &n), AURA_OK);
    AURA_ASSERT_EQ(aura_pipeline_start(p), AURA_OK);
    engine_at(0)->channels = 2;

    static int16_t ref[160 * 2];
    memset(ref, 0, sizeof(ref));
    /* 先来 2 帧参考、再来 2 帧近端：每帧近端恰好取走一帧参考 */
    for (int k = 0; k < 2; k++) {
        AURA_ASSERT_EQ(aura_pipeline_feed_ref(p, ref, 160, 2, AURA_SAMPLE_S16,
                                              (uint64_t)k * 10000, 200),
                       AURA_OK);
    }
    feed_multi(p, 2, 2, 0);
    AURA_ASSERT_EQ(aura_pipeline_wait_drained(p, 2000), AURA_OK);

    aura_dsp_stats_t s;
    AURA_ASSERT_EQ(aura_dsp_get_stats(nodes[0], &s), AURA_OK);
    AURA_ASSERT_EQ(s.frames_in, 2);
    AURA_ASSERT_EQ(s.ref_missed, 0);
    AURA_ASSERT_EQ(s.frames_out, 2);

    /* 参考帧不混进主链统计 */
    aura_pipeline_stats_t ps;
    aura_pipeline_stats(p, &ps);
    AURA_ASSERT_EQ(ps.ref_in, 2);
    AURA_ASSERT_EQ(ps.frames_in, 2);

    teardown(p, nodes, n);
}

static void test_error_passthrough(void)
{
    setup();
    aura_chain_t     c;
    aura_node_t     *nodes[AURA_CHAIN_MAX_LINKS];
    uint32_t         n = 0;
    aura_pipeline_t *p = build_and_start("fake_ns, fake_ns", &c, nodes, &n);
    AURA_ASSERT_EQ(n, 2);

    /* 第一级引擎失败：按默认策略把原始输入透传下去，链路不断。
     * （3A 挂掉时"没降噪的语音"远好于静音。） */
    engine_at(0)->fail_status = 6; /* PROCESS_FAILED → AURA_ERR_DSP */
    feed_pts(p, 2, 0);
    AURA_ASSERT_EQ(aura_pipeline_wait_drained(p, 2000), AURA_OK);

    aura_dsp_stats_t s0, s1;
    AURA_ASSERT_EQ(aura_dsp_get_stats(nodes[0], &s0), AURA_OK);
    AURA_ASSERT_EQ(aura_dsp_get_stats(nodes[1], &s1), AURA_OK);
    AURA_ASSERT_EQ(s0.process_errors, 2);
    AURA_ASSERT_EQ(s0.frames_out, 0); /* 失败帧不算引擎产出 */
    AURA_ASSERT_EQ(s1.process_errors, 0);
    AURA_ASSERT_EQ(s1.frames_in, 2);  /* 但透传保住了下游 */

    aura_pipeline_stats_t ps;
    aura_pipeline_stats(p, &ps);
    AURA_ASSERT(ps.node_errors >= 2); /* 错误必须上抛，不得静默 */

    teardown(p, nodes, n);
}

static void test_lifecycle_and_flush(void)
{
    setup();
    aura_chain_t     c;
    aura_node_t     *nodes[AURA_CHAIN_MAX_LINKS];
    uint32_t         n = 0;
    aura_pipeline_t *p = build_and_start("fake_ns", &c, nodes, &n);

    AURA_ASSERT(engine_at(0)->inited); /* start → Init */

    /* 打断场景：喂半帧后 flush，残留的攒帧必须清掉 —— 否则打断后的第一帧会把
     * 打断前的尾巴接上去（半个字的旧语音 + 新的半个字）。 */
    feed_part(p, 1, 100);
    AURA_ASSERT_EQ(aura_pipeline_wait_drained(p, 2000), AURA_OK);
    AURA_ASSERT_EQ(engine_at(0)->process_calls, 0); /* 只攒到 100，不够一帧 */

    AURA_ASSERT_EQ(aura_pipeline_flush(p), AURA_OK);
    AURA_ASSERT(engine_at(0)->reset_calls >= 1);

    /* flush 后补帧：100 + 50 = 150 仍不够一帧 —— 若残留未清，第一个 100 落进旧缓冲
     * 就会凑满 160，这里早该出帧了。 */
    feed_part(p, 1, 100);
    feed_part(p, 1, 50);
    AURA_ASSERT_EQ(aura_pipeline_wait_drained(p, 2000), AURA_OK);
    AURA_ASSERT_EQ(engine_at(0)->process_calls, 0);

    /* 再补 10 凑满 160 → 出 1 帧 */
    feed_part(p, 1, 10);
    AURA_ASSERT_EQ(aura_pipeline_wait_drained(p, 2000), AURA_OK);
    AURA_ASSERT_EQ(engine_at(0)->process_calls, 1);
    AURA_ASSERT_EQ(engine_at(0)->last_in_samples, 160);

    aura_pipeline_stop(p);
    AURA_ASSERT(engine_at(0)->deinit_calls >= 1);
    AURA_ASSERT_EQ(engine_at(0)->destroy_calls, 1); /* 句柄必须释放，不随节点结构体一起漏 */

    /* stop 后仍需能给节点收尾（destroy 会再 stop 一次，必须幂等） */
    aura_pipeline_destroy(p);
    AURA_ASSERT_EQ(engine_at(0)->destroy_calls, 1);

    aura_chain_destroy_nodes(nodes, n);
}

AURA_TEST(test_1to1_passthrough);
AURA_TEST(test_accumulate_partial_frames);
AURA_TEST(test_vad_flag_edges);
AURA_TEST(test_resample_shape);
AURA_TEST(test_pts_correction);
AURA_TEST(test_aec_ref_missing);
AURA_TEST(test_multichannel_sample_convention);
AURA_TEST(test_1to1_multichannel_maxout);
AURA_TEST(test_aec_ref_2ch_pairing);
AURA_TEST(test_error_passthrough);
AURA_TEST(test_lifecycle_and_flush);

AURA_TEST_MAIN();
