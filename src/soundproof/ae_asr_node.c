/* Aura — soundproof ASR → pipeline 节点（异步 worker）
 *
 * ASR 的 Process 是流式、同步、可能慢于 10ms 的调用，不能阻塞音频节点 task：
 *   node task：process_audio 只把 10ms PCM 入队；
 *   worker   ：按序调用 AudioEngine_Asr_Process，结果放 result_q 并发
 *              AURA_EVENT_NN_RESULT（payload = 节点名）；
 *   on_event ：在 event_bus 任务里认领结果，emit_text + ASR_PARTIAL/FINAL 事件。
 *
 * 状态门控：只有状态机进入 Listening 后才消费音频；离开 Listening 时向 worker
 * 投复位命令，清空引擎状态，避免下一轮把上一轮尾巴识别进来。
 */
#include "soundproof/ae_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_engine_asr.h"
#include "core/state_machine/state_machine.h"
#include "utils/logger/logger.h"

#define TAG "ae_asr"

#define AE_ASR_JOB_QUEUE_DEPTH 512 /* 5s @ 10ms */
#define AE_ASR_RESULT_QUEUE_DEPTH 32
#define AE_ASR_TEXT_MAX 2048

typedef enum {
    AE_ASR_CMD_AUDIO = 0,
    AE_ASR_CMD_RESET,
    AE_ASR_CMD_EXIT,
} ae_asr_cmd_kind_t;

typedef struct ae_asr_cmd {
    int       kind;
    int       samples;
    uint64_t  pts_us;
    int16_t   pcm[480]; /* 10ms @ 48k 上限；ASR 实际只用 16k/160 */
} ae_asr_cmd_t;

typedef struct ae_asr_result {
    int      is_final;
    int      len;
    uint64_t pts_us;
    char     text[AE_ASR_TEXT_MAX];
} ae_asr_result_t;

typedef struct ae_asr_node {
    aura_node_t base;
    AsrHandle   h;
    bool        inited;
    bool        listening;
    bool        worker_stop;
    uint32_t    rate;
    uint32_t    frame_len;
    char        name[32];

    aura_queue_t *job_q;
    aura_queue_t *result_q;
    aura_task_t  *worker;

    int   num_threads;
    int   enable_endpoint;
    int   decoding_method;
    int   max_active_paths;
    float lm_scale;
    int   lm_shallow_fusion;
    int   lm_num_threads;
    int   enable_punctuation;
    int   punctuate_partial;

    char  model_type[32];
    char  encoder_path[AUDIO_ENGINE_MAX_FILE_PATH_LEN];
    char  decoder_path[AUDIO_ENGINE_MAX_FILE_PATH_LEN];
    char  joiner_path[AUDIO_ENGINE_MAX_FILE_PATH_LEN];
    char  tokens_path[AUDIO_ENGINE_MAX_FILE_PATH_LEN];
    char  lm_model_path[AUDIO_ENGINE_MAX_FILE_PATH_LEN];
    char  punctuation_model_path[AUDIO_ENGINE_MAX_FILE_PATH_LEN];
    char  punctuation_bpe_vocab_path[AUDIO_ENGINE_MAX_FILE_PATH_LEN];

    char  last_partial[AE_ASR_TEXT_MAX];

    uint64_t frames_in;
    uint64_t dropped;
    uint64_t finals;
    uint64_t errors;
} ae_asr_node_t;

static void asr_resolve(const aura_algo_params_t *p, const char *key, const char *def, char *dst,
                        size_t cap)
{
    const char *v = ae_param_str(p, key, def);
    if (ae_resolve_path(v, dst, cap) == NULL) {
        dst[0] = '\0';
    }
}

static void asr_enqueue_cmd(ae_asr_node_t *n, int kind)
{
    if (n->job_q == NULL) {
        return;
    }
    ae_asr_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.kind = kind;
    if (aura_osal_queue_push(n->job_q, &cmd, AURA_NO_WAIT) != AURA_OK) {
        /* 控制命令不能丢：先丢一条最老的音频再试。 */
        ae_asr_cmd_t tmp;
        (void)aura_osal_queue_pop(n->job_q, &tmp, AURA_NO_WAIT);
        (void)aura_osal_queue_push(n->job_q, &cmd, AURA_NO_WAIT);
    }
}

static void asr_worker(void *arg)
{
    ae_asr_node_t *n = (ae_asr_node_t *)arg;
    ae_asr_cmd_t cmd;
    char text[AE_ASR_TEXT_MAX];

    while (!n->worker_stop) {
        if (aura_osal_queue_pop(n->job_q, &cmd, 100) != AURA_OK) {
            continue;
        }
        if (cmd.kind == AE_ASR_CMD_EXIT) {
            break;
        }
        if (cmd.kind == AE_ASR_CMD_RESET) {
            if (n->h != NULL) {
                (void)AudioEngine_Asr_Reset(n->h);
            }
            n->last_partial[0] = '\0';
            continue;
        }

        AsrInput_t in = {cmd.pcm, cmd.samples};
        AsrResult_t out;
        memset(&out, 0, sizeof(out));
        text[0] = '\0';
        out.text          = text;
        out.text_capacity = (int)sizeof(text);
        int st = AudioEngine_Asr_Process(n->h, &in, &out);
        if (st != AUDIO_ENGINE_SUCCESS) {
            n->errors++;
            if (n->errors == 1) {
                AURA_LOGW(TAG, "%s: Asr_Process failed (0x%x)", n->base.name, st);
            }
            continue;
        }
        int len = (out.text_len > 0) ? (out.text_len - 1) : 0;
        if (len < 0) {
            len = 0;
        }
        if (len >= (int)sizeof(text)) {
            len = (int)sizeof(text) - 1;
        }
        text[len] = '\0';

        /* 未变化的 partial 不重复下发；final 总是下发。 */
        if (len > 0 && !out.is_final && strcmp(text, n->last_partial) == 0) {
            continue;
        }
        if (len == 0 && !out.is_final) {
            continue;
        }
        snprintf(n->last_partial, sizeof(n->last_partial), "%s", text);

        ae_asr_result_t r;
        memset(&r, 0, sizeof(r));
        r.is_final = out.is_final ? 1 : 0;
        r.len      = len;
        r.pts_us   = cmd.pts_us;
        snprintf(r.text, sizeof(r.text), "%s", text);

        if (aura_osal_queue_push(n->result_q, &r, AURA_NO_WAIT) != AURA_OK) {
            n->dropped++;
            continue;
        }
        if (n->base.bus != NULL) {
            (void)aura_event_bus_post(n->base.bus, AURA_EVENT_NN_RESULT, 0, n->base.name);
        }
        if (r.is_final) {
            n->finals++;
            n->last_partial[0] = '\0';
        }
    }
}

static void asr_deinit(aura_node_t *self);

static aura_err_t asr_init(aura_node_t *self)
{
    ae_asr_node_t *n = (ae_asr_node_t *)self;

    if (n->job_q == NULL) {
        n->job_q = aura_osal_queue_create(AE_ASR_JOB_QUEUE_DEPTH, sizeof(ae_asr_cmd_t));
        n->result_q = aura_osal_queue_create(AE_ASR_RESULT_QUEUE_DEPTH, sizeof(ae_asr_result_t));
        if (n->job_q == NULL || n->result_q == NULL) {
            return AURA_ERR_NOMEM;
        }
    }
    if (n->h == NULL) {
        n->h = AudioEngine_Asr_Create();
        if (n->h == NULL) {
            return AURA_ERR_NOMEM;
        }
    }

    AsrInitConfig c;
    memset(&c, 0, sizeof(c));
    c.sample_rate = (int)n->rate;
    c.frame_len   = (int)n->frame_len;
    c.num_threads = n->num_threads;
    c.enable_endpoint     = n->enable_endpoint;
    c.decoding_method     = n->decoding_method;
    c.max_active_paths    = n->max_active_paths;
    c.lm_scale            = n->lm_scale;
    c.lm_shallow_fusion   = n->lm_shallow_fusion;
    c.lm_num_threads      = n->lm_num_threads;
    c.enable_punctuation  = n->enable_punctuation;
    c.punctuate_partial   = n->punctuate_partial;
    snprintf(c.model_type, sizeof(c.model_type), "%s", n->model_type);
    snprintf(c.encoder_path, sizeof(c.encoder_path), "%s", n->encoder_path);
    snprintf(c.decoder_path, sizeof(c.decoder_path), "%s", n->decoder_path);
    snprintf(c.joiner_path, sizeof(c.joiner_path), "%s", n->joiner_path);
    snprintf(c.tokens_path, sizeof(c.tokens_path), "%s", n->tokens_path);
    snprintf(c.lm_model_path, sizeof(c.lm_model_path), "%s", n->lm_model_path);
    snprintf(c.punctuation_model_path, sizeof(c.punctuation_model_path), "%s",
             n->punctuation_model_path);
    snprintf(c.punctuation_bpe_vocab_path, sizeof(c.punctuation_bpe_vocab_path), "%s",
             n->punctuation_bpe_vocab_path);

    int st = AudioEngine_Asr_Init(n->h, &c);
    if (st != AUDIO_ENGINE_SUCCESS) {
        AURA_LOGE(TAG, "%s: Asr_Init failed (0x%x) encoder='%s'", self->name, st, n->encoder_path);
        asr_deinit(self); /* pipeline init 失败不会再调 deinit */
        return ae_status_to_err(st, self->name);
    }
    n->inited    = true;
    n->listening = false;
    n->last_partial[0] = '\0';
    return AURA_OK;
}

static aura_err_t asr_start(aura_node_t *self)
{
    ae_asr_node_t *n = (ae_asr_node_t *)self;
    if (n->worker != NULL) {
        return AURA_OK;
    }
    n->worker_stop = false;
    n->worker = aura_osal_task_create(n->name, 32768, 3, asr_worker, n);
    return (n->worker != NULL) ? AURA_OK : AURA_ERR_NOMEM;
}

static aura_err_t asr_stop(aura_node_t *self)
{
    ae_asr_node_t *n = (ae_asr_node_t *)self;
    if (n->worker == NULL) {
        return AURA_OK;
    }
    n->worker_stop = true;
    asr_enqueue_cmd(n, AE_ASR_CMD_EXIT);
    (void)aura_osal_task_join(n->worker, 10000);
    aura_osal_task_destroy(n->worker);
    n->worker = NULL;
    return AURA_OK;
}

static aura_err_t asr_process_audio(aura_node_t *self, const aura_audio_frame_t *frame)
{
    ae_asr_node_t *n = (ae_asr_node_t *)self;
    if (!n->inited || n->h == NULL || n->job_q == NULL) {
        return AURA_ERR_STATE;
    }
    if (!n->listening) {
        return AURA_OK; /* 唤醒前算力留给 VAD/KWS */
    }
    if (!ae_frame_ok(frame, 1, self->name) || frame->frame_count != n->frame_len) {
        return AURA_ERR_INVALID_ARG;
    }
    if (frame->frame_count > (uint32_t)(sizeof(((ae_asr_cmd_t *)0)->pcm) / sizeof(int16_t))) {
        return AURA_ERR_INVALID_ARG;
    }
    n->frames_in++;

    ae_asr_cmd_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.kind    = AE_ASR_CMD_AUDIO;
    cmd.samples = (int)frame->frame_count;
    cmd.pts_us  = frame->pts_us;
    memcpy(cmd.pcm, frame->data, (size_t)frame->frame_count * sizeof(int16_t));

    if (aura_osal_queue_push(n->job_q, &cmd, AURA_NO_WAIT) != AURA_OK) {
        n->dropped++;
        if (n->dropped == 1) {
            AURA_LOGW(TAG, "%s: ASR job queue full, dropping audio frames", self->name);
        }
    }
    return AURA_OK;
}

static void asr_on_event(aura_node_t *self, const aura_event_t *event)
{
    ae_asr_node_t *n = (ae_asr_node_t *)self;

    if (event->type == AURA_EVENT_NN_RESULT) {
        if (strcmp(event->payload, n->name) != 0) {
            return;
        }
        ae_asr_result_t r;
        while (n->result_q != NULL &&
               aura_osal_queue_pop(n->result_q, &r, AURA_NO_WAIT) == AURA_OK) {
            if (r.len <= 0) {
                continue;
            }
            aura_text_chunk_t chunk;
            memset(&chunk, 0, sizeof(chunk));
            chunk.pts_us = r.pts_us;
            chunk.flag   = r.is_final ? AURA_TEXT_FINAL : AURA_TEXT_PARTIAL;
            chunk.source = AURA_TEXT_SRC_ASR;
            size_t copy = (size_t)r.len;
            if (copy >= sizeof(chunk.text)) {
                copy = sizeof(chunk.text) - 1;
            }
            memcpy(chunk.text, r.text, copy);
            chunk.text[copy] = '\0';
            chunk.len        = (uint32_t)copy;
            (void)aura_node_emit_text(self, &chunk);
            int type = r.is_final ? AURA_EVENT_ASR_FINAL : AURA_EVENT_ASR_PARTIAL;
            if (self->bus != NULL) {
                (void)aura_event_bus_post(self->bus, (aura_event_type_t)type, 0, r.text);
            } else {
                (void)aura_node_post_event(self, type, 0);
            }
        }
        return;
    }

    if (event->type == AURA_EVENT_STATE_CHANGED) {
        bool should_listen = (event->code == AURA_STATE_LISTENING);
        if (should_listen && !n->listening) {
            /* 先关门 → 投复位 → 再开门：避免复位命令排在几帧旧音频之后。 */
            n->listening = false;
            asr_enqueue_cmd(n, AE_ASR_CMD_RESET);
            n->listening = true;
            AURA_LOGI(TAG, "%s: listening — ASR engine reset and armed", self->name);
        } else if (!should_listen && n->listening) {
            n->listening = false;
            asr_enqueue_cmd(n, AE_ASR_CMD_RESET);
        }
        return;
    }

    if (event->type == AURA_EVENT_BARGE_IN) {
        n->listening = false;
        asr_enqueue_cmd(n, AE_ASR_CMD_RESET);
    }
}

static aura_err_t asr_control(aura_node_t *self, aura_node_cmd_t cmd, const void *arg)
{
    (void)arg;
    ae_asr_node_t *n = (ae_asr_node_t *)self;
    if (cmd == AURA_CMD_RESET || cmd == AURA_CMD_FLUSH) {
        n->listening = false;
        asr_enqueue_cmd(n, AE_ASR_CMD_RESET);
    }
    return AURA_OK;
}

static void asr_deinit(aura_node_t *self)
{
    ae_asr_node_t *n = (ae_asr_node_t *)self;
    (void)asr_stop(self);
    if (n->job_q != NULL) {
        aura_osal_queue_destroy(n->job_q);
        n->job_q = NULL;
    }
    if (n->result_q != NULL) {
        aura_osal_queue_destroy(n->result_q);
        n->result_q = NULL;
    }
    if (n->h != NULL) {
        (void)AudioEngine_Asr_Deinit(n->h);
        (void)AudioEngine_Asr_Destroy(n->h);
        n->h = NULL;
    }
    n->inited = false;
}

static aura_err_t asr_selftest(aura_node_t *self)
{
    ae_asr_node_t *n = (ae_asr_node_t *)self;
    return (n->h != NULL) ? AURA_OK : AURA_ERR_STATE;
}

static const aura_node_ops_t k_asr_ops = {
    .init          = asr_init,
    .start         = asr_start,
    .stop          = asr_stop,
    .process_audio = asr_process_audio,
    .on_event      = asr_on_event,
    .control       = asr_control,
    .deinit        = asr_deinit,
    .selftest      = asr_selftest,
};

static aura_node_t *asr_create_node(const aura_algo_params_t *p, aura_err_t *err)
{
    ae_asr_node_t *n = (ae_asr_node_t *)calloc(1, sizeof(*n));
    if (n == NULL) {
        if (err != NULL) {
            *err = AURA_ERR_NOMEM;
        }
        return NULL;
    }
    n->rate      = p->sample_rate;
    n->frame_len = p->sample_rate / 100;
    ae_name_copy(n->name, sizeof(n->name), p->instance_name ? p->instance_name : "asr");

    n->num_threads       = ae_param_i32(p, "num_threads", 1);
    n->enable_endpoint   = ae_param_i32(p, "enable_endpoint", 1);
    n->decoding_method   = ae_param_i32(p, "decoding_method", 0);
    n->max_active_paths  = ae_param_i32(p, "max_active_paths", 4);
    n->lm_scale          = ae_param_f32(p, "lm_scale", 0.5f);
    n->lm_shallow_fusion = ae_param_i32(p, "lm_shallow_fusion", 1);
    n->lm_num_threads    = ae_param_i32(p, "lm_num_threads", 1);
    n->enable_punctuation = ae_param_i32(p, "enable_punctuation", 0);
    n->punctuate_partial = ae_param_i32(p, "punctuate_partial", 0);

    snprintf(n->model_type, sizeof(n->model_type), "%s", ae_param_str(p, "model_type", ""));
    const char *asr_dir = "asr/sherpa-mnn-streaming-zipformer-bilingual-zh-en-2023-02-20";
    char def[256];
    snprintf(def, sizeof(def), "%s/encoder-epoch-99-avg-1.int8.mnn", asr_dir);
    asr_resolve(p, "encoder_path", def, n->encoder_path, sizeof(n->encoder_path));
    snprintf(def, sizeof(def), "%s/decoder-epoch-99-avg-1.int8.mnn", asr_dir);
    asr_resolve(p, "decoder_path", def, n->decoder_path, sizeof(n->decoder_path));
    snprintf(def, sizeof(def), "%s/joiner-epoch-99-avg-1.int8.mnn", asr_dir);
    asr_resolve(p, "joiner_path", def, n->joiner_path, sizeof(n->joiner_path));
    snprintf(def, sizeof(def), "%s/tokens.txt", asr_dir);
    asr_resolve(p, "tokens_path", def, n->tokens_path, sizeof(n->tokens_path));
    asr_resolve(p, "lm_model_path", "", n->lm_model_path, sizeof(n->lm_model_path));
    asr_resolve(p, "punctuation_model_path", "", n->punctuation_model_path,
                sizeof(n->punctuation_model_path));
    asr_resolve(p, "punctuation_bpe_vocab_path", "", n->punctuation_bpe_vocab_path,
                sizeof(n->punctuation_bpe_vocab_path));

    aura_node_caps_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.consumes_audio = true;
    caps.produces_text  = true;
    caps.wants_events   = true;
    aura_node_init(&n->base, n->name, &k_asr_ops, &caps);
    return &n->base;
}

static const aura_algo_param_spec_t k_asr_specs[] = {
    {"num_threads", AURA_ALGO_PARAM_I32, 1, 8, "MNN inference threads"},
    {"enable_endpoint", AURA_ALGO_PARAM_I32, 0, 1, "split utterances on silence"},
    {"decoding_method", AURA_ALGO_PARAM_I32, 0, 1, "0=greedy 1=modified_beam_search"},
    {"max_active_paths", AURA_ALGO_PARAM_I32, 1, 16, "beam width"},
    {"lm_scale", AURA_ALGO_PARAM_F32, 0.0f, 10.0f, "LM scale"},
    {"lm_shallow_fusion", AURA_ALGO_PARAM_I32, 0, 1, "1=shallow fusion"},
    {"lm_num_threads", AURA_ALGO_PARAM_I32, 1, 8, "LM threads"},
    {"enable_punctuation", AURA_ALGO_PARAM_I32, 0, 1, "punctuation post-processing"},
    {"punctuate_partial", AURA_ALGO_PARAM_I32, 0, 1, "punctuate partial text"},
    {"model_type", AURA_ALGO_PARAM_STR, 0, 0, "empty=auto"},
    {"encoder_path", AURA_ALGO_PARAM_STR, 0, 0, "encoder .mnn"},
    {"decoder_path", AURA_ALGO_PARAM_STR, 0, 0, "decoder .mnn"},
    {"joiner_path", AURA_ALGO_PARAM_STR, 0, 0, "joiner .mnn"},
    {"tokens_path", AURA_ALGO_PARAM_STR, 0, 0, "tokens.txt"},
    {"lm_model_path", AURA_ALGO_PARAM_STR, 0, 0, "optional RNN LM"},
    {"punctuation_model_path", AURA_ALGO_PARAM_STR, 0, 0, "optional punctuation model"},
    {"punctuation_bpe_vocab_path", AURA_ALGO_PARAM_STR, 0, 0, "optional BPE vocab"},
};

static const aura_algo_desc_t k_asr_desc = {
    .name = "asr",
    .version = 1,
    .kind = AURA_ALGO_KIND_NN_ASYNC,
    .io = {.consumes_audio = true, .produces_audio = false, .produces_text = true,
           .wants_events = true},
    .param_specs = k_asr_specs,
    .param_spec_count = sizeof(k_asr_specs) / sizeof(k_asr_specs[0]),
    .create = asr_create_node,
};

aura_err_t ae_asr_register_all(void)
{
    return aura_algo_register(&k_asr_desc);
}
