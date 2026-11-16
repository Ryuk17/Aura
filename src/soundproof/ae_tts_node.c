/* Aura — soundproof TTS → pipeline 节点
 *
 * 形态：文本源（consumes_text=true, produces_audio=true）。
 *   process_text ：把文本放进节点自有队列（不阻塞）
 *   worker       ：AudioEngine_Tts_Process（同步整段）→ 10ms 分帧 → emit_audio
 *   节奏         ：按 pts 实时 pacing，避免一次生成整段把下游队列灌爆
 *   flush        ：generation++ + 清文本队列；在途结果直接丢弃（引擎无 abort）
 *
 * 注意：AudioEngine_Tts_Process 的 PCM 由引擎持有，只在下次 Process/Reset/Deinit
 * 前有效；本节点在 pacing 期间不调用这些接口。
 */
#include "soundproof/ae_register.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_engine_tts.h"
#include "osal/osal.h"
#include "soundproof/ae_common.h"
#include "utils/logger/logger.h"

#define TAG "ae_tts"

#define AE_TTS_TEXT_QUEUE_DEPTH 8
#define AE_TTS_TAIL_MAX 1024 /* 支持到 96kHz 的 10ms 尾帧 */

typedef struct ae_tts_text_item {
    char     text[AURA_TEXT_CHUNK_MAX_BYTES];
    uint64_t pts_us;
    uint32_t generation;
} ae_tts_text_item_t;

typedef struct ae_tts_node {
    aura_node_t base;

    TtsHandle h;
    bool      inited;
    bool      started;

    uint32_t out_rate;
    uint32_t frame_len;
    int32_t  num_threads;
    float    speed;
    int32_t  speaker_id;

    char model_type[32];
    char model_path[AUDIO_ENGINE_MAX_FILE_PATH_LEN];
    char voices_path[AUDIO_ENGINE_MAX_FILE_PATH_LEN];
    char tokens_path[AUDIO_ENGINE_MAX_FILE_PATH_LEN];
    char lexicon_path[AUDIO_ENGINE_MAX_FILE_PATH_LEN];
    char data_dir[AUDIO_ENGINE_MAX_FILE_PATH_LEN];
    char dict_dir[AUDIO_ENGINE_MAX_FILE_PATH_LEN];

    aura_queue_t *text_q;
    aura_task_t  *worker;
    aura_mutex_t *engine_lock;
    volatile bool running;

    _Atomic uint32_t generation;
    _Atomic bool     need_reset;
    _Atomic uint64_t flush_time_us;
    uint64_t         dropped_texts;
} ae_tts_node_t;

static const aura_node_ops_t k_ae_tts_ops;

static void copy_path_param(const aura_algo_params_t *p, const char *name, const char *def,
                            char *dst, size_t cap)
{
    const char *raw = ae_param_str(p, name, def);
    if (raw == NULL || raw[0] == '\0') {
        dst[0] = '\0';
        return;
    }
    if (ae_resolve_path(raw, dst, cap) == NULL) {
        dst[0] = '\0';
    }
}

static aura_node_t *ae_tts_create_node(const aura_algo_params_t *p, aura_err_t *err)
{
    if (p == NULL) {
        if (err != NULL) {
            *err = AURA_ERR_INVALID_ARG;
        }
        return NULL;
    }
    ae_tts_node_t *n = (ae_tts_node_t *)calloc(1, sizeof(*n));
    if (n == NULL) {
        if (err != NULL) {
            *err = AURA_ERR_NOMEM;
        }
        return NULL;
    }

    n->out_rate = (uint32_t)ae_param_i32(p, "output_sample_rate", (int32_t)p->sample_rate);
    if (n->out_rate == 0) {
        n->out_rate = p->sample_rate;
    }
    n->frame_len   = n->out_rate / 100;
    n->num_threads = ae_param_i32(p, "num_threads", 1);
    n->speed       = ae_param_f32(p, "speed", 1.0f);
    n->speaker_id  = ae_param_i32(p, "speaker_id", 0);

    snprintf(n->model_type, sizeof(n->model_type), "%.*s", (int)(sizeof(n->model_type) - 1),
             ae_param_str(p, "model_type", "kokoro"));
    copy_path_param(p, "model_path", "tts/kokoro-v1_0.mnn/tts.mnn", n->model_path,
                    sizeof(n->model_path));
    copy_path_param(p, "voices_path", "tts/kokoro-v1_0.mnn/voices.mnn", n->voices_path,
                    sizeof(n->voices_path));
    copy_path_param(p, "tokens_path", "tts/kokoro-v1_0.mnn/tokenizer.txt", n->tokens_path,
                    sizeof(n->tokens_path));
    copy_path_param(p, "lexicon_path", "", n->lexicon_path, sizeof(n->lexicon_path));
    copy_path_param(p, "data_dir", "", n->data_dir, sizeof(n->data_dir));
    copy_path_param(p, "dict_dir", "", n->dict_dir, sizeof(n->dict_dir));

    n->engine_lock = aura_osal_mutex_create();
    if (n->engine_lock == NULL) {
        free(n);
        if (err != NULL) {
            *err = AURA_ERR_NOMEM;
        }
        return NULL;
    }
    n->text_q = aura_osal_queue_create(AE_TTS_TEXT_QUEUE_DEPTH, sizeof(ae_tts_text_item_t));
    if (n->text_q == NULL) {
        aura_osal_mutex_destroy(n->engine_lock);
        free(n);
        if (err != NULL) {
            *err = AURA_ERR_NOMEM;
        }
        return NULL;
    }

    aura_node_caps_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.consumes_text  = true;
    caps.produces_audio = true;
    aura_node_init(&n->base, (p->instance_name != NULL) ? p->instance_name : "tts",
                   &k_ae_tts_ops, &caps);
    return &n->base;
}

static aura_err_t ae_tts_init(aura_node_t *self)
{
    ae_tts_node_t *n = (ae_tts_node_t *)self;

    if (n->h == NULL) {
        n->h = AudioEngine_Tts_Create();
        if (n->h == NULL) {
            return AURA_ERR_NOMEM;
        }
    }

    TtsInitConfig c;
    memset(&c, 0, sizeof(c));
    c.num_threads         = n->num_threads;
    c.output_sample_rate  = (int)n->out_rate;
    snprintf(c.model_type, sizeof(c.model_type), "%s", n->model_type);
    snprintf(c.model_path, sizeof(c.model_path), "%s", n->model_path);
    snprintf(c.voices_path, sizeof(c.voices_path), "%s", n->voices_path);
    snprintf(c.tokens_path, sizeof(c.tokens_path), "%s", n->tokens_path);
    snprintf(c.lexicon_path, sizeof(c.lexicon_path), "%s", n->lexicon_path);
    snprintf(c.data_dir, sizeof(c.data_dir), "%s", n->data_dir);
    snprintf(c.dict_dir, sizeof(c.dict_dir), "%s", n->dict_dir);

    int st = AudioEngine_Tts_Init(n->h, &c);
    if (st != AUDIO_ENGINE_SUCCESS) {
        AURA_LOGE(TAG, "%s: TTS init failed (0x%x), model=%s", self->name, st, n->model_path);
        (void)AudioEngine_Tts_Deinit(n->h);
        (void)AudioEngine_Tts_Destroy(n->h);
        n->h = NULL;
        return ae_status_to_err(st, self->name);
    }

    TtsRtConfig rt;
    rt.speed      = n->speed;
    rt.speaker_id = n->speaker_id;
    (void)AudioEngine_Tts_SetParam(n->h, &rt);

    int sample_rate = 0;
    if (AudioEngine_Tts_GetSampleRate(n->h, &sample_rate) == AUDIO_ENGINE_SUCCESS &&
        sample_rate > 0) {
        if ((uint32_t)sample_rate != n->out_rate) {
            AURA_LOGE(TAG, "%s: engine rate %d != playback chain rate %u — set "
                         "playback_sample_rate or tts.output_sample_rate",
                      self->name, sample_rate, n->out_rate);
            (void)AudioEngine_Tts_Deinit(n->h);
            (void)AudioEngine_Tts_Destroy(n->h);
            n->h = NULL;
            return AURA_ERR_INVALID_ARG;
        }
    }

    n->inited = true;
    AURA_LOGI(TAG, "%s: TTS ready @ %u Hz (model=%s)", self->name, n->out_rate, n->model_path);
    return AURA_OK;
}

static bool gen_matches(ae_tts_node_t *n, uint32_t gen)
{
    return atomic_load(&n->generation) == gen;
}

static void pace_until(ae_tts_node_t *n, uint64_t target_us, uint32_t gen)
{
    while (n->running && gen_matches(n, gen)) {
        uint64_t now = aura_osal_time_us();
        if (now >= target_us) {
            return;
        }
        uint64_t remain_ms = (target_us - now + 999) / 1000;
        aura_osal_sleep_ms((remain_ms > 2) ? 2 : 1);
    }
}

static void tts_worker(void *arg)
{
    ae_tts_node_t  *n = (ae_tts_node_t *)arg;
    ae_tts_text_item_t item;

    while (n->running) {
        if (aura_osal_queue_pop(n->text_q, &item, 100) != AURA_OK) {
            continue;
        }
        if (!gen_matches(n, item.generation)) {
            continue;
        }

        if (atomic_exchange(&n->need_reset, false)) {
            aura_osal_mutex_lock(n->engine_lock);
            (void)AudioEngine_Tts_Reset(n->h);
            aura_osal_mutex_unlock(n->engine_lock);
        }

        TtsInput_t in;
        memset(&in, 0, sizeof(in));
        in.text       = item.text;
        in.input_type = 0; /* plain text（G2P 由 soundproof 内部处理） */

        TtsResult_t res;
        memset(&res, 0, sizeof(res));
        aura_osal_mutex_lock(n->engine_lock);
        int st = AudioEngine_Tts_Process(n->h, &in, &res);
        aura_osal_mutex_unlock(n->engine_lock);

        if (st != AUDIO_ENGINE_SUCCESS) {
            aura_node_report_error(&n->base, ae_status_to_err(st, n->base.name),
                                   "AudioEngine_Tts_Process");
            continue;
        }
        if (!gen_matches(n, item.generation) || !n->running) {
            continue; /* 推理期间被 flush/stop：丢掉整段 */
        }
        if (res.audio_out == NULL || res.out_samples <= 0) {
            continue;
        }

        uint32_t rate      = (res.sample_rate > 0) ? (uint32_t)res.sample_rate : n->out_rate;
        uint32_t frame_len = rate / 100;
        if (frame_len == 0) {
            frame_len = n->frame_len;
        }
        if (frame_len > AE_TTS_TAIL_MAX) {
            frame_len = AE_TTS_TAIL_MAX;
        }

        uint64_t pts   = (item.pts_us != 0) ? item.pts_us : aura_osal_time_us();
        bool     first = true;
        int      offset = 0;
        int16_t  tail[AE_TTS_TAIL_MAX];

        while (offset < res.out_samples && n->running && gen_matches(n, item.generation)) {
            int remain = res.out_samples - offset;
            int count  = (remain < (int)frame_len) ? remain : (int)frame_len;
            const int16_t *data = res.audio_out + offset;
            int emit_count      = count;

            if (count < (int)frame_len) {
                memset(tail, 0, sizeof(int16_t) * frame_len);
                memcpy(tail, data, sizeof(int16_t) * (size_t)count);
                data       = tail;
                emit_count = (int)frame_len;
            }

            if (first) {
                (void)aura_node_post_event(&n->base, AURA_EVENT_TTS_FIRST_CHUNK, 0);
                first = false;
            }
            (void)ae_emit_audio(&n->base, data, (uint32_t)emit_count, rate, 1);

            uint64_t dur_us = (uint64_t)emit_count * 1000000ull / rate;
            pts += dur_us;
            offset += count;
            pace_until(n, pts, item.generation);
        }

        if (!first) {
            (void)aura_node_post_event(&n->base, AURA_EVENT_TTS_DONE, 0);
        }
    }
}

static aura_err_t ae_tts_process_text(aura_node_t *self, const aura_text_chunk_t *chunk)
{
    ae_tts_node_t *n = (ae_tts_node_t *)self;
    if (!n->inited || chunk == NULL) {
        return AURA_ERR_STATE;
    }

    /* 在 flush 之前喂入、但在 flush 之后才轮到 process_text 的文本必须丢弃：
     * 典型是 barge-in 瞬间 pipeline task 已经 pop 出队、还没进本函数。 */
    uint64_t flush_us = atomic_load(&n->flush_time_us);
    if (chunk->pts_us != 0 && chunk->pts_us < flush_us) {
        return AURA_OK;
    }

    ae_tts_text_item_t item;
    memset(&item, 0, sizeof(item));
    snprintf(item.text, sizeof(item.text), "%.*s", (int)(sizeof(item.text) - 1), chunk->text);
    item.pts_us     = chunk->pts_us;
    item.generation = atomic_load(&n->generation);

    if (aura_osal_queue_push(n->text_q, &item, AURA_NO_WAIT) != AURA_OK) {
        n->dropped_texts++;
        AURA_LOGW(TAG, "%s: text queue full, dropping '%s'", self->name, item.text);
        return AURA_OK; /* 丢文本不阻塞链路；不要把它升级成节点错误 */
    }
    return AURA_OK;
}

static aura_err_t ae_tts_start(aura_node_t *self)
{
    ae_tts_node_t *n = (ae_tts_node_t *)self;
    if (!n->inited) {
        return AURA_ERR_STATE;
    }
    if (n->worker != NULL) {
        return AURA_OK;
    }
    n->running = true;
    n->worker  = aura_osal_task_create(self->name, 16384, 2, tts_worker, n);
    if (n->worker == NULL) {
        n->running = false;
        return AURA_ERR_NOMEM;
    }
    n->started = true;
    return AURA_OK;
}

static aura_err_t ae_tts_stop(aura_node_t *self)
{
    ae_tts_node_t *n = (ae_tts_node_t *)self;
    if (n->worker != NULL) {
        n->running = false;
        (void)aura_osal_task_join(n->worker, AURA_WAIT_FOREVER);
        aura_osal_task_destroy(n->worker);
        n->worker = NULL;
    }
    n->started = false;
    return AURA_OK;
}

static aura_err_t ae_tts_control(aura_node_t *self, aura_node_cmd_t cmd, const void *arg)
{
    (void)arg;
    ae_tts_node_t *n = (ae_tts_node_t *)self;
    if (cmd == AURA_CMD_FLUSH || cmd == AURA_CMD_RESET) {
        /* 不碰引擎：Process 可能正在跑，锁住会把 flush 线程拖过 200ms。
         * generation 让 worker 丢弃在途/后续产出，engine reset 延后到安全点。 */
        atomic_store(&n->flush_time_us, aura_osal_time_us());
        atomic_fetch_add(&n->generation, 1);
        if (n->text_q != NULL) {
            aura_osal_queue_reset(n->text_q);
        }
        atomic_store(&n->need_reset, true);
    }
    return AURA_OK;
}

static void ae_tts_deinit(aura_node_t *self)
{
    ae_tts_node_t *n = (ae_tts_node_t *)self;
    (void)ae_tts_stop(self);
    if (n->h != NULL) {
        (void)AudioEngine_Tts_Deinit(n->h);
        (void)AudioEngine_Tts_Destroy(n->h);
        n->h = NULL;
    }
    n->inited = false;
    if (n->text_q != NULL) {
        aura_osal_queue_destroy(n->text_q);
        n->text_q = NULL;
    }
    if (n->engine_lock != NULL) {
        aura_osal_mutex_destroy(n->engine_lock);
        n->engine_lock = NULL;
    }
}

static aura_err_t ae_tts_selftest(aura_node_t *self)
{
    ae_tts_node_t *n = (ae_tts_node_t *)self;
    return (n->h != NULL) ? AURA_OK : AURA_ERR_STATE;
}

static const aura_node_ops_t k_ae_tts_ops = {
    .init          = ae_tts_init,
    .start         = ae_tts_start,
    .stop          = ae_tts_stop,
    .process_text  = ae_tts_process_text,
    .control       = ae_tts_control,
    .deinit        = ae_tts_deinit,
    .selftest      = ae_tts_selftest,
};

/* ------------------------------------------------------------ 描述表 */

static const aura_algo_param_spec_t k_tts_specs[] = {
    {"num_threads", AURA_ALGO_PARAM_I32, 1, 16, "MNN threads"},
    {"output_sample_rate", AURA_ALGO_PARAM_I32, 0, 96000, "engine output rate; 0=chain rate"},
    {"model_type", AURA_ALGO_PARAM_STR, 0, 0, "kokoro/vits"},
    {"model_path", AURA_ALGO_PARAM_STR, 0, 0, "relative to model_dir"},
    {"voices_path", AURA_ALGO_PARAM_STR, 0, 0, "Kokoro voices"},
    {"tokens_path", AURA_ALGO_PARAM_STR, 0, 0, "tokenizer"},
    {"lexicon_path", AURA_ALGO_PARAM_STR, 0, 0, "reserved G2P"},
    {"data_dir", AURA_ALGO_PARAM_STR, 0, 0, "reserved G2P"},
    {"dict_dir", AURA_ALGO_PARAM_STR, 0, 0, "reserved G2P"},
    {"speed", AURA_ALGO_PARAM_F32, 0.1f, 3.0f, "speech speed"},
    {"speaker_id", AURA_ALGO_PARAM_I32, 0, 1000, "speaker id"},
};

static const aura_algo_desc_t k_tts_desc = {
    .name    = "tts",
    .version = 1,
    .kind    = AURA_ALGO_KIND_NN_ASYNC,
    .io      = {.consumes_text = true, .produces_audio = true},
    .param_specs      = k_tts_specs,
    .param_spec_count = sizeof(k_tts_specs) / sizeof(k_tts_specs[0]),
    .create           = ae_tts_create_node,
};

aura_err_t ae_tts_register_all(void)
{
    aura_err_t rc = aura_algo_register(&k_tts_desc);
    return (rc == AURA_ERR_EXIST) ? AURA_OK : rc;
}
