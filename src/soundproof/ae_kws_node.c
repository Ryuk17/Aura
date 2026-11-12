/* Aura — soundproof KWS → pipeline 观察者节点
 *
 * KWS 只消费音频、不产出音频；命中结果是事件（AURA_EVENT_KWS_HIT），
 * 由状态机推进到 Listening，并触发声纹校验。
 */
#include "soundproof/ae_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_engine_kws.h"
#include "utils/logger/logger.h"

#define TAG "ae_kws"

typedef struct ae_kws_node {
    aura_node_t base;
    KwsHandle   h;
    bool        inited;
    uint32_t    rate;
    uint32_t    frame_len;
    char        name[32];

    int  num_threads;
    float threshold;

    char encoder_path[AUDIO_ENGINE_MAX_FILE_PATH_LEN];
    char decoder_path[AUDIO_ENGINE_MAX_FILE_PATH_LEN];
    char joiner_path[AUDIO_ENGINE_MAX_FILE_PATH_LEN];
    char tokens_path[AUDIO_ENGINE_MAX_FILE_PATH_LEN];
    char keywords_path[AUDIO_ENGINE_MAX_FILE_PATH_LEN];

    uint64_t frames_in;
    uint64_t hits;
    uint64_t errors;
} ae_kws_node_t;

static aura_err_t kws_init(aura_node_t *self)
{
    ae_kws_node_t *n = (ae_kws_node_t *)self;
    if (n->h == NULL) {
        n->h = AudioEngine_Kws_Create();
        if (n->h == NULL) {
            return AURA_ERR_NOMEM;
        }
    }
    KwsInitConfig c;
    memset(&c, 0, sizeof(c));
    c.sample_rate = (int)n->rate;
    c.frame_len   = (int)n->frame_len;
    c.num_threads = n->num_threads;
    snprintf(c.encoder_path, sizeof(c.encoder_path), "%s", n->encoder_path);
    snprintf(c.decoder_path, sizeof(c.decoder_path), "%s", n->decoder_path);
    snprintf(c.joiner_path, sizeof(c.joiner_path), "%s", n->joiner_path);
    snprintf(c.tokens_path, sizeof(c.tokens_path), "%s", n->tokens_path);
    snprintf(c.keywords_file_path, sizeof(c.keywords_file_path), "%s", n->keywords_path);

    int st = AudioEngine_Kws_Init(n->h, &c);
    if (st != AUDIO_ENGINE_SUCCESS) {
        AURA_LOGE(TAG, "%s: Kws_Init failed (0x%x) encoder='%s'", self->name, st,
                  n->encoder_path);
        (void)AudioEngine_Kws_Destroy(n->h);
        n->h = NULL;
        return ae_status_to_err(st, self->name);
    }
    KwsRtConfig rt = {n->threshold};
    st = AudioEngine_Kws_SetParam(n->h, &rt);
    if (st != AUDIO_ENGINE_SUCCESS) {
        AURA_LOGW(TAG, "%s: Kws_SetParam failed (0x%x), using defaults", self->name, st);
    }
    n->inited = true;
    return AURA_OK;
}

static aura_err_t kws_process_audio(aura_node_t *self, const aura_audio_frame_t *frame)
{
    ae_kws_node_t *n = (ae_kws_node_t *)self;
    if (!n->inited || n->h == NULL) {
        return AURA_ERR_STATE;
    }
    if (!ae_frame_ok(frame, 1, self->name) || frame->frame_count != n->frame_len) {
        return AURA_ERR_INVALID_ARG;
    }
    n->frames_in++;

    KwsInput_t in = {(const short *)frame->data, (int)frame->frame_count};
    KwsResult_t out;
    memset(&out, 0, sizeof(out));
    out.kws_index = -1;
    int st = AudioEngine_Kws_Process(n->h, &in, &out);
    if (st != AUDIO_ENGINE_SUCCESS) {
        n->errors++;
        return ae_status_to_err(st, self->name);
    }
    if (out.kws_index >= 0) {
        n->hits++;
        aura_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.type        = AURA_EVENT_KWS_HIT;
        ev.code        = out.kws_index;
        ev.u.kws.score = (int32_t)(out.kws_score * 100.0f);
        snprintf(ev.payload, sizeof(ev.payload), "kws:%d", out.kws_index);
        (void)aura_node_emit_event(self, &ev);
    }
    return AURA_OK;
}

static aura_err_t kws_control(aura_node_t *self, aura_node_cmd_t cmd, const void *arg)
{
    (void)arg;
    ae_kws_node_t *n = (ae_kws_node_t *)self;
    if ((cmd == AURA_CMD_RESET || cmd == AURA_CMD_FLUSH) && n->h != NULL) {
        return ae_status_to_err(AudioEngine_Kws_Reset(n->h), self->name);
    }
    return AURA_OK;
}

static void kws_deinit(aura_node_t *self)
{
    ae_kws_node_t *n = (ae_kws_node_t *)self;
    if (n->h != NULL) {
        (void)AudioEngine_Kws_Deinit(n->h);
        (void)AudioEngine_Kws_Destroy(n->h);
        n->h = NULL;
    }
    n->inited = false;
}

static aura_err_t kws_selftest(aura_node_t *self)
{
    ae_kws_node_t *n = (ae_kws_node_t *)self;
    return (n->h != NULL) ? AURA_OK : AURA_ERR_STATE;
}

static const aura_node_ops_t k_kws_ops = {
    .init          = kws_init,
    .process_audio = kws_process_audio,
    .control       = kws_control,
    .deinit        = kws_deinit,
    .selftest      = kws_selftest,
};

static void resolve_into(const aura_algo_params_t *p, const char *key, const char *def, char *dst,
                         size_t cap)
{
    const char *v = ae_param_str(p, key, def);
    if (ae_resolve_path(v, dst, cap) == NULL) {
        dst[0] = '\0';
    }
}

static aura_node_t *kws_create_node(const aura_algo_params_t *p, aura_err_t *err)
{
    ae_kws_node_t *n = (ae_kws_node_t *)calloc(1, sizeof(*n));
    if (n == NULL) {
        if (err != NULL) {
            *err = AURA_ERR_NOMEM;
        }
        return NULL;
    }
    n->rate      = p->sample_rate;
    n->frame_len = p->sample_rate / 100;
    ae_name_copy(n->name, sizeof(n->name), p->instance_name ? p->instance_name : "kws");
    n->num_threads = ae_param_i32(p, "num_threads", 1);
    n->threshold   = ae_param_f32(p, "threshold", 0.25f);

    const char *kws_dir = "kws/kws-zipformer-wenetspeech-3.3M-MNN";
    char def[256];
    snprintf(def, sizeof(def), "%s/encoder-epoch-99-avg-1-chunk-16-left-64.mnn", kws_dir);
    resolve_into(p, "encoder_path", def, n->encoder_path, sizeof(n->encoder_path));
    snprintf(def, sizeof(def), "%s/decoder-epoch-99-avg-1-chunk-16-left-64.mnn", kws_dir);
    resolve_into(p, "decoder_path", def, n->decoder_path, sizeof(n->decoder_path));
    snprintf(def, sizeof(def), "%s/joiner-epoch-99-avg-1-chunk-16-left-64.mnn", kws_dir);
    resolve_into(p, "joiner_path", def, n->joiner_path, sizeof(n->joiner_path));
    snprintf(def, sizeof(def), "%s/tokens.txt", kws_dir);
    resolve_into(p, "tokens_path", def, n->tokens_path, sizeof(n->tokens_path));
    snprintf(def, sizeof(def), "%s/keywords.txt", kws_dir);
    resolve_into(p, "keywords_file_path", def, n->keywords_path, sizeof(n->keywords_path));

    aura_node_caps_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.consumes_audio = true;
    aura_node_init(&n->base, n->name, &k_kws_ops, &caps);
    return &n->base;
}

static const aura_algo_param_spec_t k_kws_specs[] = {
    {"num_threads", AURA_ALGO_PARAM_I32, 1, 8, "MNN inference threads"},
    {"threshold", AURA_ALGO_PARAM_F32, 0.0f, 1.0f, "keyword trigger threshold"},
    {"encoder_path", AURA_ALGO_PARAM_STR, 0, 0, "zipformer2 encoder .mnn"},
    {"decoder_path", AURA_ALGO_PARAM_STR, 0, 0, "zipformer2 decoder .mnn"},
    {"joiner_path", AURA_ALGO_PARAM_STR, 0, 0, "zipformer2 joiner .mnn"},
    {"tokens_path", AURA_ALGO_PARAM_STR, 0, 0, "tokens.txt"},
    {"keywords_file_path", AURA_ALGO_PARAM_STR, 0, 0, "keywords.txt"},
};

static const aura_algo_desc_t k_kws_desc = {
    .name = "kws",
    .version = 1,
    .kind = AURA_ALGO_KIND_NN_SYNC,
    .io = {.consumes_audio = true, .produces_audio = false},
    .param_specs = k_kws_specs,
    .param_spec_count = sizeof(k_kws_specs) / sizeof(k_kws_specs[0]),
    .create = kws_create_node,
};

aura_err_t ae_kws_register_all(void)
{
    return aura_algo_register(&k_kws_desc);
}
