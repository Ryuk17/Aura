/* 算法统一接口单测：注册表 / 参数 / 链语法 / 链组装 / 流形状传播 / 观察者不截断主线
 *
 * 这里用**测试自带的 mock 描述表**而不是真实算法：契约层的正确性与 TrickRoom、
 * MNN 是否在场无关，纯 C 全平台可跑（真实适配器另有 test_dsp_adapter.c）。
 */
#include "aura_test.h"

#include <stdio.h>

#include "agent_export.h" /* agent 层链装配用例（见文件末尾） */
#include "core/algorithm.h"
#include "core/config/config.h"
#include "core/pipeline/pipeline.h"

/* ============================================================ mock 节点 */

#define MOCK_MAX 16

typedef struct mock_node {
    aura_node_t base;
    char        namebuf[AURA_CHAIN_NAME_MAX];

    /* 工厂收到的生效形状 —— 用于断言链上传播 */
    uint32_t in_rate;
    uint32_t in_channels;

    /* 运行统计 */
    uint64_t frames_in;
    uint64_t frames_out;
    uint64_t first_pts;
    uint64_t last_pts;
    uint32_t last_rate;     /* 收到帧的帧头采样率 */
    uint16_t last_channels; /* 收到帧的帧头通道数 */
} mock_node_t;

static mock_node_t *g_created[MOCK_MAX];
static uint32_t     g_created_count;

static void mock_reset(void)
{
    g_created_count = 0;
}

static mock_node_t *mock_new(const aura_algo_params_t *params)
{
    if (g_created_count >= MOCK_MAX) {
        return NULL;
    }
    mock_node_t *m = (mock_node_t *)calloc(1, sizeof(*m));
    if (m == NULL) {
        return NULL;
    }
    /* 契约：实例名指向链描述，工厂必须自己拷一份。 */
    const char *nm = (params->instance_name != NULL) ? params->instance_name : "mock";
    snprintf(m->namebuf, sizeof(m->namebuf), "%s", nm);
    m->in_rate     = params->sample_rate;
    m->in_channels = params->in_channels;
    g_created[g_created_count++] = m;
    return m;
}

/* --------------------------- 透传节点（消费+产出） --------------------------- */

static aura_err_t pass_process(aura_node_t *self, const aura_audio_frame_t *f)
{
    mock_node_t *m = (mock_node_t *)self;
    m->frames_in++;
    m->last_rate     = f->sample_rate;
    m->last_channels = f->channels;
    m->frames_out++;
    return aura_node_emit_audio(self, f);
}

static const aura_node_ops_t pass_ops = {.process_audio = pass_process};

static aura_node_t *pass_create(const aura_algo_params_t *params, aura_err_t *err)
{
    mock_node_t *m = mock_new(params);
    if (m == NULL) {
        if (err) {
            *err = AURA_ERR_NOMEM;
        }
        return NULL;
    }
    aura_node_init(&m->base, m->namebuf, &pass_ops,
                   &(aura_node_caps_t){.consumes_audio = true, .produces_audio = true});
    m->base.priv = m;
    return &m->base;
}

/* ------------------------------ 终点节点（只消费） ------------------------------ */

static aura_err_t sink_process(aura_node_t *self, const aura_audio_frame_t *f)
{
    mock_node_t *m = (mock_node_t *)self;
    if (m->frames_in == 0) {
        m->first_pts = f->pts_us;
    }
    m->frames_in++;
    m->last_pts      = f->pts_us;
    m->last_rate     = f->sample_rate;
    m->last_channels = f->channels;
    return AURA_OK;
}

static const aura_node_ops_t sink_ops = {.process_audio = sink_process};

static aura_node_t *sink_create(const aura_algo_params_t *params, aura_err_t *err)
{
    mock_node_t *m = mock_new(params);
    if (m == NULL) {
        if (err) {
            *err = AURA_ERR_NOMEM;
        }
        return NULL;
    }
    aura_node_init(&m->base, m->namebuf, &sink_ops, &(aura_node_caps_t){.consumes_audio = true});
    return &m->base;
}

/* --------------------- 降采样节点（变帧率：16k → 8k，2:1） --------------------- */

static aura_err_t decim_process(aura_node_t *self, const aura_audio_frame_t *f)
{
    mock_node_t *m = (mock_node_t *)self;
    m->frames_in++;

    /* 每两个样本取一个：帧长减半，帧头采样率同步改写。
     * pts 自算并**故意**偏离输入（+5000us）—— 若 pipeline 仍然强制覆盖 pts，
     * 下游就会看到输入 pts 而不是这个值，测试据此断言"变帧率节点不被纠正"。 */
    static int16_t out[AURA_AUDIO_FRAME_MAX_BYTES / 2];
    const int16_t *in = (const int16_t *)f->data;
    uint32_t       n  = f->frame_count / 2;
    for (uint32_t i = 0; i < n; i++) {
        out[i] = in[i * 2];
    }

    aura_audio_frame_t g;
    memset(&g, 0, sizeof(g));
    g.pts_us     = f->pts_us + 5000;
    g.sample_rate = f->sample_rate / 2;
    g.channels   = 1;
    g.frame_count = (uint16_t)n;
    g.fmt        = AURA_SAMPLE_S16;
    g.data_bytes = n * 2;
    g.data       = (uint8_t *)out;
    m->frames_out++;
    return aura_node_emit_audio(self, &g);
}

static const aura_node_ops_t decim_ops = {.process_audio = decim_process};

/* shape_out：读自己的 extra 参数决定输出形状，与真实 SRC 的做法一致。 */
static aura_err_t decim_shape(const aura_algo_params_t *params, uint32_t *rate, uint32_t *ch)
{
    *rate = (uint32_t)aura_algo_params_i32(params, "out_sample_rate", 8000);
    *ch   = 1;
    return AURA_OK;
}

static aura_node_t *decim_create(const aura_algo_params_t *params, aura_err_t *err)
{
    mock_node_t *m = mock_new(params);
    if (m == NULL) {
        if (err) {
            *err = AURA_ERR_NOMEM;
        }
        return NULL;
    }
    aura_node_init(&m->base, m->namebuf, &decim_ops,
                   &(aura_node_caps_t){.consumes_audio = true,
                                       .produces_audio = true,
                                       .changes_frame_rate = true});
    return &m->base;
}

/* ============================== 描述表 ============================== */

static const aura_algo_param_spec_t decim_specs[] = {
    {"out_sample_rate", AURA_ALGO_PARAM_I32, 8000, 48000, "输出采样率"},
};

/* 四种类型各一个，用来验证 .conf 文本 → 强类型参数的转换。 */
static const aura_algo_param_spec_t pass_specs[] = {
    {"gain", AURA_ALGO_PARAM_F32, 0.0f, 4.0f, "增益"},
    {"delay_ms", AURA_ALGO_PARAM_I32, 0, 500, "参考延迟"},
    {"enabled", AURA_ALGO_PARAM_BOOL, 0, 0, "开关"},
    {"tag", AURA_ALGO_PARAM_STR, 0, 0, "标签"},
};

static const aura_algo_desc_t g_descs[] = {
    {"mock_pass",
     1,
     AURA_ALGO_KIND_DSP,
     {.consumes_audio = true, .produces_audio = true},
     NULL,
     pass_specs,
     4,
     pass_create},
    {"mock_sink",
     1,
     AURA_ALGO_KIND_DSP,
     {.consumes_audio = true, .produces_audio = false},
     NULL,
     NULL,
     0,
     sink_create},
    {"mock_decim",
     1,
     AURA_ALGO_KIND_DSP,
     {.consumes_audio = true, .produces_audio = true, .changes_frame_rate = true},
     decim_shape,
     decim_specs,
     1,
     decim_create},
};

/* 取本次创建的第 idx 个 mock 节点（按创建顺序）。 */
static mock_node_t *created(uint32_t idx)
{
    return (idx < g_created_count) ? g_created[idx] : NULL;
}

static void setup_registry(void)
{
    aura_algo_reset();
    mock_reset();
    AURA_ASSERT(aura_algo_register_all(g_descs, 3) == AURA_OK);
}

/* ============================== 用例 ============================== */

static void test_registry(void)
{
    aura_algo_reset();
    AURA_ASSERT_EQ(aura_algo_count(), 0);
    AURA_ASSERT(aura_algo_find("nope") == NULL);

    AURA_ASSERT(aura_algo_register_all(g_descs, 3) == AURA_OK);
    AURA_ASSERT_EQ(aura_algo_count(), 3);
    AURA_ASSERT_STREQ(aura_algo_at(0)->name, "mock_pass");
    AURA_ASSERT(aura_algo_at(3) == NULL);
    AURA_ASSERT(aura_algo_find("mock_sink") != NULL);

    /* 重名必须报错：静默覆盖会让"配置改了没生效"极难定位。 */
    AURA_ASSERT_EQ(aura_algo_register(&g_descs[0]), AURA_ERR_EXIST);

    /* 声明变帧率却不给 shape_out 的描述表是非法的。 */
    static const aura_algo_desc_t bad = {"bad_rate",
                                         1,
                                         AURA_ALGO_KIND_DSP,
                                         {.consumes_audio = true,
                                          .produces_audio = true,
                                          .changes_frame_rate = true},
                                         NULL,
                                         NULL,
                                         0,
                                         pass_create};
    AURA_ASSERT_EQ(aura_algo_register(&bad), AURA_ERR_INVALID_ARG);

    aura_algo_reset();
    AURA_ASSERT_EQ(aura_algo_count(), 0);
}

static void test_params(void)
{
    aura_algo_params_t p;
    memset(&p, 0, sizeof(p));

    AURA_ASSERT(aura_algo_params_set_i32(&p, "delay_ms", 40) == AURA_OK);
    AURA_ASSERT(aura_algo_params_set_f32(&p, "gain", 1.5f) == AURA_OK);
    AURA_ASSERT(aura_algo_params_set_bool(&p, "enabled", true) == AURA_OK);
    AURA_ASSERT(aura_algo_params_set_str(&p, "mode", "aggressive") == AURA_OK);
    AURA_ASSERT_EQ(p.extra_count, 4);

    AURA_ASSERT_EQ(aura_algo_params_i32(&p, "delay_ms", -1), 40);
    AURA_ASSERT(aura_algo_params_f32(&p, "gain", 0.0f) > 1.49f);
    AURA_ASSERT(aura_algo_params_bool(&p, "enabled", false));
    AURA_ASSERT_STREQ(aura_algo_params_str(&p, "mode", "x"), "aggressive");

    /* 缺省值路径 */
    AURA_ASSERT_EQ(aura_algo_params_i32(&p, "missing", 7), 7);
    AURA_ASSERT(!aura_algo_params_bool(&p, "missing", false));

    /* 同名覆盖而非追加：配置覆盖默认值依赖这个语义 */
    AURA_ASSERT(aura_algo_params_set_i32(&p, "delay_ms", 80) == AURA_OK);
    AURA_ASSERT_EQ(p.extra_count, 4);
    AURA_ASSERT_EQ(aura_algo_params_i32(&p, "delay_ms", -1), 80);
}

static void test_chain_parse(void)
{
    aura_chain_t c;

    AURA_ASSERT(aura_chain_parse("aec, ns, tee(vad,kws), asr", &c) == AURA_OK);
    AURA_ASSERT_EQ(c.count, 5);
    AURA_ASSERT_STREQ(c.links[0].algo, "aec");
    AURA_ASSERT(!c.links[0].observer_hint);
    AURA_ASSERT_STREQ(c.links[2].algo, "vad");
    AURA_ASSERT(c.links[2].observer_hint);
    AURA_ASSERT_STREQ(c.links[3].algo, "kws");
    AURA_ASSERT(c.links[3].observer_hint);
    AURA_ASSERT_STREQ(c.links[4].algo, "asr");
    AURA_ASSERT(!c.links[4].observer_hint);

    /* 空白容忍 */
    AURA_ASSERT(aura_chain_parse("  a , b  ", &c) == AURA_OK);
    AURA_ASSERT_EQ(c.count, 2);

    /* 语法错误 */
    AURA_ASSERT_EQ(aura_chain_parse("", &c), AURA_ERR_INVALID_ARG);
    AURA_ASSERT_EQ(aura_chain_parse("a,", &c), AURA_ERR_INVALID_ARG);
    AURA_ASSERT_EQ(aura_chain_parse(",a", &c), AURA_ERR_INVALID_ARG);
    AURA_ASSERT_EQ(aura_chain_parse("a,,b", &c), AURA_ERR_INVALID_ARG);
    AURA_ASSERT_EQ(aura_chain_parse("tee()", &c), AURA_ERR_INVALID_ARG);
    AURA_ASSERT_EQ(aura_chain_parse("tee(a", &c), AURA_ERR_INVALID_ARG);
    AURA_ASSERT_EQ(aura_chain_parse("teex(a)", &c), AURA_ERR_INVALID_ARG);
}

static void test_chain_build_unknown(void)
{
    setup_registry();
    aura_chain_t c;
    aura_node_t *nodes[AURA_CHAIN_MAX_LINKS];
    uint32_t     n = 0;

    aura_pipeline_t *p = aura_pipeline_create(NULL);
    AURA_ASSERT(p != NULL);

    /* 未注册的算法名：装配期就该报错，而不是运行期 */
    AURA_ASSERT(aura_chain_parse("mock_pass, nope", &c) == AURA_OK);
    AURA_ASSERT_EQ(aura_chain_build(p, &c, nodes, &n), AURA_ERR_NOT_FOUND);
    AURA_ASSERT_EQ(n, 0);
    /* 回滚：link[0] 的工厂**跑过了**（计到 1），但节点必须已从 pipeline 摘除
     * 并释放 —— 留在 pipeline 里就是"调用方拿不到句柄却仍被 stop/destroy 遍历"
     * 的悬垂引用。 */
    AURA_ASSERT_EQ(g_created_count, 1);
    AURA_ASSERT_EQ(aura_pipeline_node_count(p), 0);

    /* 未知参数键（拼错 key 不该静默失效） */
    AURA_ASSERT(aura_chain_parse("mock_decim", &c) == AURA_OK);
    aura_algo_params_set_i32(aura_chain_params(&c, 0), "out_sample_rat", 8000); /* 拼错 */
    AURA_ASSERT_EQ(aura_chain_build(p, &c, nodes, &n), AURA_ERR_INVALID_ARG);
    AURA_ASSERT_EQ(aura_pipeline_node_count(p), 0);

    /* tee() 里列了会产出音频的节点：语义冲突，装配期报错 */
    AURA_ASSERT(aura_chain_parse("mock_pass, tee(mock_pass)", &c) == AURA_OK);
    AURA_ASSERT_EQ(aura_chain_build(p, &c, nodes, &n), AURA_ERR_INVALID_ARG);
    AURA_ASSERT_EQ(aura_pipeline_node_count(p), 0);

    /* 链首就是纯消费者：没有上游可挂 */
    AURA_ASSERT(aura_chain_parse("mock_sink, mock_pass", &c) == AURA_OK);
    AURA_ASSERT_EQ(aura_chain_build(p, &c, nodes, &n), AURA_ERR_STATE);

    /* 两个变帧率节点串联：中间段采样率失去锚点，拒绝 */
    AURA_ASSERT(aura_chain_parse("mock_decim, mock_decim", &c) == AURA_OK);
    AURA_ASSERT_EQ(aura_chain_build(p, &c, nodes, &n), AURA_ERR_INVALID_ARG);

    aura_pipeline_destroy(p);
}

static void test_shape_propagation(void)
{
    setup_registry();
    aura_chain_t c;
    aura_node_t *nodes[AURA_CHAIN_MAX_LINKS];
    uint32_t     n = 0;

    aura_pipeline_t *p = aura_pipeline_create(NULL);
    AURA_ASSERT(p != NULL);

    /* pass(16k) → decim(16k→8k) → pass(8k 生效) → sink */
    AURA_ASSERT(aura_chain_parse("mock_pass, mock_decim, mock_pass, mock_sink", &c) == AURA_OK);
    aura_algo_params_set_i32(aura_chain_params(&c, 1), "out_sample_rate", 8000);
    AURA_ASSERT_EQ(aura_chain_build(p, &c, nodes, &n), AURA_OK);
    AURA_ASSERT_EQ(n, 4);
    AURA_ASSERT_EQ(aura_pipeline_node_count(p), 4);

    /* 生效形状逐级传播：decim 看到 16000，其后所有节点看到 8000 */
    AURA_ASSERT_EQ(created(0)->in_rate, 16000);
    AURA_ASSERT_EQ(created(1)->in_rate, 16000);
    AURA_ASSERT_EQ(created(2)->in_rate, 8000);
    AURA_ASSERT_EQ(created(3)->in_rate, 8000);

    /* 实例名进了节点名（同名算法在链上复用时不至于重名） */
    AURA_ASSERT_STREQ(nodes[0]->name, "mock_pass");
    AURA_ASSERT_STREQ(nodes[2]->name, "mock_pass");

    aura_pipeline_destroy(p);
    aura_chain_destroy_nodes(nodes, n);
}

static void test_observer_does_not_starve(void)
{
    setup_registry();
    aura_chain_t c;
    aura_node_t *nodes[AURA_CHAIN_MAX_LINKS];
    uint32_t     n = 0;

    aura_pipeline_t *p = aura_pipeline_create(NULL);
    AURA_ASSERT(p != NULL);

    /* pass → {vad(观察者), kws(主线)} → sink
     * 关键断言：观察者不产出音频，但**不能截断主线** —— sink 仍应收到全部帧。 */
    AURA_ASSERT(aura_chain_parse("mock_pass, tee(mock_sink), mock_pass, mock_sink", &c) == AURA_OK);
    AURA_ASSERT_EQ(aura_chain_build(p, &c, nodes, &n), AURA_OK);
    AURA_ASSERT_EQ(n, 4);

    /* 拓扑：nodes[0] 扇出到观察者 nodes[1] 与主线 nodes[2]；nodes[2] → nodes[3] */
    AURA_ASSERT_EQ(nodes[0]->next_audio_count, 2);
    AURA_ASSERT(nodes[0]->next_audio[0] == nodes[1]);
    AURA_ASSERT(nodes[0]->next_audio[1] == nodes[2]);
    AURA_ASSERT_EQ(nodes[2]->next_audio_count, 1);
    AURA_ASSERT(nodes[2]->next_audio[0] == nodes[3]);

    AURA_ASSERT_EQ(aura_pipeline_start(p), AURA_OK);

    int16_t pcm[160];
    for (int i = 0; i < 160; i++) {
        pcm[i] = (int16_t)(i * 10);
    }
    for (int k = 0; k < 5; k++) {
        AURA_ASSERT_EQ(aura_pipeline_feed(p, pcm, 160, 1, AURA_SAMPLE_S16, 0, 200), AURA_OK);
    }
    AURA_ASSERT_EQ(aura_pipeline_wait_drained(p, 2000), AURA_OK);

    /* 主线终点与旁路观察者都收到了 5 帧 */
    AURA_ASSERT_EQ(created(3)->frames_in, 5); /* 主线 sink */
    AURA_ASSERT_EQ(created(1)->frames_in, 5); /* 旁路观察者 */
    AURA_ASSERT_EQ(created(2)->frames_in, 5); /* 主线中继 */

    aura_pipeline_stop(p);
    aura_pipeline_destroy(p);
    aura_chain_destroy_nodes(nodes, n);
}

static void test_changes_frame_rate_pts_not_corrected(void)
{
    setup_registry();
    aura_chain_t c;
    aura_node_t *nodes[AURA_CHAIN_MAX_LINKS];
    uint32_t     n = 0;

    aura_pipeline_t *p = aura_pipeline_create(NULL);
    AURA_ASSERT(p != NULL);

    AURA_ASSERT(aura_chain_parse("mock_decim, mock_sink", &c) == AURA_OK);
    aura_algo_params_set_i32(aura_chain_params(&c, 0), "out_sample_rate", 8000);
    AURA_ASSERT_EQ(aura_chain_build(p, &c, nodes, &n), AURA_OK);
    AURA_ASSERT_EQ(aura_pipeline_start(p), AURA_OK);

    int16_t pcm[160];
    memset(pcm, 0, sizeof(pcm));
    /* 显式 pts=0 走自动推进：首帧 0、次帧 10ms */
    AURA_ASSERT_EQ(aura_pipeline_feed(p, pcm, 160, 1, AURA_SAMPLE_S16, 0, 200), AURA_OK);
    AURA_ASSERT_EQ(aura_pipeline_feed(p, pcm, 160, 1, AURA_SAMPLE_S16, 0, 200), AURA_OK);
    AURA_ASSERT_EQ(aura_pipeline_wait_drained(p, 2000), AURA_OK);

    /* decim 产出时自加 5000us；变帧率节点不被 pts 纠正 → 下游看到的是自算值。
     * （普通节点在这里会被强制改回输入 pts，见 test_pipeline.c 的对应用例。） */
    AURA_ASSERT_EQ(created(1)->frames_in, 2);
    AURA_ASSERT_EQ(created(1)->last_pts, 10000 + 5000);
    /* 帧头采样率也被节点改写，随帧传播到下游 */
    AURA_ASSERT_EQ(created(1)->last_rate, 8000);

    aura_pipeline_stop(p);
    aura_pipeline_destroy(p);
    aura_chain_destroy_nodes(nodes, n);
}

/* 配置 → 链：语法、链首形状、参数类型转换，以及三类"静默失效"的报错路径。
 * 走真实的 .conf 文本（而不是手填结构体）—— 用户手里拿到的就是那个文件。 */
static void test_chain_from_config(void)
{
    setup_registry();

    const char *path = "aura_chain_test.conf";
    FILE       *fp   = fopen(path, "wb");
    AURA_ASSERT(fp != NULL);
    fputs("# 链装配用例\n"
          "sample_rate = 48000\n"
          "channels = 2\n"
          "chain = mock_pass, tee(mock_sink), mock_decim\n"
          "chain_param_mock_pass.gain = 1.5\n"
          "chain_param_mock_pass.delay_ms = 40\n"
          "chain_param_mock_pass.enabled = true\n"
          "chain_param_mock_pass.tag = aggressive\n"
          "chain_param_mock_decim.out_sample_rate = 8000\n",
          fp);
    fclose(fp);

    aura_config_t cfg;
    aura_config_default(&cfg);
    AURA_ASSERT(aura_config_load_file(&cfg, path) == AURA_OK);
    remove(path);

    AURA_ASSERT_STREQ(cfg.chain, "mock_pass, tee(mock_sink), mock_decim");
    AURA_ASSERT_EQ(cfg.chain_param_count, 5);

    aura_chain_t chain;
    AURA_ASSERT(aura_chain_from_config(&cfg, &chain) == AURA_OK);
    AURA_ASSERT_EQ(chain.count, 3);
    AURA_ASSERT_EQ(chain.sample_rate, 48000); /* 链首形状跟着配置走 */
    AURA_ASSERT_EQ(chain.channels, 2);
    AURA_ASSERT(!chain.links[0].observer_hint);
    AURA_ASSERT(chain.links[1].observer_hint); /* tee() 里的那个 */

    /* 值按 param_specs 声明的类型转换，而不是"看着像什么就是什么" */
    const aura_algo_params_t *p0 = &chain.links[0].params;
    AURA_ASSERT_EQ(p0->extra_count, 4);
    AURA_ASSERT_EQ(aura_algo_params_i32(p0, "delay_ms", -1), 40);
    AURA_ASSERT(aura_algo_params_f32(p0, "gain", 0.0f) > 1.49f);
    AURA_ASSERT(aura_algo_params_bool(p0, "enabled", false));
    AURA_ASSERT_STREQ(aura_algo_params_str(p0, "tag", "x"), "aggressive");
    /* 只有声明了规格的算法才收得到参数；没配的节点保持空 */
    AURA_ASSERT_EQ(chain.links[2].params.extra_count, 1);
    AURA_ASSERT_EQ(aura_algo_params_i32(&chain.links[2].params, "out_sample_rate", 0), 8000);

    /* 拼错键名 = 装配期报错（本函数存在的主要理由） */
    aura_config_t bad = cfg;
    snprintf(bad.chain_params[0].key, sizeof(bad.chain_params[0].key), "%s", "mock_pass.gian");
    AURA_ASSERT_EQ(aura_chain_from_config(&bad, &chain), AURA_ERR_INVALID_ARG);

    /* 值不是声明的类型：整段必须被吃掉，"1.5abc" 不算 1.5 */
    bad = cfg;
    snprintf(bad.chain_params[0].val, sizeof(bad.chain_params[0].val), "%s", "1.5abc");
    AURA_ASSERT_EQ(aura_chain_from_config(&bad, &chain), AURA_ERR_INVALID_ARG);

    /* 算法没注册 / 注册了但不在链上 / 连参数表都没有 */
    bad = cfg;
    snprintf(bad.chain_params[0].key, sizeof(bad.chain_params[0].key), "%s", "mock_nope.gain");
    AURA_ASSERT_EQ(aura_chain_from_config(&bad, &chain), AURA_ERR_NOT_FOUND);
    /* 注册了、也在链上，但描述表一个参数都没声明 —— 不是 NOT_FOUND 而是参数错 */
    bad = cfg;
    snprintf(bad.chain_params[0].key, sizeof(bad.chain_params[0].key), "%s", "mock_sink.gain");
    AURA_ASSERT_EQ(aura_chain_from_config(&bad, &chain), AURA_ERR_INVALID_ARG);
    bad = cfg;
    snprintf(bad.chain_params[0].key, sizeof(bad.chain_params[0].key), "%s", "mock_decim.gain");
    AURA_ASSERT_EQ(aura_chain_from_config(&bad, &chain), AURA_ERR_INVALID_ARG);

    /* 键名不合语法 */
    bad = cfg;
    snprintf(bad.chain_params[0].key, sizeof(bad.chain_params[0].key), "%s", "no_dot");
    AURA_ASSERT_EQ(aura_chain_from_config(&bad, &chain), AURA_ERR_INVALID_ARG);

    /* 有参数没链：多半是把 chain 这个键敲错了 */
    bad = cfg;
    bad.chain[0] = '\0';
    AURA_ASSERT_EQ(aura_chain_from_config(&bad, &chain), AURA_ERR_INVALID_ARG);

    /* 空链不是错误：就是"这份构建手工装节点" */
    aura_config_t empty;
    aura_config_default(&empty);
    AURA_ASSERT(aura_chain_from_config(&empty, &chain) == AURA_OK);
    AURA_ASSERT_EQ(chain.count, 0);

    /* 链语法错误 */
    snprintf(empty.chain, sizeof(empty.chain), "%s", "mock_pass,,mock_sink");
    AURA_ASSERT_EQ(aura_chain_from_config(&empty, &chain), AURA_ERR_INVALID_ARG);

    /* 算法名超长（>= AURA_CHAIN_NAME_MAX）：截断会变成另一个合法的名字 */
    bad = cfg;
    memset(bad.chain_params[0].key, 'x', 40);
    snprintf(bad.chain_params[0].key + 40, 8, "%s", ".gain");
    AURA_ASSERT_EQ(aura_chain_from_config(&bad, &chain), AURA_ERR_INVALID_ARG);
}

/* agent 层的链装配：配置里的 chain 能在 init 时变成真节点。
 * 放在本文件是因为只有这里有 mock 描述表（agent 层不认识具体算法，
 * 所以它自己也没有可测的链）。 */
static void test_agent_chain_assembly(void)
{
    setup_registry();

    aura_agent_config_t acfg;
    aura_agent_config_default(&acfg);
    acfg.log_level = AURA_LOG_LVL_ERROR; /* 免得刷屏 */
    acfg.chain     = "mock_pass, tee(mock_sink)";

    /* 未注册的算法名：init 直接失败，且 chain_build 已把建出的第一个节点回滚掉 */
    acfg.chain = "mock_pass, mock_nope";
    AURA_ASSERT_EQ(aura_agent_init(&acfg), AURA_ERR_NOT_FOUND);
    AURA_ASSERT_EQ(g_created_count, 1); /* 只造到 mock_pass 就失败了 */

    mock_reset();
    acfg.chain = "mock_pass, tee(mock_sink)";
    AURA_ASSERT_EQ(aura_agent_init(&acfg), AURA_OK);
    AURA_ASSERT_EQ(g_created_count, 2); /* 主线 + tee 成员都造出来了 */
    AURA_ASSERT_STREQ(g_created[0]->namebuf, "mock_pass");
    AURA_ASSERT_STREQ(g_created[1]->namebuf, "mock_sink");
    AURA_ASSERT_EQ(aura_agent_deinit(), AURA_OK);

    /* 再 init 一次且不带链：不能把上一次的链"复活" */
    mock_reset();
    acfg.chain = NULL;
    AURA_ASSERT_EQ(aura_agent_init(&acfg), AURA_OK);
    AURA_ASSERT_EQ(g_created_count, 0);
    AURA_ASSERT_EQ(aura_agent_deinit(), AURA_OK);
}

AURA_TEST(test_registry);
AURA_TEST(test_params);
AURA_TEST(test_chain_from_config);
AURA_TEST(test_agent_chain_assembly);
AURA_TEST(test_chain_parse);
AURA_TEST(test_chain_build_unknown);
AURA_TEST(test_shape_propagation);
AURA_TEST(test_observer_does_not_starve);
AURA_TEST(test_changes_frame_rate_pts_not_corrected);

AURA_TEST_MAIN();
