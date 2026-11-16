/* Aura — Phase C1 播放链单测
 *
 * 覆盖：playback sink 打开/写帧、pts/采样率、pipeline feed、flush 停止。
 * 不依赖 soundproof/MNN：用测试内注册的 pass 节点作为音频源。
 */
#include "aura_test.h"

#include "core/algorithm.h"
#include "core/node.h"
#include "core/pipeline/pipeline.h"
#include "core/playback/playback.h"

static int32_t g_pass_create_count;

typedef struct {
    aura_node_t base;
} pass_node_t;

static aura_err_t pass_process(aura_node_t *self, const aura_audio_frame_t *frame)
{
    return aura_node_emit_audio(self, frame);
}

static const aura_node_ops_t kPassOps = {.process_audio = pass_process};

static aura_node_t *pass_create(const aura_algo_params_t *p, aura_err_t *err)
{
    (void)p;
    pass_node_t *n = (pass_node_t *)calloc(1, sizeof(*n));
    if (n == NULL) {
        if (err != NULL) {
            *err = AURA_ERR_NOMEM;
        }
        return NULL;
    }
    aura_node_caps_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.consumes_audio  = true;
    caps.produces_audio  = true;
    aura_node_init(&n->base, "pass", &kPassOps, &caps);
    g_pass_create_count++;
    return &n->base;
}

static const aura_algo_desc_t kPassDesc = {
    .name    = "pass",
    .version = 1,
    .kind    = AURA_ALGO_KIND_DSP,
    .io      = {.consumes_audio = true, .produces_audio = true},
    .create  = pass_create,
};

/* ------------------------------------------------------------ 记录后端 */

typedef struct {
    uint32_t opens;
    uint32_t closes;
    uint32_t writes;
    uint32_t flushes;
    uint32_t rate;
    uint32_t channels;
    uint64_t last_pts;
    uint64_t flush_us;
} rec_backend_t;

static rec_backend_t g_rec;
static aura_mutex_t *g_rec_lock;

static aura_err_t rec_open(void *user, uint32_t sample_rate, uint32_t channels)
{
    (void)user;
    aura_osal_mutex_lock(g_rec_lock);
    g_rec.opens++;
    g_rec.rate     = sample_rate;
    g_rec.channels = channels;
    aura_osal_mutex_unlock(g_rec_lock);
    return AURA_OK;
}

static aura_err_t rec_write(void *user, const int16_t *pcm, uint32_t frame_count,
                            uint32_t channels, uint64_t pts_us)
{
    (void)user;
    (void)pcm;
    (void)frame_count;
    (void)channels;
    aura_osal_mutex_lock(g_rec_lock);
    g_rec.writes++;
    g_rec.last_pts = pts_us;
    aura_osal_mutex_unlock(g_rec_lock);
    return AURA_OK;
}

static void rec_flush(void *user)
{
    (void)user;
    aura_osal_mutex_lock(g_rec_lock);
    g_rec.flushes++;
    g_rec.flush_us = aura_osal_time_us();
    aura_osal_mutex_unlock(g_rec_lock);
}

static void rec_close(void *user)
{
    (void)user;
    aura_osal_mutex_lock(g_rec_lock);
    g_rec.closes++;
    aura_osal_mutex_unlock(g_rec_lock);
}

static uint32_t rec_get(const uint32_t *field)
{
    aura_osal_mutex_lock(g_rec_lock);
    uint32_t v = *field;
    aura_osal_mutex_unlock(g_rec_lock);
    return v;
}

static uint64_t rec_get_u64(const uint64_t *field)
{
    aura_osal_mutex_lock(g_rec_lock);
    uint64_t v = *field;
    aura_osal_mutex_unlock(g_rec_lock);
    return v;
}

static bool wait_writes(uint32_t n, int timeout_ms)
{
    for (int waited = 0; waited < timeout_ms; waited += 10) {
        if (rec_get(&g_rec.writes) >= n) {
            return true;
        }
        aura_test_sleep_ms(10);
    }
    return rec_get(&g_rec.writes) >= n;
}

AURA_TEST(test_playback_sink_and_flush)
{
    memset(&g_rec, 0, sizeof(g_rec));
    g_rec_lock = aura_osal_mutex_create();
    AURA_ASSERT(g_rec_lock != NULL);

    aura_playback_backend_t backend;
    memset(&backend, 0, sizeof(backend));
    backend.name  = "rec";
    backend.open  = rec_open;
    backend.write = rec_write;
    backend.flush = rec_flush;
    backend.close = rec_close;
    aura_playback_set_backend(&backend);

    AURA_ASSERT_EQ(aura_playback_register(), AURA_OK);
    aura_err_t rc = aura_algo_register(&kPassDesc);
    AURA_ASSERT(rc == AURA_OK || rc == AURA_ERR_EXIST);

    aura_pipeline_cfg_t pc;
    aura_pipeline_config_default(&pc);
    pc.sample_rate       = 16000;
    pc.audio_queue_depth = 8;
    pc.frame_pool_blocks = 32;
    aura_pipeline_t *p   = aura_pipeline_create(&pc);
    AURA_ASSERT(p != NULL);

    aura_chain_t chain;
    aura_chain_init(&chain);
    AURA_ASSERT_EQ(aura_chain_add(&chain, "pass", NULL), AURA_OK);
    AURA_ASSERT_EQ(aura_chain_add(&chain, "playback", NULL), AURA_OK);
    chain.sample_rate = 16000;
    chain.frame_ms    = 10;
    chain.channels    = 1;

    aura_node_t *nodes[AURA_CHAIN_MAX_LINKS];
    uint32_t     count = 0;
    AURA_ASSERT_EQ(aura_chain_build(p, &chain, nodes, &count), AURA_OK);
    AURA_ASSERT_EQ(count, 2);
    AURA_ASSERT_EQ(aura_pipeline_start(p), AURA_OK);

    /* 喂 10 帧 10ms 静音 */
    int16_t frame[160];
    memset(frame, 0, sizeof(frame));
    for (int i = 0; i < 10; i++) {
        AURA_ASSERT_EQ(aura_pipeline_feed(p, frame, 160, 1, AURA_SAMPLE_S16, 0, 1000),
                       AURA_OK);
    }
    AURA_ASSERT(wait_writes(10, 2000));
    AURA_ASSERT_EQ(rec_get(&g_rec.opens), 1);
    AURA_ASSERT_EQ(rec_get(&g_rec.rate), 16000);
    AURA_ASSERT_EQ(rec_get(&g_rec.channels), 1);
    AURA_ASSERT(rec_get_u64(&g_rec.last_pts) >= 80000);

    /* flush：停止回调必须同步发生，且之后不再有新写入 */
    uint64_t t0 = aura_osal_time_us();
    AURA_ASSERT_EQ(aura_pipeline_flush(p), AURA_OK);
    uint64_t elapsed = aura_osal_time_us() - t0;
    AURA_ASSERT_EQ(rec_get(&g_rec.flushes), 1);
    AURA_ASSERT(elapsed < 200000);

    uint32_t before = rec_get(&g_rec.writes);
    aura_test_sleep_ms(150);
    AURA_ASSERT_EQ(rec_get(&g_rec.writes), before);

    AURA_ASSERT_EQ(aura_pipeline_stop(p), AURA_OK);
    aura_pipeline_destroy(p);
    aura_chain_destroy_nodes(nodes, count);
    AURA_ASSERT_EQ(rec_get(&g_rec.closes), 1);

    aura_osal_mutex_destroy(g_rec_lock);
    g_rec_lock = NULL;
}

AURA_TEST_MAIN()
