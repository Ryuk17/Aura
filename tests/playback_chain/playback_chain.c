/* Aura — playback_chain：Phase C 播放链闭环仿真
 *
 * 真实 agent（event_bus → bridge → stage）驱动：
 *   ASR_FINAL → dialogue → tool → orchestrator → tts.say
 *   → tts_egress → pipeline.feed_text → tts_mock → playback sink
 *
 * tts_mock 是测试内注册的音频源节点（10ms 分帧、实时 pacing），用来验证：
 *   - playback_chain 配置/装配；
 *   - tts.say 到播放 sink 的闭环；
 *   - barge-in 时 sink.flush 立即触发，未播帧不再写出（< 200ms）。
 */
#include "aura_test.h"

#include <stdatomic.h>

#include "agent/agent_core.h"
#include "agent/agent_stages.h"
#include "agent_export.h"
#include "core/algorithm.h"

/* ------------------------------------------------------------ mock TTS */

#define MOCK_TTS_FRAMES 40 /* 400ms @10ms */

typedef struct {
    aura_node_t      base;
    uint32_t         rate;
    _Atomic uint32_t generation;
    _Atomic uint64_t flush_time_us;
} mock_tts_t;

static aura_err_t mock_tts_process_text(aura_node_t *self, const aura_text_chunk_t *chunk)
{
    mock_tts_t *n   = (mock_tts_t *)self;
    uint32_t    gen = atomic_load(&n->generation);

    /* 与真实 TTS 适配层一致：flush 之前喂入、flush 之后才执行的文本丢弃。 */
    if (chunk->pts_us != 0 && chunk->pts_us < atomic_load(&n->flush_time_us)) {
        return AURA_OK;
    }

    uint32_t frame_len = n->rate / 100;
    if (frame_len == 0 || frame_len > 480) {
        return AURA_ERR_INVALID_ARG;
    }
    int16_t tone[480];
    for (uint32_t i = 0; i < frame_len; i++) {
        tone[i] = (int16_t)((i % 20) * 100 - 1000);
    }

    uint64_t pts = (chunk->pts_us != 0) ? chunk->pts_us : aura_osal_time_us();
    for (int i = 0; i < MOCK_TTS_FRAMES; i++) {
        if (atomic_load(&n->generation) != gen) {
            break;
        }
        aura_audio_frame_t f;
        memset(&f, 0, sizeof(f));
        f.data        = (uint8_t *)tone;
        f.data_bytes  = frame_len * (uint32_t)sizeof(int16_t);
        f.frame_count = (uint16_t)frame_len;
        f.sample_rate = n->rate;
        f.channels    = 1;
        f.fmt         = AURA_SAMPLE_S16;
        f.pts_us      = pts;
        (void)aura_node_emit_audio(self, &f);
        pts += 10000;
        aura_osal_sleep_ms(10);
    }
    return AURA_OK;
}

static aura_err_t mock_tts_control(aura_node_t *self, aura_node_cmd_t cmd, const void *arg)
{
    (void)arg;
    mock_tts_t *n = (mock_tts_t *)self;
    if (cmd == AURA_CMD_FLUSH || cmd == AURA_CMD_RESET) {
        atomic_store(&n->flush_time_us, aura_osal_time_us());
        atomic_fetch_add(&n->generation, 1);
    }
    return AURA_OK;
}

static const aura_node_ops_t kMockTtsOps = {
    .process_text = mock_tts_process_text,
    .control      = mock_tts_control,
};

static aura_node_t *mock_tts_create(const aura_algo_params_t *p, aura_err_t *err)
{
    mock_tts_t *n = (mock_tts_t *)calloc(1, sizeof(*n));
    if (n == NULL) {
        if (err != NULL) {
            *err = AURA_ERR_NOMEM;
        }
        return NULL;
    }
    n->rate = (p != NULL) ? p->sample_rate : 16000;
    atomic_init(&n->generation, 0);
    atomic_init(&n->flush_time_us, 0);
    aura_node_caps_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.consumes_text  = true;
    caps.produces_audio = true;
    aura_node_init(&n->base, (p != NULL && p->instance_name) ? p->instance_name : "tts_mock",
                   &kMockTtsOps, &caps);
    return &n->base;
}

static const aura_algo_desc_t kMockTtsDesc = {
    .name    = "tts_mock",
    .version = 1,
    .kind    = AURA_ALGO_KIND_NN_ASYNC,
    .io      = {.consumes_text = true, .produces_audio = true},
    .create  = mock_tts_create,
};

/* ------------------------------------------------------------ 记录后端 */

typedef struct {
    uint32_t opens;
    uint32_t writes;
    uint32_t flushes;
    uint32_t rate;
    uint32_t channels;
    uint64_t last_write_us;
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
    (void)pts_us;
    aura_osal_mutex_lock(g_rec_lock);
    g_rec.writes++;
    g_rec.last_write_us = aura_osal_time_us();
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
}

static uint32_t rec_writes(void)
{
    aura_osal_mutex_lock(g_rec_lock);
    uint32_t v = g_rec.writes;
    aura_osal_mutex_unlock(g_rec_lock);
    return v;
}

static bool wait_writes(uint32_t n, int timeout_ms)
{
    for (int waited = 0; waited < timeout_ms; waited += 10) {
        if (rec_writes() >= n) {
            return true;
        }
        aura_test_sleep_ms(10);
    }
    return rec_writes() >= n;
}

static void post_voiceprint(aura_event_bus_t *bus, bool passed, int32_t score)
{
    aura_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type                = AURA_EVENT_VOICEPRINT_RESULT;
    ev.u.voiceprint.passed = passed;
    ev.u.voiceprint.score  = score;
    (void)aura_event_bus_publish(bus, &ev);
}

AURA_TEST(test_playback_chain_and_barge_in)
{
    memset(&g_rec, 0, sizeof(g_rec));
    g_rec_lock = aura_osal_mutex_create();
    AURA_ASSERT(g_rec_lock != NULL);

    aura_err_t reg = aura_algo_register(&kMockTtsDesc);
    AURA_ASSERT(reg == AURA_OK || reg == AURA_ERR_EXIST);

    aura_playback_backend_t backend;
    memset(&backend, 0, sizeof(backend));
    backend.name  = "rec";
    backend.open  = rec_open;
    backend.write = rec_write;
    backend.flush = rec_flush;
    backend.close = rec_close;
    AURA_ASSERT_EQ(aura_agent_set_playback_backend(&backend), AURA_OK);

    aura_agent_config_t cfg;
    aura_agent_config_default(&cfg);
    cfg.playback_chain       = "tts_mock, playback";
    cfg.playback_sample_rate = 16000;
    AURA_ASSERT_EQ(aura_agent_init(&cfg), AURA_OK);
    AURA_ASSERT_EQ(aura_agent_start(), AURA_OK);

    aura_event_bus_t *bus = aura_agent_bus();
    AURA_ASSERT(bus != NULL);

    /* wake → voiceprint → vad → asr.final → dialogue/tool/orchestrator → tts.say */
    AURA_ASSERT_EQ(aura_event_bus_post(bus, AURA_EVENT_KWS_HIT, 0, NULL), AURA_OK);
    aura_test_sleep_ms(80);
    post_voiceprint(bus, true, 90);
    AURA_ASSERT_EQ(aura_event_bus_post(bus, AURA_EVENT_VAD_SPEECH_START, 0, NULL), AURA_OK);
    aura_test_sleep_ms(80);
    AURA_ASSERT_EQ(aura_event_bus_post(bus, AURA_EVENT_ASR_FINAL, 0, "今天天气怎么样"),
                   AURA_OK);

    /* tts.say 经 tts_egress 注入 TTS 节点，播放 sink 应有 PCM */
    AURA_ASSERT(wait_writes(5, 3000));
    AURA_ASSERT_EQ(g_rec.opens, 1);
    AURA_ASSERT_EQ(g_rec.rate, 16000);
    AURA_ASSERT_EQ(g_rec.channels, 1);

    /* barge-in：flush 必须立即触发，之后不再有新帧写出 */
    uint64_t interrupt_us = aura_osal_time_us();
    AURA_ASSERT_EQ(aura_agent_interrupt(), AURA_OK);
    AURA_ASSERT_EQ(g_rec.flushes, 1);
    AURA_ASSERT(g_rec.flush_us - interrupt_us < 200000);

    /* 允许最多一帧在途；之后 120ms 内不应再有新写入。 */
    aura_test_sleep_ms(120);
    uint32_t after = rec_writes();
    aura_test_sleep_ms(120);
    AURA_ASSERT_EQ(rec_writes(), after);

    AURA_ASSERT_EQ(aura_agent_stop(), AURA_OK);
    AURA_ASSERT_EQ(aura_agent_deinit(), AURA_OK);
    aura_osal_mutex_destroy(g_rec_lock);
}

AURA_TEST_MAIN()
