/* Aura — soundproof SR（说话人识别）→ pipeline 观察者节点
 *
 * 在线接口分两段：
 *   trigger=0：只缓存特征（每 10ms 喂一帧）；
 *   trigger=1：对缓存特征跑一次推理（KWS 命中后的下一帧触发）。
 *
 * 注册（enroll）是离线/上层动作，走 soundproof 的 AudioEngine_Sr_Register*；
 * Aura 节点只做验证，不持有注册库文件格式。
 */
#include "soundproof/ae_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_engine_sr.h"
#include "utils/logger/logger.h"

#define TAG "ae_sr"

typedef struct ae_sr_node {
    aura_node_t base;
    SrHandle    h;
    bool        inited;
    uint32_t    rate;
    uint32_t    frame_len;
    char        name[32];

    int   num_threads;
    float threshold;
    char  model_type[32];
    char  model_path[AUDIO_ENGINE_MAX_FILE_PATH_LEN];

    bool trigger_pending;
    char speaker_name[128];

    uint64_t frames_in;
    uint64_t triggers;
    uint64_t errors;
} ae_sr_node_t;

static aura_err_t sr_init(aura_node_t *self)
{
    ae_sr_node_t *n = (ae_sr_node_t *)self;
    if (n->h == NULL) {
        n->h = AudioEngine_Sr_Create();
        if (n->h == NULL) {
            return AURA_ERR_NOMEM;
        }
    }
    SrInitConfig c;
    memset(&c, 0, sizeof(c));
    c.sample_rate = (int)n->rate;
    c.frame_len   = (int)n->frame_len;
    c.num_threads = n->num_threads;
    snprintf(c.model_type, sizeof(c.model_type), "%s", n->model_type);
    snprintf(c.model_path, sizeof(c.model_path), "%s", n->model_path);

    int st = AudioEngine_Sr_Init(n->h, &c);
    if (st != AUDIO_ENGINE_SUCCESS) {
        AURA_LOGE(TAG, "%s: Sr_Init failed (0x%x) model='%s'", self->name, st, n->model_path);
        (void)AudioEngine_Sr_Destroy(n->h);
        n->h = NULL;
        return ae_status_to_err(st, self->name);
    }
    SrRtConfig rt = {n->threshold};
    st = AudioEngine_Sr_SetParam(n->h, &rt);
    if (st != AUDIO_ENGINE_SUCCESS) {
        AURA_LOGW(TAG, "%s: Sr_SetParam failed (0x%x), using defaults", self->name, st);
    }
    n->inited = true;
    return AURA_OK;
}

static void sr_emit_result(ae_sr_node_t *n, const SrResult_t *r)
{
    aura_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type                 = AURA_EVENT_VOICEPRINT_RESULT;
    ev.u.voiceprint.passed  = (r->has_result && r->score >= n->threshold && n->speaker_name[0] != '\0');
    ev.u.voiceprint.score   = (int32_t)(r->score * 100.0f);
    ev.code                 = ev.u.voiceprint.passed ? 0 : -1;
    snprintf(ev.payload, sizeof(ev.payload), "%s", n->speaker_name);
    (void)aura_node_emit_event(&n->base, &ev);
}

static aura_err_t sr_process_audio(aura_node_t *self, const aura_audio_frame_t *frame)
{
    ae_sr_node_t *n = (ae_sr_node_t *)self;
    if (!n->inited || n->h == NULL) {
        return AURA_ERR_STATE;
    }
    if (!ae_frame_ok(frame, 1, self->name) || frame->frame_count != n->frame_len) {
        return AURA_ERR_INVALID_ARG;
    }
    n->frames_in++;

    SrInput_t in;
    memset(&in, 0, sizeof(in));
    in.audio_in = (const short *)frame->data;
    in.samples  = (int)frame->frame_count;
    in.trigger  = n->trigger_pending ? 1 : 0;
    n->trigger_pending = false;

    SrResult_t out;
    memset(&out, 0, sizeof(out));
    out.speaker_name = n->speaker_name;
    out.name_capacity = (int)sizeof(n->speaker_name);
    n->speaker_name[0] = '\0';

    int st = AudioEngine_Sr_Process(n->h, &in, &out);
    if (st != AUDIO_ENGINE_SUCCESS) {
        n->errors++;
        return ae_status_to_err(st, self->name);
    }
    if (in.trigger) {
        n->triggers++;
    }
    if (out.has_result) {
        sr_emit_result(n, &out);
    }
    return AURA_OK;
}

static void sr_on_event(aura_node_t *self, const aura_event_t *event)
{
    ae_sr_node_t *n = (ae_sr_node_t *)self;
    if (event->type == AURA_EVENT_KWS_HIT) {
        n->trigger_pending = true;
    } else if (event->type == AURA_EVENT_BARGE_IN) {
        n->trigger_pending = false;
    }
}

static aura_err_t sr_control(aura_node_t *self, aura_node_cmd_t cmd, const void *arg)
{
    (void)arg;
    ae_sr_node_t *n = (ae_sr_node_t *)self;
    if (cmd == AURA_CMD_RESET || cmd == AURA_CMD_FLUSH) {
        n->trigger_pending = false;
        if (n->h != NULL) {
            return ae_status_to_err(AudioEngine_Sr_Reset(n->h), self->name);
        }
    }
    return AURA_OK;
}

static void sr_deinit(aura_node_t *self)
{
    ae_sr_node_t *n = (ae_sr_node_t *)self;
    if (n->h != NULL) {
        (void)AudioEngine_Sr_Deinit(n->h);
        (void)AudioEngine_Sr_Destroy(n->h);
        n->h = NULL;
    }
    n->inited = false;
}

static aura_err_t sr_selftest(aura_node_t *self)
{
    ae_sr_node_t *n = (ae_sr_node_t *)self;
    return (n->h != NULL) ? AURA_OK : AURA_ERR_STATE;
}

static const aura_node_ops_t k_sr_ops = {
    .init          = sr_init,
    .process_audio = sr_process_audio,
    .on_event      = sr_on_event,
    .control       = sr_control,
    .deinit        = sr_deinit,
    .selftest      = sr_selftest,
};

static aura_node_t *sr_create_node(const aura_algo_params_t *p, aura_err_t *err)
{
    ae_sr_node_t *n = (ae_sr_node_t *)calloc(1, sizeof(*n));
    if (n == NULL) {
        if (err != NULL) {
            *err = AURA_ERR_NOMEM;
        }
        return NULL;
    }
    n->rate        = p->sample_rate;
    n->frame_len   = p->sample_rate / 100;
    n->num_threads = ae_param_i32(p, "num_threads", 1);
    n->threshold   = ae_param_f32(p, "threshold", 0.6f);
    ae_name_copy(n->name, sizeof(n->name), p->instance_name ? p->instance_name : "sr");
    snprintf(n->model_type, sizeof(n->model_type), "%s", ae_param_str(p, "model_type", ""));
    const char *model = ae_param_str(
        p, "model_path", "speaker_recognition/3dspeaker_speech_eres2netv2_sv_zh-cn_16k-common.mnn");
    if (ae_resolve_path(model, n->model_path, sizeof(n->model_path)) == NULL) {
        n->model_path[0] = '\0';
    }

    aura_node_caps_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.consumes_audio = true;
    caps.wants_events   = true;
    aura_node_init(&n->base, n->name, &k_sr_ops, &caps);
    return &n->base;
}

static const aura_algo_param_spec_t k_sr_specs[] = {
    {"num_threads", AURA_ALGO_PARAM_I32, 1, 8, "MNN inference threads"},
    {"threshold", AURA_ALGO_PARAM_F32, 0.0f, 1.0f, "identification threshold"},
    {"model_type", AURA_ALGO_PARAM_STR, 0, 0, "empty=auto (3d-speaker/wespeaker/nemo)"},
    {"model_path", AURA_ALGO_PARAM_STR, 0, 0, "speaker embedding .mnn"},
};

static const aura_algo_desc_t k_sr_desc = {
    .name = "sr",
    .version = 1,
    .kind = AURA_ALGO_KIND_NN_SYNC,
    .io = {.consumes_audio = true, .produces_audio = false, .wants_events = true},
    .param_specs = k_sr_specs,
    .param_spec_count = sizeof(k_sr_specs) / sizeof(k_sr_specs[0]),
    .create = sr_create_node,
};

aura_err_t ae_sr_register_all(void)
{
    return aura_algo_register(&k_sr_desc);
}
