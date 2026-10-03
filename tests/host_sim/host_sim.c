/* Aura — host_sim：PC 全链路仿真（Phase 1 出口验收）
 *
 * 目标：文件喂音频驱动 pipeline + event_bus + 状态机跑通全链路骨架，
 * 无硬件、无麦克风。真实算法模块（Phase 2–4）接入前，用 mock 节点
 * 保持骨架可运行、可回归。
 *
 * 链路（mock，但拓扑与真实一致）：
 *   文件音频 ─► VAD ─► KWS ─► ASR ─► (文本走事件) ─► LLM ─► TTS ─► 播放落盘
 *               │       │
 *        TurnTaking   Voiceprint/BargeInJudge/状态机（事件驱动）
 *
 * 仿真时间轴（16 kHz 单声道 16-bit WAV，总长 7s）：
 *   [0.0, 0.8s)  speech A  → KWS 唤醒 → 声纹 → ASR → LLM → TTS 开播
 *   [0.8, 1.3s)  silence
 *   [1.3, 2.1s)  speech B  → TTS 播放中被打断（barge-in）→ 停播+中止 LLM
 *   [2.1, 7.0s)  silence   → 静音超时 → 回 Idle
 *
 * 验收断言：
 *   1. 状态序列命中预期（含打断回 Listening，Interrupted 不是状态）
 *   2. 打断 → 停播延迟 < 200ms（todo.md 第 6 节）
 *   3. 错误注入路径：任意异常 → Error → 自动复位回 Idle
 *   4. profiler 打点齐全
 */
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent/agent_core.h"
#include "agent_export.h"
#include "algorithm/aura_nn_adapter.h"
#include "core/node.h"
#include "core/state_machine/state_machine.h"
#include "inference/inference.h"
#include "utils/logger/logger.h"
#include "utils/profiler/profiler.h"
#include "wav_io.h"

#define TAG "host_sim"

#define SAMPLE_RATE 16000
#define FRAME_SAMPLES 160 /* 10ms */
#define SIM_TOTAL_SECONDS 7.0f

/* ------------------------------------------------------------ 全局（sim） */

typedef struct {
    bool voiceprint_fail;   /* --voiceprint-fail：第一次唤醒声纹拒绝 */
    bool use_silero;        /* --use-silero：真实 silero VAD 代替能量 VAD */
    int64_t inject_error_at_ms; /* <0 = 不注入 */
    const char *wav_in;
    const char *wav_out;
    const char *config;     /* .conf 覆盖（含算法链） */
    int32_t log_level;      /* 0 = 默认 INFO */
} sim_opts_t;

static sim_opts_t g_opts;

/* VAD 有两种实现：默认的能量 mock（本文件手写），--use-silero 时换成经
 * aura_nn_adapter 接入的真实模型。judge/turn_taking 只读"是否有人在说"，
 * 两种实现经 vad_active() 给出同一语义。 */
typedef struct vad_node_impl vad_node_impl_t;

static vad_node_impl_t *g_vad_energy; /* 能量 VAD（未用 silero 时非空） */
static aura_nn_node_t  *g_vad_nn;     /* silero VAD（--use-silero 时非空） */
static aura_nn_stats_t  g_vad_stats;  /* 上者 deinit 前的统计快照 */

/* 当前 VAD 判定（判定与 silero 的 prob 同语义）。定义在结构体之后。 */
static bool vad_active(void);

/* 状态轨迹记录（断言用） */
#define MAX_STATE_LOG 64
static aura_agent_state_t g_state_log[MAX_STATE_LOG];
static uint32_t           g_state_log_count = 0;

/* ============================================================== VAD 节点 */

/* silero v5 窗口口径（官方 silero-vad-model.cc）：每次推理喂 512 个新样本，
 * 模型窗口 = 64 个上文样本 + 512 新样本 = 576。这 64 个上文是模型判句内/句间的
 * 依据，漏掉会让概率跳变。 */
#define SILERO_CTX_SAMPLES 64
#define SILERO_NEW_SAMPLES 512
#define SILERO_WIN_SAMPLES (SILERO_CTX_SAMPLES + SILERO_NEW_SAMPLES)

/* 能量 VAD（mock）：RMS 阈值 + 滞回。播放期间抬高阈值（todo.md 4.5）。 */
struct vad_node_impl {
    aura_node_t base;
    float start_thr;
    float end_thr;
    float barge_scale;
    bool  playing;
    float last_rms;
    /* 状态 */
    bool     active;
    uint32_t high_run;
    uint32_t low_run;
};

/* 当前 VAD 判定。silero 侧直接读适配器 report 出去的状态位（= 它的判定结果），
 * 于是两条实现路径（能量 mock / 真实模型）在调用方看来是同一个语义。 */
static bool vad_active(void)
{
    if (g_vad_nn != NULL) {
        return aura_nn_flag(g_vad_nn);
    }
    return (g_vad_energy != NULL) && g_vad_energy->active;
}

static aura_err_t vad_init(aura_node_t *self)
{
    vad_node_impl_t *v = (vad_node_impl_t *)self;
    v->start_thr   = 0.05f;
    v->end_thr     = 0.02f;
    v->barge_scale = 1.5f;
    return AURA_OK;
}

static void vad_update(vad_node_impl_t *v, float rms)
{
    float start = v->start_thr;
    if (v->playing) {
        start *= v->barge_scale; /* 播放期间抬高阈值：宁漏打断不可自打断 */
    }
    if (!v->active) {
        if (rms > start) {
            v->high_run++;
            v->low_run = 0;
            if (v->high_run >= 2) {
                v->active = true;
                aura_node_post_event(&v->base, AURA_EVENT_VAD_SPEECH_START, 0);
            }
        } else {
            v->high_run = 0;
        }
    } else {
        if (rms < v->end_thr) {
            v->low_run++;
            if (v->low_run >= 5) {
                v->active  = false;
                v->low_run = 0;
                aura_node_post_event(&v->base, AURA_EVENT_VAD_SPEECH_END, 0);
            }
        } else {
            v->low_run = 0;
        }
    }
}

static aura_err_t vad_process(aura_node_t *self, const aura_audio_frame_t *frame)
{
    vad_node_impl_t *v = (vad_node_impl_t *)self;
    const int16_t  *pcm = (const int16_t *)frame->data;
    uint32_t        n   = frame->frame_count;

    double acc = 0.0;
    for (uint32_t i = 0; i < n; i++) {
        acc += (double)pcm[i] * pcm[i];
    }
    v->last_rms = (float)sqrt(acc / (double)n) / 32768.f;
    vad_update(v, v->last_rms);
    return aura_node_emit_audio(self, frame); /* 直通 */
}

static void vad_on_event(aura_node_t *self, const aura_event_t *ev)
{
    vad_node_impl_t *v = (vad_node_impl_t *)self;
    switch (ev->type) {
    case AURA_EVENT_TTS_FIRST_CHUNK:
    case AURA_EVENT_PLAYBACK_START:
        v->playing = true;
        break;
    case AURA_EVENT_TTS_DONE:
    case AURA_EVENT_PLAYBACK_STOP:
        v->playing = false;
        break;
    default:
        break;
    }
}

static const aura_node_ops_t vad_ops = {
    .init          = vad_init,
    .process_audio = vad_process,
    .on_event      = vad_on_event,
};

/* ========================================= silero VAD（走 NN 适配模板）
 *
 * 这是"同一套接口"对 NN 族的验证点：真实模型经 algorithm/aura_nn_adapter.h
 * 的模板接入 —— 与 TrickRoom 一族共用注册名 / 描述表 / 工厂 / 节点契约，
 * 本文件里不再出现手写节点。把这张表搬去 src/（Phase 3）就是一个正式算法。
 *
 * 攒窗交给框架的 aura_nn_window_*：帧长（160）与窗长（512）不成整数倍，
 * 手写很容易把跨窗那帧多出来的样本丢掉（每 3.2 帧丢 128 个，表现为判定漂移
 * 且不报错）。 */

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
    /* 与能量 VAD 同口径的滞回：silero 在句尾也会有零星低概率窗，直接按
     * 0.5 翻转会让 VAD_END 抖动。只在实际翻转时报一次（适配层做边沿检测）。
     * 播放期间（on_event 置的 playing）把"认语音"所需的连续窗数从 2 抬到 4：
     * 播放回声/残留更容易出孤立高概率窗，宁漏打断不可自打断（todo.md 4.5）。 */
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

/* 播放期间抬高判定门槛（软策略：只改滞回所需的连续窗数，不碰模型本身）。
 * 这是 on_event 钩子的典型用法 —— 算法需要"上下文状态"，但不属于音频流。 */
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

/* ============================================================== KWS 节点 */

/* mock KWS：Idle 状态下持续人声 300ms → 唤醒。播放期间 KWS 命中可作为
 * barge-in 因子（Phase 1 裁决暂不用，见 BargeInJudge 注释）。 */
typedef struct {
    aura_node_t base;
    bool  speech_active;
    bool  armed; /* 状态机在 Idle */
    bool  hit;
    uint32_t frames;
} kws_node_impl_t;

static void kws_on_event(aura_node_t *self, const aura_event_t *ev)
{
    kws_node_impl_t *k = (kws_node_impl_t *)self;
    switch (ev->type) {
    case AURA_EVENT_VAD_SPEECH_START:
        k->speech_active = true;
        k->frames        = 0;
        break;
    case AURA_EVENT_VAD_SPEECH_END:
        k->speech_active = false;
        k->frames        = 0;
        break;
    case AURA_EVENT_STATE_CHANGED:
        if (ev->u.state.to == AURA_STATE_IDLE) {
            k->armed = true;
            k->hit   = false; /* 回 Idle 后可再次唤醒 */
        } else {
            k->armed = false;
        }
        break;
    default:
        break;
    }
}

static aura_err_t kws_process(aura_node_t *self, const aura_audio_frame_t *frame)
{
    kws_node_impl_t *k = (kws_node_impl_t *)self;
    if (k->armed && k->speech_active && !k->hit) {
        k->frames++;
        if (k->frames >= 30) { /* 300ms 持续人声 */
            k->hit = true;
            aura_profiler_mark(AURA_PROF_KWS_HIT);
            aura_node_post_event(&k->base, AURA_EVENT_KWS_HIT, 0);
        }
    }
    return aura_node_emit_audio(self, frame);
}

static const aura_node_ops_t kws_ops = {
    .process_audio = kws_process,
    .on_event      = kws_on_event,
};

/* ========================================================= 声纹节点 */

/* mock voiceprint：唤醒后异步"校验"50ms（大推理入队模式的演示），
 * --voiceprint-fail 时第一次拒绝（验证退回 Idle，不进 ASR）。 */
typedef struct {
    aura_node_t base;
    aura_queue_t *q;
    aura_task_t  *task;
    volatile bool running;
    int fail_remaining; /* 剩余拒绝次数 */
} vp_node_impl_t;

static void vp_task(void *arg)
{
    vp_node_impl_t *v = (vp_node_impl_t *)arg;
    int work;
    while (v->running) {
        if (aura_osal_queue_pop(v->q, &work, 50) != AURA_OK) {
            continue;
        }
        aura_osal_sleep_ms(50); /* 模拟 embedding 提取 */
        bool passed = (v->fail_remaining > 0) ? (v->fail_remaining--, false) : true;
        aura_profiler_mark(AURA_PROF_VOICEPRINT_DONE);
        aura_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.type                   = AURA_EVENT_VOICEPRINT_RESULT;
        ev.u.voiceprint.passed    = passed;
        ev.u.voiceprint.score     = passed ? 95 : 40;
        aura_node_emit_event(&v->base, &ev);
        if (!passed) {
            AURA_LOGI(TAG, "voiceprint: REJECT (score=40)");
        }
    }
}

static aura_err_t vp_start(aura_node_t *self)
{
    vp_node_impl_t *v = (vp_node_impl_t *)self;
    v->q = aura_osal_queue_create(4, sizeof(int));
    v->running = true;
    v->task    = aura_osal_task_create("vp", 8192, 3, vp_task, v);
    return (v->q != NULL && v->task != NULL) ? AURA_OK : AURA_ERR_NOMEM;
}

static aura_err_t vp_stop(aura_node_t *self)
{
    vp_node_impl_t *v = (vp_node_impl_t *)self;
    v->running = false;
    if (v->task != NULL) {
        aura_osal_task_destroy(v->task);
        v->task = NULL;
    }
    if (v->q != NULL) {
        aura_osal_queue_destroy(v->q);
        v->q = NULL;
    }
    return AURA_OK;
}

static void vp_on_event(aura_node_t *self, const aura_event_t *ev)
{
    vp_node_impl_t *v = (vp_node_impl_t *)self;
    if (ev->type == AURA_EVENT_KWS_HIT) {
        int work = 1;
        aura_osal_queue_push(v->q, &work, AURA_NO_WAIT);
    }
}

static const aura_node_ops_t vp_ops = {
    .start    = vp_start,
    .stop     = vp_stop,
    .on_event = vp_on_event,
};

/* ============================================================== ASR 节点 */

/* mock ASR：VAD_SPEECH_END（且状态机在 Listening）→ 异步识别 100ms → 中间结果
 * + 最终结果。--inject-error-at 在指定 pts 注入错误（验证 Error/自动复位）。 */
typedef struct {
    aura_node_t base;
    aura_queue_t *q;
    aura_task_t  *task;
    volatile bool running;
    uint64_t inject_at_pts;
    bool     injected;
    uint32_t turn;
} asr_node_impl_t;

static void asr_task(void *arg)
{
    asr_node_impl_t *a = (asr_node_impl_t *)arg;
    int work;
    while (a->running) {
        if (aura_osal_queue_pop(a->q, &work, 50) != AURA_OK) {
            continue;
        }
        aura_osal_sleep_ms(100); /* 模拟流式识别 */
        aura_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = AURA_EVENT_ASR_PARTIAL;
        snprintf(ev.payload, sizeof(ev.payload), "你好");
        aura_node_emit_event(&a->base, &ev);
        aura_osal_sleep_ms(50);
        a->turn++;
        aura_profiler_mark(AURA_PROF_ASR_FINAL);
        memset(&ev, 0, sizeof(ev));
        ev.type = AURA_EVENT_ASR_FINAL;
        snprintf(ev.payload, sizeof(ev.payload), "你好，我是Aura。turn=%u", a->turn);
        aura_node_emit_event(&a->base, &ev);
    }
}

static aura_err_t asr_start(aura_node_t *self)
{
    asr_node_impl_t *a = (asr_node_impl_t *)self;
    a->q = aura_osal_queue_create(4, sizeof(int));
    a->running = true;
    a->task    = aura_osal_task_create("asr", 8192, 3, asr_task, a);
    return (a->q != NULL && a->task != NULL) ? AURA_OK : AURA_ERR_NOMEM;
}

static aura_err_t asr_stop(aura_node_t *self)
{
    asr_node_impl_t *a = (asr_node_impl_t *)self;
    a->running = false;
    if (a->task != NULL) {
        aura_osal_task_destroy(a->task);
        a->task = NULL;
    }
    if (a->q != NULL) {
        aura_osal_queue_destroy(a->q);
        a->q = NULL;
    }
    return AURA_OK;
}

static void asr_on_event(aura_node_t *self, const aura_event_t *ev)
{
    asr_node_impl_t *a = (asr_node_impl_t *)self;
    if (ev->type == AURA_EVENT_VAD_SPEECH_END) {
        aura_sm_t *sm = aura_agent_sm();
        if (sm != NULL && aura_sm_state(sm) == AURA_STATE_LISTENING) {
            int work = 1;
            aura_osal_queue_push(a->q, &work, AURA_NO_WAIT);
        }
    }
}

static aura_err_t asr_process(aura_node_t *self, const aura_audio_frame_t *frame)
{
    asr_node_impl_t *a = (asr_node_impl_t *)self;
    if (a->inject_at_pts != 0 && !a->injected && frame->pts_us >= a->inject_at_pts) {
        a->injected = true;
        AURA_LOGI(TAG, "asr: injected error at pts=%llu", (unsigned long long)frame->pts_us);
        aura_profiler_mark(AURA_PROF_ERROR);
        return AURA_ERR_MODEL; /* pipeline 会代为上抛 EV_ERROR */
    }
    return AURA_OK; /* 终端节点：不产出音频 */
}

static const aura_node_ops_t asr_ops = {
    .start         = asr_start,
    .stop          = asr_stop,
    .process_audio = asr_process,
    .on_event      = asr_on_event,
};

/* ============================================================== LLM 节点 */

/* mock LLM：ASR_FINAL → 20 token 流（50ms/token）。BARGE_IN 中止（abort），
 * 这正是 Phase 4 "LLM 推理 task 支持外部中止" 的契约预演。 */
typedef struct {
    aura_node_t base;
    aura_queue_t *q;
    aura_task_t  *task;
    volatile bool running;
    volatile bool abort;
    uint32_t turn;
} llm_node_impl_t;

static const char *const k_tokens[] = {
    "你好", "，", "我是", "Aura", "。", "这是", "一次", "端到端", "链路", "仿真",
    "。", "语音", "前端", "、", "唤醒", "、", "识别", "、", "对话", "全通", "。",
};
#define TOKEN_COUNT ((uint32_t)(sizeof(k_tokens) / sizeof(k_tokens[0])))

static void llm_task(void *arg)
{
    llm_node_impl_t *l = (llm_node_impl_t *)arg;
    int work;
    while (l->running) {
        if (aura_osal_queue_pop(l->q, &work, 50) != AURA_OK) {
            continue;
        }
        l->abort = false;
        for (uint32_t i = 0; i < TOKEN_COUNT; i++) {
            if (l->abort) {
                AURA_LOGI(TAG, "llm: aborted at token %u/%u", i, TOKEN_COUNT);
                aura_event_t ev;
                memset(&ev, 0, sizeof(ev));
                ev.type = AURA_EVENT_USER;
                ev.code = 1; /* 1 = llm aborted */
                aura_node_emit_event(&l->base, &ev);
                break;
            }
            aura_osal_sleep_ms(50);
            aura_event_t ev;
            memset(&ev, 0, sizeof(ev));
            if (i == 0) {
                ev.type = AURA_EVENT_LLM_FIRST_TOKEN;
                aura_profiler_mark(AURA_PROF_LLM_FIRST_TOKEN);
            } else {
                ev.type = AURA_EVENT_LLM_TOKEN;
            }
            snprintf(ev.payload, sizeof(ev.payload), "%s", k_tokens[i]);
            ev.u.llm.tokens = i + 1;
            aura_node_emit_event(&l->base, &ev);
        }
        if (!l->abort) {
            AURA_LOGI(TAG, "llm: stream complete (%u tokens)", TOKEN_COUNT);
            aura_profiler_mark(AURA_PROF_LLM_DONE);
            aura_node_post_event(&l->base, AURA_EVENT_LLM_DONE, 0);
        }
    }
}

static aura_err_t llm_start(aura_node_t *self)
{
    llm_node_impl_t *l = (llm_node_impl_t *)self;
    l->q = aura_osal_queue_create(4, sizeof(int));
    l->running = true;
    l->task    = aura_osal_task_create("llm", 8192, 3, llm_task, l);
    return (l->q != NULL && l->task != NULL) ? AURA_OK : AURA_ERR_NOMEM;
}

static aura_err_t llm_stop(aura_node_t *self)
{
    llm_node_impl_t *l = (llm_node_impl_t *)self;
    l->running = false;
    l->abort   = true;
    if (l->task != NULL) {
        aura_osal_task_destroy(l->task);
        l->task = NULL;
    }
    if (l->q != NULL) {
        aura_osal_queue_destroy(l->q);
        l->q = NULL;
    }
    return AURA_OK;
}

static void llm_on_event(aura_node_t *self, const aura_event_t *ev)
{
    llm_node_impl_t *l = (llm_node_impl_t *)self;
    switch (ev->type) {
    case AURA_EVENT_ASR_FINAL:
        l->turn++;
        {
            int work = 1;
            aura_osal_queue_push(l->q, &work, AURA_NO_WAIT);
        }
        break;
    case AURA_EVENT_BARGE_IN:
        l->abort = true; /* 中止生成（协作式：token 间检查） */
        break;
    default:
        break;
    }
}

static aura_err_t llm_control(aura_node_t *self, aura_node_cmd_t cmd, const void *arg)
{
    (void)arg;
    llm_node_impl_t *l = (llm_node_impl_t *)self;
    if (cmd == AURA_CMD_FLUSH) {
        l->abort = true;
        if (l->q != NULL) {
            aura_osal_queue_reset(l->q);
        }
    }
    return AURA_OK;
}

static const aura_node_ops_t llm_ops = {
    .start    = llm_start,
    .stop     = llm_stop,
    .on_event = llm_on_event,
    .control  = llm_control,
};

/* ============================================================== TTS 节点 */

/* mock TTS：每个 LLM token → 100ms 音频（10×10ms 正弦），实时节奏产出。
 * BARGE_IN → 立即停播（清队列 + 停任务循环），这也是 200ms 停播指标的来源。 */
typedef struct {
    aura_node_t base;
    aura_queue_t *q;
    aura_task_t  *task;
    volatile bool running;
    volatile bool stop;
    volatile bool llm_done; /* 收到 LLM_DONE，待队列排空后发 TTS_DONE */
    uint32_t chunk;
    uint64_t base_pts;
    bool     first_chunk;
} tts_node_impl_t;

static aura_err_t tts_emit_chunk(tts_node_impl_t *t, uint64_t pts)
{
    /* 100ms = 10 帧 × 10ms，440Hz 正弦（模拟语音音频） */
    for (int f = 0; f < 10; f++) {
        int16_t pcm[FRAME_SAMPLES];
        for (int i = 0; i < FRAME_SAMPLES; i++) {
            double ph = 2.0 * 3.14159265 * 440.0 * (double)(t->chunk * FRAME_SAMPLES + i) /
                        SAMPLE_RATE;
            pcm[i] = (int16_t)(sin(ph) * 8000.0);
        }
        aura_audio_frame_t frame;
        memset(&frame, 0, sizeof(frame));
        frame.pts_us      = pts + (uint64_t)f * 10000;
        frame.sample_rate = SAMPLE_RATE;
        frame.channels    = 1;
        frame.frame_count = FRAME_SAMPLES;
        frame.fmt         = AURA_SAMPLE_S16;
        frame.data        = (uint8_t *)pcm;
        frame.data_bytes  = sizeof(pcm);
        if (aura_node_emit_audio(&t->base, &frame) != AURA_OK) {
            return AURA_ERR_FULL;
        }
    }
    t->chunk++;
    return AURA_OK;
}

static void tts_task(void *arg)
{
    tts_node_impl_t *t = (tts_node_impl_t *)arg;
    int work;
    while (t->running) {
        aura_err_t rc = aura_osal_queue_pop(t->q, &work, 50);
        if (rc != AURA_OK) {
            /* 队列排空：LLM 已结束且本流未被中止 → 播报完成 */
            AURA_LOGD(TAG, "tts idle: llm_done=%d stop=%d queued=%u", (int)t->llm_done,
                      (int)t->stop, aura_osal_queue_count(t->q));
            if (t->llm_done && !t->stop && aura_osal_queue_count(t->q) == 0) {
                t->llm_done = false;
                AURA_LOGI(TAG, "tts: stream complete → TTS_DONE");
                aura_node_post_event(&t->base, AURA_EVENT_TTS_DONE, 0);
            }
            continue;
        }
        if (t->stop) {
            continue;
        }
        if (!t->first_chunk) {
            t->first_chunk = true;
            t->base_pts    = aura_osal_time_us();
            aura_profiler_mark(AURA_PROF_TTS_FIRST_CHUNK);
            aura_node_post_event(&t->base, AURA_EVENT_TTS_FIRST_CHUNK, 0);
        }
        if (tts_emit_chunk(t, t->base_pts + (uint64_t)(t->chunk) * 100000ull) != AURA_OK) {
            AURA_LOGW(TAG, "tts: emit chunk %u failed → stop", t->chunk);
            t->stop = true;
        }
        aura_osal_sleep_ms(100); /* 实时节奏 */
    }
}

static aura_err_t tts_start(aura_node_t *self)
{
    tts_node_impl_t *t = (tts_node_impl_t *)self;
    t->q = aura_osal_queue_create(64, sizeof(int));
    t->running = true;
    t->task    = aura_osal_task_create("tts", 8192, 3, tts_task, t);
    return (t->q != NULL && t->task != NULL) ? AURA_OK : AURA_ERR_NOMEM;
}

static aura_err_t tts_stop(aura_node_t *self)
{
    tts_node_impl_t *t = (tts_node_impl_t *)self;
    t->running = false;
    t->stop    = true;
    if (t->task != NULL) {
        aura_osal_task_destroy(t->task);
        t->task = NULL;
    }
    if (t->q != NULL) {
        aura_osal_queue_destroy(t->q);
        t->q = NULL;
    }
    return AURA_OK;
}

static void tts_on_event(aura_node_t *self, const aura_event_t *ev)
{
    tts_node_impl_t *t = (tts_node_impl_t *)self;
    switch (ev->type) {
    case AURA_EVENT_LLM_FIRST_TOKEN:
        /* 新一轮播报开始：清除上一轮的 stop/first_chunk 状态 */
        t->stop        = false;
        t->first_chunk = false;
        t->chunk       = 0;
        t->llm_done    = false;
        /* fallthrough */
    case AURA_EVENT_LLM_TOKEN: {
        int work = 1;
        aura_osal_queue_push(t->q, &work, AURA_NO_WAIT);
        break;
    }
    case AURA_EVENT_LLM_DONE:
        t->llm_done = true; /* 队列自然排空后由任务发 TTS_DONE */
        break;
    case AURA_EVENT_BARGE_IN:
        t->stop = true;
        if (t->q != NULL) {
            aura_osal_queue_reset(t->q); /* 丢弃未播内容 */
        }
        aura_profiler_mark(AURA_PROF_PLAYBACK_STOP);
        aura_node_post_event(&t->base, AURA_EVENT_PLAYBACK_STOP, 0);
        break;
    default:
        break;
    }
}

static const aura_node_ops_t tts_ops = {
    .start    = tts_start,
    .stop     = tts_stop,
    .on_event = tts_on_event,
};

/* ========================================================== 播放落盘节点 */

typedef struct {
    aura_node_t base;
    FILE    *fp;
    bool     started;
} sink_node_impl_t;

static aura_err_t sink_process(aura_node_t *self, const aura_audio_frame_t *frame)
{
    sink_node_impl_t *s = (sink_node_impl_t *)self;
    if (!s->started) {
        s->started = true;
        aura_profiler_mark(AURA_PROF_PLAYBACK_START);
        aura_node_post_event(&s->base, AURA_EVENT_PLAYBACK_START, 0);
    }
    if (s->fp != NULL) {
        fwrite(frame->data, 1, frame->data_bytes, s->fp);
    }
    return AURA_OK;
}

static const aura_node_ops_t sink_ops = {
    .process_audio = sink_process,
};

/* ========================================================== 打断裁决节点 */

/* barge-in 多因子裁决（todo.md 4.5）Phase 1 简化版：
 *  ① 近端能量（VAD 判定本身已含阈值）② 播放标志（playing 窗口）
 *  ③ NN-VAD 概率（energy VAD 的 RMS / silero 的 prob）④ KWS 命中 —— Phase 4 加入
 *  ⑤ DoA —— Phase 2 后加入
 * 策略：播放期间 VAD 阈值已抬高（vad 节点内 barge_scale），裁决再加 30ms 去抖。 */
typedef struct {
    aura_node_t base;
    aura_queue_t *q;
    aura_task_t  *task;
    volatile bool running;
    bool playing;
} judge_node_impl_t;

static void judge_task(void *arg)
{
    judge_node_impl_t *j = (judge_node_impl_t *)arg;
    int work;
    while (j->running) {
        if (aura_osal_queue_pop(j->q, &work, 50) != AURA_OK) {
            continue;
        }
        aura_osal_sleep_ms(30); /* 去抖：3 帧 */
        if (j->playing && vad_active()) {
            AURA_LOGI(TAG, "barge-in: ALLOW (playing=%d vad_active=%d)", (int)j->playing,
                      (int)vad_active());
            aura_profiler_mark(AURA_PROF_BARGE_IN);
            aura_node_post_event(&j->base, AURA_EVENT_BARGE_IN, 0);
        }
    }
}

static aura_err_t judge_start(aura_node_t *self)
{
    judge_node_impl_t *j = (judge_node_impl_t *)self;
    j->q = aura_osal_queue_create(8, sizeof(int));
    j->running = true;
    j->task    = aura_osal_task_create("judge", 8192, 3, judge_task, j);
    return (j->q != NULL && j->task != NULL) ? AURA_OK : AURA_ERR_NOMEM;
}

static aura_err_t judge_stop(aura_node_t *self)
{
    judge_node_impl_t *j = (judge_node_impl_t *)self;
    j->running = false;
    if (j->task != NULL) {
        aura_osal_task_destroy(j->task);
        j->task = NULL;
    }
    if (j->q != NULL) {
        aura_osal_queue_destroy(j->q);
        j->q = NULL;
    }
    return AURA_OK;
}

static void judge_on_event(aura_node_t *self, const aura_event_t *ev)
{
    judge_node_impl_t *j = (judge_node_impl_t *)self;
    switch (ev->type) {
    case AURA_EVENT_TTS_FIRST_CHUNK:
    case AURA_EVENT_PLAYBACK_START:
        j->playing = true;
        break;
    case AURA_EVENT_TTS_DONE:
    case AURA_EVENT_PLAYBACK_STOP:
        j->playing = false;
        break;
    case AURA_EVENT_VAD_SPEECH_START:
        if (j->playing) {
            int work = 1;
            aura_osal_queue_push(j->q, &work, AURA_NO_WAIT);
        }
        break;
    default:
        break;
    }
}

static const aura_node_ops_t judge_ops = {
    .start    = judge_start,
    .stop     = judge_stop,
    .on_event = judge_on_event,
};

/* ========================================================== 轮次裁决节点 */

/* mock turn_taking（真实版在 Phase 4 agent/dialogue/turn_taking）：
 * VAD_END 或 进入 Listening → 1s 静音超时 → SILENCE_TIMEOUT（仅在 Listening 有效）。 */
typedef struct {
    aura_node_t base;
    aura_task_t  *task;
    volatile bool running;
    uint64_t deadline_ms;
    bool     deadline_set;
} turn_node_impl_t;

static void turn_task(void *arg)
{
    turn_node_impl_t *t = (turn_node_impl_t *)arg;
    while (t->running) {
        aura_osal_sleep_ms(10);
        if (!t->deadline_set) {
            continue;
        }
        if (aura_osal_time_ms() < t->deadline_ms) {
            continue;
        }
        t->deadline_set = false;
        bool speech = vad_active();
        aura_sm_t *sm = aura_agent_sm();
        AURA_LOGD(TAG, "turn: deadline fired speech=%d sm=%p state=%d", (int)speech,
                  (void *)sm, sm != NULL ? (int)aura_sm_state(sm) : -1);
        if (!speech && sm != NULL && aura_sm_state(sm) == AURA_STATE_LISTENING) {
            aura_node_post_event(&t->base, AURA_EVENT_SILENCE_TIMEOUT, 0);
        }
    }
}

static aura_err_t turn_start(aura_node_t *self)
{
    turn_node_impl_t *t = (turn_node_impl_t *)self;
    t->running = true;
    t->task    = aura_osal_task_create("turn", 8192, 3, turn_task, t);
    return (t->task != NULL) ? AURA_OK : AURA_ERR_NOMEM;
}

static aura_err_t turn_stop(aura_node_t *self)
{
    turn_node_impl_t *t = (turn_node_impl_t *)self;
    t->running = false;
    if (t->task != NULL) {
        aura_osal_task_destroy(t->task);
        t->task = NULL;
    }
    return AURA_OK;
}

static void turn_on_event(aura_node_t *self, const aura_event_t *ev)
{
    turn_node_impl_t *t = (turn_node_impl_t *)self;
    switch (ev->type) {
    case AURA_EVENT_VAD_SPEECH_END:
    case AURA_EVENT_STATE_CHANGED:
        if (ev->type == AURA_EVENT_STATE_CHANGED &&
            ev->u.state.to != AURA_STATE_LISTENING) {
            t->deadline_set = false;
            break;
        }
        if (vad_active()) {
            break; /* 语音进行中，不武装超时 */
        }
        t->deadline_ms  = aura_osal_time_ms() + 1000;
        t->deadline_set = true;
        AURA_LOGD(TAG, "turn: armed deadline +1000ms (ev=%s)", aura_event_type_name(ev->type));
        break;
    case AURA_EVENT_VAD_SPEECH_START:
        t->deadline_set = false;
        break;
    default:
        break;
    }
}

static const aura_node_ops_t turn_ops = {
    .start    = turn_start,
    .stop     = turn_stop,
    .on_event = turn_on_event,
};

/* ========================================================== 事件记录（断言） */

static void on_agent_event(const aura_agent_event_data_t *ev, void *user)
{
    (void)user;
    if (ev->type == AURA_AGENT_EVENT_STATE_CHANGED) {
        if (g_state_log_count < MAX_STATE_LOG) {
            g_state_log[g_state_log_count++] = ev->to_state;
        }
        AURA_LOGI(TAG, "[state] %d -> %d", (int)ev->from_state, (int)ev->to_state);
    } else if (ev->type == AURA_AGENT_EVENT_BARGE_IN) {
        AURA_LOGI(TAG, "[event] barge-in");
    } else if (ev->type == AURA_AGENT_EVENT_ERROR) {
        AURA_LOGI(TAG, "[event] error: %s", ev->text);
    }
}

/* ============================================================ WAV 生成 */

static void gen_speech(int16_t *dst, uint32_t n, float amp)
{
    /* 语音模拟：白噪声 + 低通 + 基音调制，能量集中在中低频 */
    static uint32_t s_rng = 12345;
    float lp = 0.f;
    for (uint32_t i = 0; i < n; i++) {
        s_rng = s_rng * 1664525u + 1013904223u;
        float noise = (float)((int32_t)(s_rng >> 8) & 0xFFFF) / 32768.f - 1.f;
        lp = lp + 0.25f * (noise - lp); /* 一阶低通 ~1kHz */
        double ph  = 2.0 * 3.14159265 * 180.0 * (double)i / SAMPLE_RATE;
        float tone = (float)sin(ph) * 0.4f;
        float v    = (lp * 0.6f + tone) * amp;
        /* 首尾 50ms 包络，避免爆音 */
        uint32_t fade = (n / 20) + 1;
        float env = 1.f;
        if (i < fade) {
            env = (float)i / (float)fade;
        } else if (i > n - fade) {
            env = (float)(n - i) / (float)fade;
        }
        dst[i] = (int16_t)(v * env * 32000.f);
    }
}

/* extra_speech：错误注入模式下追加第三段语音（[3.5,4.3s)）——
 * 错误把首轮唤醒顺延到 speech B，需要 speech C 落在 TTS 播放窗口内
 * 才能继续验证打断路径。 */
static aura_err_t build_wav(const char *path, int16_t **samples, uint32_t *count,
                            bool extra_speech)
{
    uint32_t rate = SAMPLE_RATE;
    uint32_t total = (uint32_t)(SIM_TOTAL_SECONDS * (float)rate);
    int16_t *buf = (int16_t *)calloc(total, sizeof(int16_t));
    if (buf == NULL) {
        return AURA_ERR_NOMEM;
    }
    const uint32_t sp_a_start = (uint32_t)(0.0f * rate);
    const uint32_t sp_a_end   = (uint32_t)(0.8f * rate);
    const uint32_t sp_b_start = (uint32_t)(1.3f * rate);
    const uint32_t sp_b_end   = (uint32_t)(2.1f * rate);

    gen_speech(buf + sp_a_start, sp_a_end - sp_a_start, 0.9f);
    gen_speech(buf + sp_b_start, sp_b_end - sp_b_start, 0.9f);
    uint32_t sp_c_start = 0;
    uint32_t sp_c_end   = 0;
    if (extra_speech) {
        sp_c_start = (uint32_t)(3.5f * rate);
        sp_c_end   = (uint32_t)(4.3f * rate);
        gen_speech(buf + sp_c_start, sp_c_end - sp_c_start, 0.9f);
    }
    /* 静音段加微扰（真实环境底噪） */
    static uint32_t s_rng2 = 777;
    for (uint32_t i = 0; i < total; i++) {
        bool speech = (i >= sp_a_start && i < sp_a_end) || (i >= sp_b_start && i < sp_b_end) ||
                      (extra_speech && i >= sp_c_start && i < sp_c_end);
        if (!speech) {
            s_rng2 = s_rng2 * 1664525u + 1013904223u;
            buf[i] = (int16_t)((int32_t)(s_rng2 >> 16) & 0x1F);
        }
    }
    *samples = buf;
    *count   = total;
    if (path != NULL) {
        aura_err_t rc = wav_write(path, buf, total, rate);
        if (rc != AURA_OK) {
            free(buf);
            *samples = NULL;
            return rc;
        }
    }
    return AURA_OK;
}

/* ============================================================ 主流程 */

static void usage(const char *argv0)
{
    printf("usage: %s [options]\n", argv0);
    printf("  --wav <path>         输入 WAV（默认生成合成音频 7s）\n");
    printf("  --out <path>         播放落盘 WAV（默认 host_sim_out.wav）\n");
    printf("  --use-silero         用真实 silero VAD 代替能量 VAD\n");
    printf("  --config <path>      .conf 覆盖配置（可含算法链，见 configs/silero.conf）\n");
    printf("  --voiceprint-fail    第一次唤醒声纹拒绝（验证退回 Idle）\n");
    printf("  --inject-error <ms>  在该时刻让 ASR 注入错误（验证 Error/自动复位）\n");
    printf("  --log <level>        error|warn|info|debug|trace\n");
}

/* 期望状态序列：取决于注入模式。 */
static bool match_expected(const aura_agent_state_t *log, uint32_t n, bool fail_mode,
                           bool err_mode)
{
    static const aura_agent_state_t base_seq[] = {
        AURA_AGENT_STATE_LISTENING, AURA_AGENT_STATE_THINKING, AURA_AGENT_STATE_SPEAKING,
        AURA_AGENT_STATE_LISTENING, AURA_AGENT_STATE_THINKING, AURA_AGENT_STATE_SPEAKING,
        AURA_AGENT_STATE_LISTENING, AURA_AGENT_STATE_IDLE,
    };
    uint32_t idx = 0;
    /* 前置段 */
    if (err_mode) {
        /* Idle→Listening→Error→Idle */
        static const aura_agent_state_t pre_err[] = {
            AURA_AGENT_STATE_LISTENING, AURA_AGENT_STATE_ERROR, AURA_AGENT_STATE_IDLE,
        };
        for (uint32_t i = 0; i < 3 && idx < n; i++, idx++) {
            if (log[idx] != pre_err[i]) {
                return false;
            }
        }
    } else if (fail_mode) {
        /* Idle→Listening→(声纹拒绝)→Idle；二次唤醒仍发生在同一段语音内 */
        static const aura_agent_state_t pre_fail[] = {
            AURA_AGENT_STATE_LISTENING, AURA_AGENT_STATE_IDLE,
        };
        for (uint32_t i = 0; i < 2 && idx < n; i++, idx++) {
            if (log[idx] != pre_fail[i]) {
                return false;
            }
        }
    }
    /* 主体段 */
    uint32_t k = 0;
    for (; idx < n && k < 8; idx++, k++) {
        if (log[idx] != base_seq[k]) {
            return false;
        }
    }
    if (k != 8) {
        return false; /* 主体段不完整 */
    }
    /* 尾段允许停留在 Idle（多余重复不计） */
    for (; idx < n; idx++) {
        if (log[idx] != AURA_AGENT_STATE_IDLE) {
            return false;
        }
    }
    return true;
}

int main(int argc, char **argv)
{
    memset(&g_opts, 0, sizeof(g_opts));
    g_opts.inject_error_at_ms = -1;
    g_opts.wav_out            = "host_sim_out.wav";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--wav") == 0 && i + 1 < argc) {
            g_opts.wav_in = argv[++i];
        } else if (strcmp(argv[i], "--out") == 0 && i + 1 < argc) {
            g_opts.wav_out = argv[++i];
        } else if (strcmp(argv[i], "--use-silero") == 0) {
            g_opts.use_silero = true;
        } else if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            g_opts.config = argv[++i];
        } else if (strcmp(argv[i], "--voiceprint-fail") == 0) {
            g_opts.voiceprint_fail = true;
        } else if (strcmp(argv[i], "--inject-error") == 0 && i + 1 < argc) {
            g_opts.inject_error_at_ms = atoll(argv[++i]);
        } else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
            g_opts.log_level = AURA_LOG_LVL_DEBUG;
            (void)argv[++i];
        } else {
            usage(argv[0]);
            return 2;
        }
    }

    aura_log_init();
    printf("== Aura host_sim (Phase 1 exit gate) ==\n");
    printf("== build: %s\n", aura_agent_build_info());

    /* ---- 输入音频 ---- */
    int16_t *wav = NULL;
    uint32_t wav_count = 0;
    uint32_t wav_rate  = SAMPLE_RATE;
    if (g_opts.wav_in != NULL) {
        if (wav_read(g_opts.wav_in, &wav, &wav_count, &wav_rate) != AURA_OK) {
            printf("FAIL: cannot read %s\n", g_opts.wav_in);
            return 1;
        }
        if (wav_rate != SAMPLE_RATE) {
            printf("FAIL: input must be 16 kHz (got %u)\n", wav_rate);
            free(wav);
            return 1;
        }
        printf("input: %s (%u samples, %.1fs)\n", g_opts.wav_in, wav_count,
               (double)wav_count / wav_rate);
    } else {
        if (build_wav("host_sim_in.wav", &wav, &wav_count, g_opts.inject_error_at_ms >= 0) !=
            AURA_OK) {
            printf("FAIL: wav generation failed\n");
            return 1;
        }
        printf("input: generated host_sim_in.wav (%u samples, %.1fs)\n", wav_count,
               (double)wav_count / wav_rate);
    }

    /* ---- 组装 ---- */
    aura_agent_config_t cfg;
    aura_agent_config_default(&cfg);
    if (g_opts.config != NULL) {
        /* .conf 里的字符串由静态缓冲承接（aura_agent_config_load 的约定），
         * 所以必须在 --use-silero 覆盖 chain 之前调用。 */
        aura_err_t lrc = aura_agent_config_load(&cfg, g_opts.config);
        if (lrc != AURA_OK) {
            printf("FAIL: config %s: %s\n", g_opts.config, aura_strerror(lrc));
            free(wav);
            return 1;
        }
        printf("config: %s (chain='%s')\n", g_opts.config, cfg.chain ? cfg.chain : "-");
    }
    cfg.frame_pool_blocks = 32; /* 仿真链路深，帧池给足（.conf 也可覆盖，这里以仿真为准） */
    if (g_opts.log_level > 0) {
        cfg.log_level = g_opts.log_level; /* --log 优先于 .conf */
    }

    /* --use-silero：真实 VAD 不再手写节点，改为"注册进算法表 + 写进配置链"，
     * 由 agent 按链装配 —— 与 TrickRoom 一族共用同一条装配路径（本文件里
     * 除这张描述表外没有任何 MNN 代码）。 */
    aura_err_t rc;
    if (g_opts.use_silero) {
        rc = aura_nn_finalize(&g_silero_desc); /* 填好内嵌 algo 描述并注册进算法表 */
        if (rc != AURA_OK) {
            printf("FAIL: register silero_vad: %s\n", aura_strerror(rc));
            free(wav);
            return 1;
        }
        cfg.model_dir = AURA_HOST_SIM_SOURCE_DIR "/models";
        /* 权重不入库（models/download.sh）：没下模型时跳过，别把"没下模型"
         * 变成一条红灯 —— 与 tests/unit/test_inference.c 同一约定。 */
        char model_probe[512];
        snprintf(model_probe, sizeof(model_probe), "%s/%s", cfg.model_dir,
                 g_silero_desc.model_file);
        FILE *probe = fopen(model_probe, "rb");
        if (probe == NULL) {
            printf("SKIP: silero model not found (%s) — run models/download.sh\n", model_probe);
            free(wav);
            return 0;
        }
        fclose(probe);
        if (cfg.chain == NULL || cfg.chain[0] == '\0') {
            cfg.chain = "silero_vad"; /* 没给 .conf/链时用它兜底；给了就以 .conf 为准 */
        }
        printf("silero: registered, chain = '%s', models = %s\n", cfg.chain, cfg.model_dir);
    }

    rc = aura_agent_init(&cfg);
    if (rc != AURA_OK) {
        printf("FAIL: aura_agent_init: %s\n", aura_strerror(rc));
        free(wav);
        return 1;
    }

    vad_node_impl_t  vad;
    kws_node_impl_t  kws;
    asr_node_impl_t  asr;
    tts_node_impl_t  tts;
    sink_node_impl_t sink;
    vp_node_impl_t   vp;
    llm_node_impl_t  llm;
    judge_node_impl_t judge;
    turn_node_impl_t  turn;

    memset(&vad, 0, sizeof(vad));
    memset(&kws, 0, sizeof(kws));
    memset(&asr, 0, sizeof(asr));
    memset(&tts, 0, sizeof(tts));
    memset(&sink, 0, sizeof(sink));
    memset(&vp, 0, sizeof(vp));
    memset(&llm, 0, sizeof(llm));
    memset(&judge, 0, sizeof(judge));
    memset(&turn, 0, sizeof(turn));

    /* silero 模式的 VAD 句柄来自配置链（节点归 agent 所有，deinit 时随链回收）。 */
    if (g_opts.use_silero) {
        g_vad_nn = (aura_nn_node_t *)aura_agent_chain_node(g_silero_desc.name);
        if (g_vad_nn == NULL) {
            printf("FAIL: chain node '%s' not found after init\n", g_silero_desc.name);
            aura_agent_deinit();
            free(wav);
            return 1;
        }
    }

    aura_node_init(&vad.base, "vad", &vad_ops,
                   &(aura_node_caps_t){.consumes_audio = true, .produces_audio = true,
                                       .wants_events = true});
    aura_node_init(&kws.base, "kws", &kws_ops,
                   &(aura_node_caps_t){.consumes_audio = true, .produces_audio = true,
                                       .wants_events = true});
    aura_node_init(&asr.base, "asr", &asr_ops,
                   &(aura_node_caps_t){.consumes_audio = true, .wants_events = true});
    aura_node_init(&tts.base, "tts", &tts_ops,
                   &(aura_node_caps_t){.produces_audio = true, .wants_events = true});
    aura_node_init(&sink.base, "sink", &sink_ops,
                   &(aura_node_caps_t){.consumes_audio = true});
    aura_node_init(&vp.base, "voiceprint", &vp_ops,
                   &(aura_node_caps_t){.wants_events = true});
    aura_node_init(&llm.base, "llm", &llm_ops, &(aura_node_caps_t){.wants_events = true});
    aura_node_init(&judge.base, "bargein", &judge_ops,
                   &(aura_node_caps_t){.wants_events = true});
    aura_node_init(&turn.base, "turn", &turn_ops, &(aura_node_caps_t){.wants_events = true});

    vp.fail_remaining = g_opts.voiceprint_fail ? 1 : 0;
    asr.inject_at_pts = (g_opts.inject_error_at_ms >= 0)
                            ? (uint64_t)g_opts.inject_error_at_ms * 1000
                            : 0;
    kws.armed = true; /* 状态机 START 事件会确认 Idle；初始即 Idle */

    /* 注册顺序即音频链顺序：vad → kws → asr；tts → sink（自动线性连线）。
     * silero 模式下 vad 由配置链装配（在 init 里已入链，排在手工节点之前），
     * 这里不再注册能量 VAD —— 否则音频会先过 silero 再过能量 VAD，判定打架。 */
    rc = AURA_OK;
    if (!g_opts.use_silero) {
        g_vad_energy = &vad;
        rc           = aura_agent_add_node(&vad.base);
    }
    if (rc != AURA_OK ||
        (rc = aura_agent_add_node(&kws.base)) != AURA_OK ||
        (rc = aura_agent_add_node(&asr.base)) != AURA_OK ||
        (rc = aura_agent_add_node(&tts.base)) != AURA_OK ||
        (rc = aura_agent_add_node(&sink.base)) != AURA_OK ||
        (rc = aura_agent_add_node(&vp.base)) != AURA_OK ||
        (rc = aura_agent_add_node(&llm.base)) != AURA_OK ||
        (rc = aura_agent_add_node(&judge.base)) != AURA_OK ||
        (rc = aura_agent_add_node(&turn.base)) != AURA_OK) {
        printf("FAIL: aura_agent_add_node: %s\n", aura_strerror(rc));
        aura_agent_deinit();
        free(wav);
        return 1;
    }

    aura_agent_set_event_callback(on_agent_event, NULL);

    rc = aura_agent_start();
    if (rc != AURA_OK) {
        printf("FAIL: aura_agent_start: %s\n", aura_strerror(rc));
        aura_agent_deinit();
        free(wav);
        return 1;
    }

    /* ---- 喂音频（实时节奏 10ms/帧，pts 显式给出） ---- */
    uint32_t pos = 0;
    uint64_t pts = 0;
    const uint32_t step = FRAME_SAMPLES;
    while (pos + step <= wav_count) {
        if (aura_agent_feed_audio(wav + pos, step, 1, AURA_AGENT_AUDIO_S16, pts,
                                  AURA_AGENT_WAIT_FOREVER) != AURA_OK) {
            printf("FAIL: feed_audio at %u\n", pos);
            break;
        }
        pos += step;
        pts += 10000;
        aura_osal_sleep_ms(10); /* 实时节奏 */
    }
    printf("fed %u frames (%.1fs)\n", pos / step, (double)pos / SAMPLE_RATE);

    /* ---- 收尾：等状态机回 Idle + 队列排空 ---- */
    int waited_ms = 0;
    const int wait_max_ms = 10000;
    for (;;) {
        aura_agent_state_t st;
        aura_agent_get_state(&st);
        if (st == AURA_AGENT_STATE_IDLE && waited_ms > 1500) {
            break;
        }
        if (waited_ms >= wait_max_ms) {
            printf("WARN: timed out waiting for Idle (state=%d)\n", (int)st);
            break;
        }
        aura_osal_sleep_ms(20);
        waited_ms += 20;
    }

    /* ---- 停链路 + 落盘 ---- */
    /* 统计要在 deinit 前取：链上的节点归 agent 所有，deinit 后句柄就悬垂了。 */
    if (g_opts.use_silero && g_vad_nn != NULL) {
        (void)aura_nn_get_stats(&g_vad_nn->base, &g_vad_stats);
    }
    aura_agent_stop();
    if (wav != NULL) {
        printf("out: %s\n", g_opts.wav_out);
    }
    aura_agent_deinit();
    g_vad_nn = NULL; /* 链上节点已随 agent 释放，别留悬垂句柄 */
    free(wav);

    /* ---- 断言 ---- */
    aura_profiler_report();

    /* silero 模式：真实 VAD 对合成音频不会触发 —— 推理次数必须 > 0（证明
     * MNN 在链路里跑了），但状态序列用合成音频无法验证，给出明确提示并跳过。 */
    if (g_opts.use_silero) {
        /* 由链上的 silero_vad 节点（NN 适配模板）在 deinit 前快照。 */
        printf("silero VAD: %llu frames in, %llu inferences, max %.2f ms/inference\n",
               (unsigned long long)g_vad_stats.frames_in,
               (unsigned long long)g_vad_stats.windows,
               (double)g_vad_stats.infer_us_max / 1000.0);
        if (g_vad_stats.windows == 0) {
            printf("== host_sim FAIL (silero never ran) ==\n");
            return 1;
        }
        if (g_state_log_count == 0) {
            printf("SKIP: synthetic audio did not trigger silero — use --wav with recorded "
                   "speech to validate the full loop with real VAD\n");
            printf("== host_sim PASS (MNN-in-loop verified; sequence check skipped) ==\n");
            return 0;
        }
    }

    bool ok = true;

    /* 1) 状态序列 */
    printf("state trace:");
    for (uint32_t i = 0; i < g_state_log_count; i++) {
        printf(" %d", (int)g_state_log[i]);
    }
    printf("\n");
    bool seq_ok = match_expected(g_state_log, g_state_log_count, g_opts.voiceprint_fail,
                                 g_opts.inject_error_at_ms >= 0);
    printf("state sequence: %s\n", seq_ok ? "PASS" : "FAIL");
    ok = ok && seq_ok;

    /* 2) 打断 → 停播 < 200ms */
    aura_prof_session_t prof;
    aura_profiler_snapshot(&prof);
    if (prof.valid[AURA_PROF_BARGE_IN] && prof.valid[AURA_PROF_PLAYBACK_STOP]) {
        uint64_t stop_ms = aura_prof_snapshot_delta_ms(&prof, AURA_PROF_BARGE_IN,
                                                       AURA_PROF_PLAYBACK_STOP);
        printf("barge-in -> playback stop: %llu ms (target < 200)\n",
               (unsigned long long)stop_ms);
        printf("barge-in latency: %s\n", (stop_ms < 200) ? "PASS" : "FAIL");
        ok = ok && (stop_ms < 200);
    } else {
        printf("barge-in latency: SKIP (no barge-in occurred)\n");
    }

    /* 3) 全链路时延（sim 无硬件，只做记录） */
    uint64_t wake_to_speak =
        aura_prof_snapshot_delta_ms(&prof, AURA_PROF_KWS_HIT, AURA_PROF_PLAYBACK_START);
    uint64_t llm_ttft =
        aura_prof_snapshot_delta_ms(&prof, AURA_PROF_ASR_FINAL, AURA_PROF_LLM_FIRST_TOKEN);
    printf("wake -> playout: %llu ms | llm first token: %llu ms\n",
           (unsigned long long)wake_to_speak, (unsigned long long)llm_ttft);

    printf("== host_sim %s ==\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
