/* Aura — silero VAD 描述表（NN_SYNC 接入，从 tests/host_sim 搬进正式模块）
 *
 * 这是"算法统一接口"在 NN 族的验证点：真实模型经 algorithm/aura_nn_adapter.h
 * 的模板接入 —— 与 DSP 一族共用注册名 / 描述表 / 工厂 / 节点契约。
 *
 * silero v5 窗口口径（官方 silero-vad-model.cc）：每次推理喂 512 个新样本，
 * 模型窗口 = 64 个上文样本 + 512 新样本 = 576。64 个上文是模型判句内/句间的
 * 依据，漏掉会让概率跳变 —— 攒窗交给框架的 aura_nn_window_*（帧长 160 与
 * 窗长 512 不成整数倍，手写容易把跨窗那帧多出来的样本丢掉，表现为判定漂移
 * 且不报错）。
 *
 * 滞回：silero 在句尾也会有零星低概率窗，直接按阈值翻转会让 VAD_END 抖动。
 * 连续 high_run 窗才报开始、连续 low_run 窗才报结束（与能量 VAD 同口径）。
 * 播放期间（on_event 置 playing）把"认语音"所需连续窗数从 2 抬到 4：
 * 播放回声/残留更容易出孤立高概率窗，宁漏打断不可自打断（todo.md 4.5）。
 */
#include <string.h>

#include "algorithm/aura_nn_adapter.h"
#include "inference/inference.h"
#include "utils/logger/logger.h"

#define TAG "vad.silero"

#define SILERO_CTX_SAMPLES 64
#define SILERO_NEW_SAMPLES 512
#define SILERO_WIN_SAMPLES (SILERO_CTX_SAMPLES + SILERO_NEW_SAMPLES)

typedef struct {
    aura_nn_window_t win;
    float    hc[2][128]; /* LSTM 递推状态 (2,1,128) */
    int32_t  sr;         /* 模型的 sr 输入（int32 标量） */
    bool     playing;    /* TTS 播放中（on_event 置位）→ 抬高判定门槛 */
    float    threshold;  /* 概率判定阈值（默认 0.5，配置可覆盖） */
    uint32_t high_run;
    uint32_t low_run;
} silero_state_t;

static aura_err_t silero_algo_init(aura_nn_node_t *self, aura_infer_model_t *model)
{
    silero_state_t *s = aura_nn_state(self);
    if (model == NULL) {
        AURA_LOGE(TAG, "silero: model is mandatory");
        return AURA_ERR_MODEL;
    }
    memset(s, 0, sizeof(*s));
    s->sr = (int32_t)self->rate; /* 链上传播后的生效采样率，不是写死的 16000 */
    /* 配置里的 chain_param_silero_vad.threshold 走到这里（描述表声明过才允许配，
     * 拼错的键在 init 期就报 NOT_FOUND）。 */
    s->threshold = aura_algo_params_f32(&self->params, "threshold", 0.5f);

    aura_err_t rc = aura_nn_window_init(&s->win, SILERO_CTX_SAMPLES, SILERO_NEW_SAMPLES);
    if (rc != AURA_OK) {
        return rc;
    }
    /* 形状固定，初始化时定形一次即可（窗口与 LSTM 状态在会话内不变） */
    aura_tensor_t *in = aura_model_input(model, "input");
    aura_tensor_t *st = aura_model_input(model, "state");
    int32_t        in_shape[2] = {1, SILERO_WIN_SAMPLES};
    int32_t        st_shape[3] = {2, 1, 128};
    if (in == NULL || st == NULL || aura_tensor_resize(in, in_shape, 2) != AURA_OK ||
        aura_tensor_resize(st, st_shape, 3) != AURA_OK ||
        aura_tensor_bytes(in) != sizeof(float) * SILERO_WIN_SAMPLES ||
        aura_tensor_bytes(st) != sizeof(s->hc)) {
        AURA_LOGE(TAG, "silero: tensor shape setup failed");
        return AURA_ERR_MODEL;
    }
    AURA_LOGI(TAG, "silero: ready (window=%d = %d ctx + %d new, sr=%d, threshold=%.2f)",
              SILERO_WIN_SAMPLES, SILERO_CTX_SAMPLES, SILERO_NEW_SAMPLES, (int)s->sr,
              (double)s->threshold);
    return AURA_OK;
}

static bool silero_frame_in(aura_nn_node_t *self, const aura_audio_frame_t *frame)
{
    silero_state_t *s   = aura_nn_state(self);
    const float    *win = aura_nn_window_push(&s->win, (const int16_t *)frame->data,
                                              frame->frame_count);
    if (win == NULL) {
        return false; /* 还没攒够一窗 */
    }
    aura_infer_model_t *m  = aura_nn_model(self);
    aura_tensor_t      *in = aura_model_input(m, "input");
    aura_tensor_t      *sr = aura_model_input(m, "sr");
    aura_tensor_t      *st = aura_model_input(m, "state");
    if (in == NULL || sr == NULL || st == NULL) {
        AURA_LOGE(TAG, "silero: missing tensors in=%p sr=%p st=%p", (void *)in, (void *)sr,
                  (void *)st);
        return false;
    }
    aura_tensor_write(in, win, sizeof(float) * SILERO_WIN_SAMPLES);
    aura_tensor_write(sr, &s->sr, sizeof(s->sr));
    aura_tensor_write(st, s->hc, sizeof(s->hc));
    return true;
}

static aura_err_t silero_infer_out(aura_nn_node_t *self)
{
    silero_state_t      *s   = aura_nn_state(self);
    aura_infer_model_t  *m   = aura_nn_model(self);
    aura_tensor_t       *out = aura_model_output(m, "output");
    aura_tensor_t       *stn = aura_model_output(m, "stateN");
    float                prob = 0.f;
    if (out == NULL || stn == NULL ||
        aura_tensor_read(out, &prob, aura_tensor_bytes(out)) != AURA_OK ||
        aura_tensor_read(stn, s->hc, sizeof(s->hc)) != AURA_OK) {
        AURA_LOGE(TAG, "silero: read outputs failed out=%p stn=%p", (void *)out, (void *)stn);
        aura_nn_window_consume(&s->win); /* 窗口已经喂过，必须消费掉，否则下一窗错位 */
        return AURA_ERR_FAIL;
    }
    bool     speech    = (prob > s->threshold);
    uint32_t need_high = s->playing ? 4u : 2u;
    if (speech) {
        s->high_run++;
        s->low_run = 0;
        if (!aura_nn_flag(self) && s->high_run >= need_high) {
            aura_nn_report(self, true);
        }
    } else {
        s->low_run++;
        s->high_run = 0;
        if (aura_nn_flag(self) && s->low_run >= 5) {
            aura_nn_report(self, false);
        }
    }
    aura_nn_window_consume(&s->win);
    AURA_LOGT(TAG, "silero prob=%.3f -> %d", (double)prob, (int)aura_nn_flag(self));
    return AURA_OK;
}

/* FLUSH/RESET：清窗口与 LSTM 递推状态（判定状态由适配层一并清）。 */
static aura_err_t silero_algo_reset(aura_nn_node_t *self)
{
    silero_state_t *s = aura_nn_state(self);
    aura_nn_window_reset(&s->win);
    memset(s->hc, 0, sizeof(s->hc));
    s->playing  = false;
    s->high_run = 0;
    s->low_run  = 0;
    return AURA_OK;
}

/* 播放期间抬高判定门槛（软策略：只改滞回所需的连续窗数，不碰模型本身）。 */
static void silero_on_event(aura_nn_node_t *self, const aura_event_t *ev)
{
    silero_state_t *s = aura_nn_state(self);
    switch (ev->type) {
    case AURA_EVENT_TTS_FIRST_CHUNK:
    case AURA_EVENT_PLAYBACK_START:
        s->playing  = true;
        s->high_run = 0; /* 门槛刚抬高：之前攒的命中数不算数 */
        break;
    case AURA_EVENT_TTS_DONE:
    case AURA_EVENT_PLAYBACK_STOP:
        s->playing = false;
        break;
    default:
        break;
    }
}

/* 参数表：声明本算法认哪些 extra 键。声明之后 chain_build 才会接受
 * `chain_param_silero_vad.<键>`，且拼错的键会报错而不是静默失效。 */
static const aura_algo_param_spec_t SILERO_SPECS[] = {
    {"threshold", AURA_ALGO_PARAM_F32, 0.0f, 1.0f, "语音判定阈值"},
};

static aura_nn_desc_t g_silero_desc = {
    .name               = "silero_vad",
    .kind               = AURA_ALGO_KIND_NN_SYNC,
    .io_kind            = AURA_NN_IO_PASSTHROUGH, /* 判定走事件，音频直通 */
    .form               = AURA_INFER_FORM_MODULE, /* 含 If 子图：必须走 Module */
    .model_file         = "vad/silero_vad.mnn/silero_vad.mnn",
    .wants_events       = true,
    .private_group      = true, /* 与其他 NN 节点并发时各自独立推理组：
                                 * 默认组 + 私有组混跑在 MNN 侧实测段错误，
                                 * 全链统一私有组（见 voice_chain 测试） */
    .state_size         = sizeof(silero_state_t),
    .algo_init          = silero_algo_init,
    .frame_in           = silero_frame_in,
    .infer_out          = silero_infer_out,
    .algo_reset         = silero_algo_reset,
    .on_event           = silero_on_event,
    .flag_rise_type     = AURA_EVENT_VAD_SPEECH_START,
    .flag_fall_type     = AURA_EVENT_VAD_SPEECH_END,
    .param_specs        = SILERO_SPECS,
    .param_spec_count   = sizeof(SILERO_SPECS) / sizeof(SILERO_SPECS[0]),
};

aura_err_t aura_silero_register(void)
{
    if (aura_algo_find("silero_vad") != NULL) {
        return AURA_OK;
    }
    return aura_nn_register_all(&g_silero_desc, 1);
}
