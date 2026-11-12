/* Aura — soundproof VAD → pipeline 观察者节点
 *
 * VAD 是“只消费音频、不产出音频”的旁路节点：pipeline 把它挂在 tee() 上，
 * 主线继续。判定结果只发**边沿事件**（SPEECH_START / SPEECH_END），
 * 每帧发事件会刷爆总线，下游要的本来就是状态变化。
 *
 * 播放期间抬高阈值（宁漏打断不可自打断）：订阅 TTS/PLAYBACK 事件，
 * 在 Speaking 状态用 playback_threshold 替换 threshold。
 */
#include "soundproof/ae_common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio_engine_vad.h"
#include "utils/logger/logger.h"

#define TAG "ae_vad"

typedef struct ae_vad_node {
    aura_node_t base;
    VadHandle   h;
    bool        inited;
    uint32_t    rate;
    uint32_t    frame_len;
    char        name[32];
    char        model_path[AUDIO_ENGINE_MAX_FILE_PATH_LEN];

    int   vad_mode;
    float threshold;
    float playback_threshold;
    float min_speech_duration;
    float min_silence_duration;
    float hysteresis;
    bool  playback_active;
    bool  playback_threshold_applied;

    int  last_flag;
    bool flag_valid;

    uint64_t frames_in;
    uint64_t errors;
} ae_vad_node_t;

static void vad_post_edge(ae_vad_node_t *n, int flag)
{
    if (n->flag_valid && n->last_flag == flag) {
        return;
    }
    n->last_flag  = flag;
    n->flag_valid = true;
    int type = flag ? AURA_EVENT_VAD_SPEECH_START : AURA_EVENT_VAD_SPEECH_END;
    (void)aura_node_post_event(&n->base, type, flag ? 100 : 0);
}

/* 只在 node task 里调用：soundproof 的句柄不是线程安全的，SetParam 不能和
 * Process 并发。on_event 只置标志，阈值切换推迟到 process_audio 开头。 */
static void vad_apply_threshold(ae_vad_node_t *n)
{
    if (n->h == NULL || n->playback_active == n->playback_threshold_applied) {
        return;
    }
    VadRtConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.threshold = n->playback_active ? n->playback_threshold : n->threshold;
    if (AudioEngine_Vad_SetParam(n->h, &cfg) == AUDIO_ENGINE_SUCCESS) {
        n->playback_threshold_applied = n->playback_active;
    }
}

static aura_err_t vad_init(aura_node_t *self)
{
    ae_vad_node_t *n = (ae_vad_node_t *)self;
    if (n->h == NULL) {
        n->h = AudioEngine_Vad_Create();
        if (n->h == NULL) {
            return AURA_ERR_NOMEM;
        }
    }

    /* 参数已在工厂期拷进 state；字符串在工厂期已解析到 n->model_path。 */
    VadInitConfig c;
    memset(&c, 0, sizeof(c));
    c.sample_rate = (int)n->rate;
    c.frame_len   = (int)n->frame_len;
    c.vad_mode    = n->vad_mode;
    snprintf(c.model_path, sizeof(c.model_path), "%s", n->model_path);

    int st = AudioEngine_Vad_Init(n->h, &c);
    if (st != AUDIO_ENGINE_SUCCESS) {
        AURA_LOGE(TAG, "%s: Vad_Init failed (0x%x), mode=%d model='%s'", self->name, st,
                  n->vad_mode, n->model_path);
        (void)AudioEngine_Vad_Destroy(n->h);
        n->h = NULL;
        return ae_status_to_err(st, self->name);
    }

    VadRtConfig rt;
    memset(&rt, 0, sizeof(rt));
    rt.threshold            = n->threshold;
    rt.min_speech_duration  = n->min_speech_duration;
    rt.min_silence_duration = n->min_silence_duration;
    rt.hysteresis           = n->hysteresis;
    st = AudioEngine_Vad_SetParam(n->h, &rt);
    if (st != AUDIO_ENGINE_SUCCESS) {
        AURA_LOGW(TAG, "%s: Vad_SetParam failed (0x%x), using engine defaults", self->name, st);
    }
    n->inited    = true;
    n->flag_valid = false;
    return AURA_OK;
}

static aura_err_t vad_process_audio(aura_node_t *self, const aura_audio_frame_t *frame)
{
    ae_vad_node_t *n = (ae_vad_node_t *)self;
    if (!n->inited || n->h == NULL) {
        return AURA_ERR_STATE;
    }
    if (!ae_frame_ok(frame, 1, self->name)) {
        return AURA_ERR_INVALID_ARG;
    }
    if (frame->frame_count != n->frame_len) {
        return AURA_ERR_INVALID_ARG;
    }
    n->frames_in++;
    vad_apply_threshold(n);

    VadInput_t in = {(const short *)frame->data, (int)frame->frame_count};
    VadResult_t out;
    memset(&out, 0, sizeof(out));
    int st = AudioEngine_Vad_Process(n->h, &in, &out);
    if (st != AUDIO_ENGINE_SUCCESS) {
        n->errors++;
        return ae_status_to_err(st, self->name);
    }
    vad_post_edge(n, out.vad_flag ? 1 : 0);
    return AURA_OK;
}

static void vad_on_event(aura_node_t *self, const aura_event_t *event)
{
    ae_vad_node_t *n = (ae_vad_node_t *)self;
    switch (event->type) {
    case AURA_EVENT_TTS_FIRST_CHUNK:
    case AURA_EVENT_PLAYBACK_START:
        n->playback_active = true; /* 实际 SetParam 在 process_audio 里做 */
        break;
    case AURA_EVENT_TTS_DONE:
    case AURA_EVENT_PLAYBACK_STOP:
        n->playback_active = false;
        break;
    default:
        break;
    }
}

static aura_err_t vad_control(aura_node_t *self, aura_node_cmd_t cmd, const void *arg)
{
    (void)arg;
    ae_vad_node_t *n = (ae_vad_node_t *)self;
    if (cmd == AURA_CMD_RESET || cmd == AURA_CMD_FLUSH) {
        n->flag_valid = false;
        n->last_flag  = 0;
        if (n->h != NULL) {
            return ae_status_to_err(AudioEngine_Vad_Reset(n->h), self->name);
        }
    }
    return AURA_OK;
}

static void vad_deinit(aura_node_t *self)
{
    ae_vad_node_t *n = (ae_vad_node_t *)self;
    if (n->h != NULL) {
        (void)AudioEngine_Vad_Deinit(n->h);
        (void)AudioEngine_Vad_Destroy(n->h);
        n->h = NULL;
    }
    n->inited = false;
}

static aura_err_t vad_selftest(aura_node_t *self)
{
    ae_vad_node_t *n = (ae_vad_node_t *)self;
    return (n->h != NULL) ? AURA_OK : AURA_ERR_STATE;
}

static const aura_node_ops_t k_vad_ops = {
    .init          = vad_init,
    .process_audio = vad_process_audio,
    .on_event      = vad_on_event,
    .control       = vad_control,
    .deinit        = vad_deinit,
    .selftest      = vad_selftest,
};

static aura_node_t *vad_create_node(const aura_algo_params_t *p, aura_err_t *err)
{
    ae_vad_node_t *n = (ae_vad_node_t *)calloc(1, sizeof(*n));
    if (n == NULL) {
        if (err != NULL) {
            *err = AURA_ERR_NOMEM;
        }
        return NULL;
    }
    n->rate      = p->sample_rate;
    n->frame_len = p->sample_rate / 100;
    ae_name_copy(n->name, sizeof(n->name), p->instance_name ? p->instance_name : "vad");

    n->vad_mode            = ae_param_i32(p, "vad_mode", 1);
    n->threshold           = ae_param_f32(p, "threshold", 0.5f);
    n->playback_threshold  = ae_param_f32(p, "playback_threshold", 0.65f);
    n->min_speech_duration = ae_param_f32(p, "min_speech_duration", 0.25f);
    n->min_silence_duration = ae_param_f32(p, "min_silence_duration", 0.5f);
    n->hysteresis          = ae_param_f32(p, "hysteresis", 0.15f);

    const char *model = ae_param_str(p, "model_path", "vad/silero_vad.mnn/silero_vad.mnn");
    if (ae_resolve_path(model, n->model_path, sizeof(n->model_path)) == NULL) {
        n->model_path[0] = '\0';
    }

    aura_node_caps_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.consumes_audio = true;
    caps.wants_events   = true;
    aura_node_init(&n->base, n->name, &k_vad_ops, &caps);
    return &n->base;
}

static const aura_algo_param_spec_t k_vad_specs[] = {
    {"vad_mode", AURA_ALGO_PARAM_I32, 0, 1, "0=WebRTC DSP, 1=silero (needs MNN)"},
    {"threshold", AURA_ALGO_PARAM_F32, 0.0f, 1.0f, "speech threshold"},
    {"playback_threshold", AURA_ALGO_PARAM_F32, 0.0f, 1.0f, "threshold while playing"},
    {"min_speech_duration", AURA_ALGO_PARAM_F32, 0.0f, 10.0f, "onset debounce seconds"},
    {"min_silence_duration", AURA_ALGO_PARAM_F32, 0.0f, 10.0f, "hangover seconds"},
    {"hysteresis", AURA_ALGO_PARAM_F32, 0.0f, 1.0f, "active threshold hysteresis"},
    {"model_path", AURA_ALGO_PARAM_STR, 0, 0, "silero model path (relative to model_dir)"},
};

static const aura_algo_desc_t k_vad_desc = {
    .name = "vad",
    .version = 1,
    .kind = AURA_ALGO_KIND_DSP,
    .io = {.consumes_audio = true, .produces_audio = false, .wants_events = true},
    .param_specs = k_vad_specs,
    .param_spec_count = sizeof(k_vad_specs) / sizeof(k_vad_specs[0]),
    .create = vad_create_node,
};

aura_err_t ae_vad_register_all(void)
{
    return aura_algo_register(&k_vad_desc);
}
