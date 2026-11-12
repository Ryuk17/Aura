/* Aura — soundproof 音频类算法 → pipeline node
 *
 * 覆盖 AEC/AECM/NS/AGC/AGC2/BF/SRC/HS/TS。它们是同步、逐帧、无异步任务，
 * 统一走一个节点模板：
 *   - 1→1：NS/AGC/AGC2/HS/TS
 *   - 2→1：AEC/AECM（近端 + 参考）
 *   - N→1：BF（多麦 → 单波束）
 *   - 变采样率：SRC
 *
 * 与旧 TrickRoom 适配层的区别只在“引擎调用”这一步：现在调用的是 soundproof
 * 的统一 C ABI（AudioEngine_*），而不是 libAE_* 的 C++ 内部实现。
 */
#include "soundproof/ae_common.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "audio_engine_aec.h"
#include "audio_engine_aecm.h"
#include "audio_engine_agc.h"
#include "audio_engine_agc2.h"
#include "audio_engine_bf.h"
#include "audio_engine_hs.h"
#include "audio_engine_ns.h"
#include "audio_engine_src.h"
#include "audio_engine_ts.h"
#include "utils/logger/logger.h"

#define TAG "ae_audio"

/* ============================== 描述表 ============================== */

typedef enum {
    AE_AUDIO_1TO1 = 0,
    AE_AUDIO_2TO1,
    AE_AUDIO_NIN1,
    AE_AUDIO_RESAMPLE,
} ae_audio_kind_t;

typedef struct ae_audio_desc {
    aura_algo_desc_t base; /* 必须首位：通用工厂从 params->provider 取回它 */

    ae_audio_kind_t kind;

    int (*create)(void **h);
    int (*init)(void *h, const aura_algo_params_t *p, int rate, int frame_len, int channels);
    int (*process)(void *h, const int16_t *in, const int16_t *ref, int in_samples, int16_t *out,
                   int max_out, int *out_samples, int *flag);
    int (*reset)(void *h);
    int (*deinit)(void *h);
    int (*destroy)(void *h);
} ae_audio_desc_t;

/* ============================== 节点 ============================== */

typedef struct ae_audio_node {
    aura_node_t base;
    const ae_audio_desc_t *desc;

    void    *h;
    bool     inited;
    uint32_t rate;
    uint32_t rate_out;
    uint32_t frame_len;
    uint32_t in_ch;
    uint32_t out_ch;
    char     name[32];

    /* 工厂在 chain_build 期间保存一份参数副本；DSP 参数全是数值，按值拷贝安全。 */
    aura_algo_params_t params_cache;

    /* 参考帧：pipeline 会先投参考、再投近端（按 pts 配对），这里只暂存一帧。 */
    int16_t ref_buf[AE_MAX_SAMPLES];
    int     ref_len;
    bool    ref_ready;

    int16_t out_buf[AE_MAX_SAMPLES];

    /* 统计/限流 */
    uint64_t frames_in;
    uint64_t ref_missed;
    uint64_t process_errors;
    bool     err_logged;
} ae_audio_node_t;

static void ae_audio_emit(ae_audio_node_t *n, aura_node_t *self, const int16_t *data,
                          uint32_t samples, uint32_t channels)
{
    if (samples == 0) {
        return;
    }
    (void)ae_emit_audio(self, data, samples, n->rate_out, channels);
}

static aura_err_t ae_audio_process_audio(aura_node_t *self, const aura_audio_frame_t *frame)
{
    ae_audio_node_t *n = (ae_audio_node_t *)self;

    if (!n->inited || n->h == NULL) {
        return AURA_ERR_STATE;
    }
    if (!ae_frame_ok(frame, n->in_ch, self->name)) {
        return AURA_ERR_INVALID_ARG;
    }
    if (frame->frame_count != n->frame_len) {
        if (!n->err_logged) {
            n->err_logged = true;
            AURA_LOGW(TAG, "%s: expects %u samples/frame (10ms), got %u", self->name,
                      (unsigned)n->frame_len, (unsigned)frame->frame_count);
        }
        return AURA_ERR_INVALID_ARG;
    }

    n->frames_in++;

    const int16_t *in  = (const int16_t *)frame->data;
    const int16_t *ref = NULL;
    if (n->desc->kind == AE_AUDIO_2TO1) {
        if (n->ref_ready && n->ref_len == (int)frame->frame_count) {
            ref = n->ref_buf;
        } else {
            static const int16_t silence[AE_MAX_SAMPLES] = {0};
            ref = silence;
            n->ref_missed++;
        }
        n->ref_ready = false;
    }

    int out_samples = 0;
    int flag        = 0;
    int st = n->desc->process(n->h, in, ref, (int)frame->frame_count, n->out_buf, AE_MAX_SAMPLES,
                              &out_samples, &flag);
    if (st != AUDIO_ENGINE_SUCCESS) {
        n->process_errors++;
        if (!n->err_logged) {
            n->err_logged = true;
            AURA_LOGW(TAG, "%s: AudioEngine process failed (0x%x), passing input through",
                      self->name, st);
        }
        /* 降级：DSP 出问题时“原声”远好于“静音”。SRC 的输入输出率不同，不能原样透传，
         * 直接报错让 pipeline 计数。 */
        if (n->desc->kind == AE_AUDIO_RESAMPLE) {
            return ae_status_to_err(st, self->name);
        }
        if (n->desc->kind == AE_AUDIO_NIN1) {
            /* BF 输出是单通道：取输入的第 0 通道 */
            int16_t *dst = n->out_buf;
            for (uint32_t i = 0; i < frame->frame_count; i++) {
                dst[i] = in[i * n->in_ch];
            }
            ae_audio_emit(n, self, n->out_buf, frame->frame_count, 1);
        } else {
            ae_audio_emit(n, self, in, frame->frame_count * n->in_ch, n->in_ch);
        }
        return AURA_OK;
    }

    if (out_samples > 0) {
        uint32_t ch = (n->desc->kind == AE_AUDIO_NIN1) ? 1 : n->out_ch;
        ae_audio_emit(n, self, n->out_buf, (uint32_t)out_samples, ch);
    }
    (void)flag;
    return AURA_OK;
}

static aura_err_t ae_audio_process_ref(aura_node_t *self, const aura_audio_frame_t *frame)
{
    ae_audio_node_t *n = (ae_audio_node_t *)self;
    if (n->desc->kind != AE_AUDIO_2TO1) {
        return AURA_OK;
    }
    if (!ae_frame_ok(frame, 1, self->name)) {
        return AURA_ERR_INVALID_ARG;
    }
    if (frame->frame_count != n->frame_len) {
        return AURA_ERR_INVALID_ARG;
    }
    memcpy(n->ref_buf, frame->data, (size_t)frame->frame_count * sizeof(int16_t));
    n->ref_len   = (int)frame->frame_count;
    n->ref_ready = true;
    return AURA_OK;
}

static aura_err_t ae_audio_init(aura_node_t *self)
{
    ae_audio_node_t *n = (ae_audio_node_t *)self;

    n->frame_len = n->rate / 100;
    n->out_ch    = (n->desc->kind == AE_AUDIO_NIN1) ? 1 : n->in_ch;

    if (n->h == NULL) {
        int st = n->desc->create(&n->h);
        if (st != AUDIO_ENGINE_SUCCESS || n->h == NULL) {
            return ae_status_to_err(st, self->name);
        }
    }
    int st = n->desc->init(n->h, &n->params_cache, (int)n->rate, (int)n->frame_len, (int)n->in_ch);
    if (st != AUDIO_ENGINE_SUCCESS) {
        AURA_LOGE(TAG, "%s: AudioEngine init failed (0x%x)", self->name, st);
        /* pipeline 在 init 失败时不会再调 deinit（p->inited 仍为 false），
         * 句柄必须在这里收掉。 */
        if (n->desc->deinit != NULL) {
            (void)n->desc->deinit(n->h);
        }
        if (n->desc->destroy != NULL) {
            (void)n->desc->destroy(n->h);
        }
        n->h = NULL;
        return ae_status_to_err(st, self->name);
    }
    n->inited    = true;
    n->ref_ready = false;
    return AURA_OK;
}

static aura_err_t ae_audio_start(aura_node_t *self)
{
    (void)self;
    return AURA_OK;
}

static aura_err_t ae_audio_stop(aura_node_t *self)
{
    (void)self;
    return AURA_OK;
}

static aura_err_t ae_audio_control(aura_node_t *self, aura_node_cmd_t cmd, const void *arg)
{
    (void)arg;
    ae_audio_node_t *n = (ae_audio_node_t *)self;
    if (cmd == AURA_CMD_RESET || cmd == AURA_CMD_FLUSH) {
        n->ref_ready = false;
        if (n->h != NULL && n->desc->reset != NULL) {
            int st = n->desc->reset(n->h);
            return ae_status_to_err(st, self->name);
        }
    }
    return AURA_OK;
}

static void ae_audio_deinit(aura_node_t *self)
{
    ae_audio_node_t *n = (ae_audio_node_t *)self;
    if (n->h != NULL) {
        if (n->desc->deinit != NULL) {
            (void)n->desc->deinit(n->h);
        }
        if (n->desc->destroy != NULL) {
            (void)n->desc->destroy(n->h);
        }
        n->h = NULL;
    }
    n->inited = false;
}

static aura_err_t ae_audio_selftest(aura_node_t *self)
{
    ae_audio_node_t *n = (ae_audio_node_t *)self;
    return (n->h != NULL) ? AURA_OK : AURA_ERR_STATE;
}

static const aura_node_ops_t k_ae_audio_ops = {
    .init             = ae_audio_init,
    .start            = ae_audio_start,
    .stop             = ae_audio_stop,
    .process_audio    = ae_audio_process_audio,
    .process_ref_audio = ae_audio_process_ref,
    .control          = ae_audio_control,
    .deinit           = ae_audio_deinit,
    .selftest         = ae_audio_selftest,
};

/* ============================== 工厂 ============================== */

static aura_node_t *ae_audio_create_node(const aura_algo_params_t *p, aura_err_t *err)
{
    const ae_audio_desc_t *d = (const ae_audio_desc_t *)p->provider;
    if (d == NULL) {
        if (err != NULL) {
            *err = AURA_ERR_INVALID_ARG;
        }
        return NULL;
    }
    ae_audio_node_t *n = (ae_audio_node_t *)calloc(1, sizeof(*n));
    if (n == NULL) {
        if (err != NULL) {
            *err = AURA_ERR_NOMEM;
        }
        return NULL;
    }
    n->desc = d;
    n->rate = p->sample_rate;
    n->in_ch = p->in_channels ? p->in_channels : 1;
    n->rate_out = p->sample_rate;
    if (d->kind == AE_AUDIO_RESAMPLE) {
        int32_t out = ae_param_i32(p, "out_sample_rate", (int32_t)p->sample_rate);
        n->rate_out = (out > 0) ? (uint32_t)out : p->sample_rate;
    }
    /* 保存一份 params 供 init 使用：链描述的生命周期只到装配结束，不能存指针。 */
    n->params_cache = *p;
    ae_name_copy(n->name, sizeof(n->name), p->instance_name ? p->instance_name : d->base.name);

    aura_node_caps_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.consumes_audio = true;
    caps.produces_audio = true;
    if (d->kind == AE_AUDIO_2TO1) {
        caps.consumes_ref_audio = true;
    }
    if (d->kind == AE_AUDIO_RESAMPLE) {
        caps.changes_frame_rate = true;
    }
    aura_node_init(&n->base, n->name, &k_ae_audio_ops, &caps);
    return &n->base;
}

/* ============================== 各算法 shim ============================== */

/* ---- AEC3 ---- */
static int aec_create(void **h)
{
    *h = AudioEngine_Aec_Create();
    return *h ? AUDIO_ENGINE_SUCCESS : AUDIO_ENGINE_ERR_INIT_FAILED;
}
static int aec_init(void *h, const aura_algo_params_t *p, int rate, int fl, int ch)
{
    (void)fl;
    if (ch != 1) {
        return AUDIO_ENGINE_ERR_INVALID_PARAM; /* 推荐 bf 先降为单通道 */
    }
    AecInitConfig c;
    memset(&c, 0, sizeof(c));
    c.sample_rate          = rate;
    c.num_render_channels  = 1;
    c.num_capture_channels = 1;
    int st = AudioEngine_Aec_Init(h, &c);
    if (st == AUDIO_ENGINE_SUCCESS) {
        int32_t delay = ae_param_i32(p, "delay_ms", -1);
        if (delay >= 0) {
            AecRtConfig r = {delay};
            st = AudioEngine_Aec_SetParam(h, &r);
        }
    }
    return st;
}
static int aec_process(void *h, const int16_t *in, const int16_t *ref, int n, int16_t *out,
                       int max_out, int *out_samples, int *flag)
{
    AecInput_t i = {in, ref, n};
    AecResult_t r = {out, max_out, 0};
    *flag = 0;
    int st = AudioEngine_Aec_Process(h, &i, &r);
    *out_samples = r.out_samples;
    return st;
}
static int aec_reset(void *h) { return AudioEngine_Aec_Reset(h); }
static int aec_deinit(void *h) { return AudioEngine_Aec_Deinit(h); }
static int aec_destroy(void *h) { return AudioEngine_Aec_Destroy(h); }

/* ---- AECM ---- */
static int aecm_create(void **h)
{
    *h = AudioEngine_Aecm_Create();
    return *h ? AUDIO_ENGINE_SUCCESS : AUDIO_ENGINE_ERR_INIT_FAILED;
}
static int aecm_init(void *h, const aura_algo_params_t *p, int rate, int fl, int ch)
{
    (void)fl;
    if (ch != 1) {
        return AUDIO_ENGINE_ERR_INVALID_PARAM;
    }
    AecmInitConfig c;
    memset(&c, 0, sizeof(c));
    c.sample_rate = rate;
    c.cng_mode    = ae_param_i32(p, "cng_mode", 1);
    c.echo_mode   = ae_param_i32(p, "echo_mode", 3);
    return AudioEngine_Aecm_Init(h, &c);
}
static int aecm_process(void *h, const int16_t *in, const int16_t *ref, int n, int16_t *out,
                        int max_out, int *out_samples, int *flag)
{
    AecmInput_t i = {in, ref, n};
    AecmResult_t r = {out, max_out, 0};
    *flag = 0;
    int st = AudioEngine_Aecm_Process(h, &i, &r);
    *out_samples = r.out_samples;
    return st;
}
static int aecm_reset(void *h) { return AudioEngine_Aecm_Reset(h); }
static int aecm_deinit(void *h) { return AudioEngine_Aecm_Deinit(h); }
static int aecm_destroy(void *h) { return AudioEngine_Aecm_Destroy(h); }

/* ---- NS ---- */
static int ns_create(void **h)
{
    *h = AudioEngine_Ns_Create();
    return *h ? AUDIO_ENGINE_SUCCESS : AUDIO_ENGINE_ERR_INIT_FAILED;
}
static int ns_init(void *h, const aura_algo_params_t *p, int rate, int fl, int ch)
{
    (void)fl;
    if (ch != 1) {
        return AUDIO_ENGINE_ERR_INVALID_PARAM;
    }
    NsInitConfig c;
    memset(&c, 0, sizeof(c));
    c.sample_rate       = rate;
    c.num_channels      = 1;
    c.suppression_level = ae_param_i32(p, "suppression_level", 1);
    return AudioEngine_Ns_Init(h, &c);
}
static int ns_process(void *h, const int16_t *in, const int16_t *ref, int n, int16_t *out,
                      int max_out, int *out_samples, int *flag)
{
    (void)ref;
    NsInput_t i = {in, n};
    NsResult_t r = {out, max_out, 0};
    *flag = 0;
    int st = AudioEngine_Ns_Process(h, &i, &r);
    *out_samples = r.out_samples;
    return st;
}
static int ns_reset(void *h) { return AudioEngine_Ns_Reset(h); }
static int ns_deinit(void *h) { return AudioEngine_Ns_Deinit(h); }
static int ns_destroy(void *h) { return AudioEngine_Ns_Destroy(h); }

/* ---- AGC ---- */
static int agc_create(void **h)
{
    *h = AudioEngine_Agc_Create();
    return *h ? AUDIO_ENGINE_SUCCESS : AUDIO_ENGINE_ERR_INIT_FAILED;
}
static int agc_init(void *h, const aura_algo_params_t *p, int rate, int fl, int ch)
{
    (void)fl;
    if (ch != 1) {
        return AUDIO_ENGINE_ERR_INVALID_PARAM;
    }
    AgcInitConfig c;
    memset(&c, 0, sizeof(c));
    c.sample_rate = rate;
    c.agc_mode    = ae_param_i32(p, "agc_mode", 2);
    c.min_level   = ae_param_i32(p, "min_level", 0);
    c.max_level   = ae_param_i32(p, "max_level", 255);
    int st = AudioEngine_Agc_Init(h, &c);
    if (st == AUDIO_ENGINE_SUCCESS) {
        AgcRtConfig r;
        r.compression_gain_db = ae_param_i32(p, "compression_gain_db", -1);
        r.limiter_enable      = ae_param_i32(p, "limiter_enable", -1);
        r.target_level_dbfs   = ae_param_i32(p, "target_level_dbfs", -1);
        st = AudioEngine_Agc_SetParam(h, &r);
    }
    return st;
}
static int agc_process(void *h, const int16_t *in, const int16_t *ref, int n, int16_t *out,
                       int max_out, int *out_samples, int *flag)
{
    (void)ref;
    AgcInput_t i = {in, n};
    AgcResult_t r = {out, max_out, 0};
    *flag = 0;
    int st = AudioEngine_Agc_Process(h, &i, &r);
    *out_samples = r.out_samples;
    return st;
}
static int agc_reset(void *h) { return AudioEngine_Agc_Reset(h); }
static int agc_deinit(void *h) { return AudioEngine_Agc_Deinit(h); }
static int agc_destroy(void *h) { return AudioEngine_Agc_Destroy(h); }

/* ---- AGC2 ---- */
static int agc2_create(void **h)
{
    *h = AudioEngine_Agc2_Create();
    return *h ? AUDIO_ENGINE_SUCCESS : AUDIO_ENGINE_ERR_INIT_FAILED;
}
static int agc2_init(void *h, const aura_algo_params_t *p, int rate, int fl, int ch)
{
    (void)fl;
    if (ch != 1) {
        return AUDIO_ENGINE_ERR_INVALID_PARAM;
    }
    Agc2InitConfig c;
    memset(&c, 0, sizeof(c));
    c.sample_rate                      = rate;
    c.num_channels                     = 1;
    c.headroom_db                      = ae_param_f32(p, "headroom_db", 5.0f);
    c.max_gain_db                      = ae_param_f32(p, "max_gain_db", 50.0f);
    c.initial_gain_db                  = ae_param_f32(p, "initial_gain_db", 15.0f);
    c.max_gain_change_db_per_second    = ae_param_f32(p, "max_gain_change_db_per_second", 6.0f);
    c.max_output_noise_level_dbfs      = ae_param_f32(p, "max_output_noise_level_dbfs", -50.0f);
    return AudioEngine_Agc2_Init(h, &c);
}
static int agc2_process(void *h, const int16_t *in, const int16_t *ref, int n, int16_t *out,
                        int max_out, int *out_samples, int *flag)
{
    (void)ref;
    Agc2Input_t i = {in, n};
    Agc2Result_t r = {out, max_out, 0};
    *flag = 0;
    int st = AudioEngine_Agc2_Process(h, &i, &r);
    *out_samples = r.out_samples;
    return st;
}
static int agc2_reset(void *h) { return AudioEngine_Agc2_Reset(h); }
static int agc2_deinit(void *h) { return AudioEngine_Agc2_Deinit(h); }
static int agc2_destroy(void *h) { return AudioEngine_Agc2_Destroy(h); }

/* ---- HS ---- */
static int hs_create(void **h)
{
    *h = AudioEngine_Hs_Create();
    return *h ? AUDIO_ENGINE_SUCCESS : AUDIO_ENGINE_ERR_INIT_FAILED;
}
static int hs_init(void *h, const aura_algo_params_t *p, int rate, int fl, int ch)
{
    if (ch != 1) {
        return AUDIO_ENGINE_ERR_INVALID_PARAM;
    }
    HsInitConfig c;
    memset(&c, 0, sizeof(c));
    c.sample_rate          = rate;
    c.frame_len            = fl;
    c.detect_threshold     = ae_param_f32(p, "detect_threshold", 13.0f);
    c.detect_block         = ae_param_i32(p, "detect_block", 5);
    c.detect_freq_min      = ae_param_f32(p, "detect_freq_min", 650.0f);
    c.detect_freq_max      = ae_param_f32(p, "detect_freq_max", 3000.0f);
    c.detect_freq_step     = ae_param_f32(p, "detect_freq_step", 25.0f);
    c.notch_persist_block  = ae_param_i32(p, "notch_persist_block", 5);
    c.notch_filter_Q       = ae_param_f32(p, "notch_filter_Q", 0.7071f);
    return AudioEngine_Hs_Init(h, &c);
}
static int hs_process(void *h, const int16_t *in, const int16_t *ref, int n, int16_t *out,
                      int max_out, int *out_samples, int *flag)
{
    (void)ref;
    (void)max_out;
    HsInput_t i = {in, n};
    HsResult_t r = {out};
    *flag = 0;
    int st = AudioEngine_Hs_Process(h, &i, &r);
    *out_samples = (st == AUDIO_ENGINE_SUCCESS) ? n : 0;
    return st;
}
static int hs_reset(void *h) { return AudioEngine_Hs_Reset(h); }
static int hs_deinit(void *h) { return AudioEngine_Hs_Deinit(h); }
static int hs_destroy(void *h) { return AudioEngine_Hs_Destroy(h); }

/* ---- TS ---- */
static int ts_create(void **h)
{
    *h = AudioEngine_Ts_Create();
    return *h ? AUDIO_ENGINE_SUCCESS : AUDIO_ENGINE_ERR_INIT_FAILED;
}
static int ts_init(void *h, const aura_algo_params_t *p, int rate, int fl, int ch)
{
    if (ch != 1) {
        return AUDIO_ENGINE_ERR_INVALID_PARAM;
    }
    TsInitConfig c;
    memset(&c, 0, sizeof(c));
    c.sample_rate   = rate;
    c.frame_len     = fl;
    c.num_channels  = 1;
    c.detector_rate = ae_param_i32(p, "detector_rate", rate);
    return AudioEngine_Ts_Init(h, &c);
}
static int ts_process(void *h, const int16_t *in, const int16_t *ref, int n, int16_t *out,
                      int max_out, int *out_samples, int *flag)
{
    (void)ref;
    (void)max_out;
    TsInput_t i;
    memset(&i, 0, sizeof(i));
    i.audio_in          = in;
    i.in_samples        = n;
    i.voice_probability = 1.0f;
    i.key_pressed       = 0;
    TsResult_t r = {out};
    *flag = 0;
    int st = AudioEngine_Ts_Process(h, &i, &r);
    *out_samples = (st == AUDIO_ENGINE_SUCCESS) ? n : 0;
    return st;
}
static int ts_reset(void *h) { return AudioEngine_Ts_Reset(h); }
static int ts_deinit(void *h) { return AudioEngine_Ts_Deinit(h); }
static int ts_destroy(void *h) { return AudioEngine_Ts_Destroy(h); }

/* ---- BF ---- */
static int bf_create(void **h)
{
    *h = AudioEngine_Bf_Create();
    return *h ? AUDIO_ENGINE_SUCCESS : AUDIO_ENGINE_ERR_INIT_FAILED;
}
static int bf_init(void *h, const aura_algo_params_t *p, int rate, int fl, int ch)
{
    if (ch <= 0 || ch > 16) {
        return AUDIO_ENGINE_ERR_INVALID_PARAM;
    }
    float spacing = ae_param_f32(p, "mic_spacing_m", 0.02f);
    if (spacing <= 0.0f) {
        spacing = 0.02f;
    }
    BfMicPosition pos[16];
    for (int i = 0; i < ch; i++) {
        pos[i].x = ((float)i - (float)(ch - 1) * 0.5f) * spacing;
        pos[i].y = 0.0f;
        pos[i].z = 0.0f;
    }
    BfInitConfig c;
    memset(&c, 0, sizeof(c));
    c.sample_rate      = rate;
    c.num_channels     = ch;
    c.frame_len        = fl;
    c.mic_pos          = pos;
    c.target_azimuth   = ae_param_f32(p, "target_azimuth", 1.57079632679f);
    c.target_elevation = ae_param_f32(p, "target_elevation", 0.0f);
    return AudioEngine_Bf_Init(h, &c);
}
static int bf_process(void *h, const int16_t *in, const int16_t *ref, int n, int16_t *out,
                      int max_out, int *out_samples, int *flag)
{
    (void)ref;
    (void)max_out;
    BfInput_t i = {in, n};
    BfResult_t r = {out, 0};
    int st = AudioEngine_Bf_Process(h, &i, &r);
    *out_samples = (st == AUDIO_ENGINE_SUCCESS) ? n : 0;
    *flag        = r.is_target_present;
    return st;
}
static int bf_reset(void *h) { return AudioEngine_Bf_Reset(h); }
static int bf_deinit(void *h) { return AudioEngine_Bf_Deinit(h); }
static int bf_destroy(void *h) { return AudioEngine_Bf_Destroy(h); }

/* ---- SRC ---- */
static int src_create(void **h)
{
    *h = AudioEngine_Resample_Create();
    return *h ? AUDIO_ENGINE_SUCCESS : AUDIO_ENGINE_ERR_INIT_FAILED;
}
static int src_init(void *h, const aura_algo_params_t *p, int rate, int fl, int ch)
{
    (void)fl;
    if (ch != 1) {
        return AUDIO_ENGINE_ERR_INVALID_PARAM;
    }
    int32_t dst = ae_param_i32(p, "out_sample_rate", rate);
    if (dst <= 0 || dst > 192000) {
        return AUDIO_ENGINE_ERR_INVALID_PARAM;
    }
    ResampleInitConfig c;
    memset(&c, 0, sizeof(c));
    c.src_sample_rate = rate;
    c.dst_sample_rate = dst;
    c.num_channels    = 1;
    return AudioEngine_Resample_Init(h, &c);
}
static int src_process(void *h, const int16_t *in, const int16_t *ref, int n, int16_t *out,
                       int max_out, int *out_samples, int *flag)
{
    (void)ref;
    ResampleInput_t i = {in, n};
    ResampleResult_t r = {out, max_out, 0};
    *flag = 0;
    int st = AudioEngine_Resample_Process(h, &i, &r);
    *out_samples = r.out_samples;
    return st;
}
static int src_reset(void *h) { return AudioEngine_Resample_Reset(h); }
static int src_deinit(void *h) { return AudioEngine_Resample_Deinit(h); }
static int src_destroy(void *h) { return AudioEngine_Resample_Destroy(h); }

/* ============================== 参数规格 ============================== */

static const aura_algo_param_spec_t k_aec_specs[] = {
    {"delay_ms", AURA_ALGO_PARAM_I32, -1, 1000, "audio buffer delay; -1 keeps default"},
};
static const aura_algo_param_spec_t k_aecm_specs[] = {
    {"cng_mode", AURA_ALGO_PARAM_I32, 0, 1, "comfort noise 0/1"},
    {"echo_mode", AURA_ALGO_PARAM_I32, 0, 4, "echo suppression 0..4"},
};
static const aura_algo_param_spec_t k_ns_specs[] = {
    {"suppression_level", AURA_ALGO_PARAM_I32, 0, 3, "0=k6dB 1=k12dB 2=k18dB 3=k21dB"},
};
static const aura_algo_param_spec_t k_agc_specs[] = {
    {"agc_mode", AURA_ALGO_PARAM_I32, 0, 3, "0=unchanged 1=analog 2=digital 3=fixed"},
    {"min_level", AURA_ALGO_PARAM_I32, 0, 255, "mic level min"},
    {"max_level", AURA_ALGO_PARAM_I32, 0, 255, "mic level max"},
    {"compression_gain_db", AURA_ALGO_PARAM_I32, -1, 90, "-1 keeps current"},
    {"limiter_enable", AURA_ALGO_PARAM_I32, -1, 1, "-1 keeps current"},
    {"target_level_dbfs", AURA_ALGO_PARAM_I32, -1, 31, "-1 keeps current"},
};
static const aura_algo_param_spec_t k_agc2_specs[] = {
    {"headroom_db", AURA_ALGO_PARAM_F32, 0, 60, "target headroom"},
    {"max_gain_db", AURA_ALGO_PARAM_F32, 0, 90, "max gain"},
    {"initial_gain_db", AURA_ALGO_PARAM_F32, 0, 90, "initial gain"},
    {"max_gain_change_db_per_second", AURA_ALGO_PARAM_F32, 0, 100, "gain slew rate"},
    {"max_output_noise_level_dbfs", AURA_ALGO_PARAM_F32, -100, 0, "noise floor"},
};
static const aura_algo_param_spec_t k_hs_specs[] = {
    {"detect_threshold", AURA_ALGO_PARAM_F32, 0, 100, "howling detection threshold"},
    {"detect_block", AURA_ALGO_PARAM_I32, 1, 100, "consecutive detection blocks"},
    {"detect_freq_min", AURA_ALGO_PARAM_F32, 20, 20000, "min detection frequency"},
    {"detect_freq_max", AURA_ALGO_PARAM_F32, 20, 20000, "max detection frequency"},
    {"detect_freq_step", AURA_ALGO_PARAM_F32, 1, 1000, "frequency step"},
    {"notch_persist_block", AURA_ALGO_PARAM_I32, 1, 100, "notch persist blocks"},
    {"notch_filter_Q", AURA_ALGO_PARAM_F32, 0.1f, 100, "notch Q"},
};
static const aura_algo_param_spec_t k_ts_specs[] = {
    {"detector_rate", AURA_ALGO_PARAM_I32, 0, 192000, "detection sample rate; 0=same"},
};
static const aura_algo_param_spec_t k_bf_specs[] = {
    {"mic_spacing_m", AURA_ALGO_PARAM_F32, 0.001f, 1.0f, "linear array spacing in meters"},
    {"target_azimuth", AURA_ALGO_PARAM_F32, -3.15f, 3.15f, "target azimuth (rad)"},
    {"target_elevation", AURA_ALGO_PARAM_F32, -1.57f, 1.57f, "target elevation (rad)"},
};
static const aura_algo_param_spec_t k_src_specs[] = {
    {"out_sample_rate", AURA_ALGO_PARAM_I32, 8000, 192000, "destination sample rate"},
};

/* ============================== shape_out ============================== */

static aura_err_t bf_shape_out(const aura_algo_params_t *p, uint32_t *rate, uint32_t *channels)
{
    (void)p;
    (void)rate;
    *channels = 1;
    return AURA_OK;
}

static aura_err_t src_shape_out(const aura_algo_params_t *p, uint32_t *rate, uint32_t *channels)
{
    (void)channels;
    int32_t out = ae_param_i32(p, "out_sample_rate", 16000);
    if (out <= 0) {
        return AURA_ERR_INVALID_ARG;
    }
    *rate = (uint32_t)out;
    return AURA_OK;
}

/* ============================== 描述表 ============================== */

static const ae_audio_desc_t k_desc_aec = {
    .base = {.name = "aec",
             .version = 1,
             .kind = AURA_ALGO_KIND_DSP,
             .io = {.consumes_audio = true, .produces_audio = true, .consumes_ref = true},
             .param_specs = k_aec_specs,
             .param_spec_count = sizeof(k_aec_specs) / sizeof(k_aec_specs[0]),
             .create = ae_audio_create_node},
    .kind = AE_AUDIO_2TO1,
    .create = aec_create,
    .init = aec_init,
    .process = aec_process,
    .reset = aec_reset,
    .deinit = aec_deinit,
    .destroy = aec_destroy,
};

static const ae_audio_desc_t k_desc_aecm = {
    .base = {.name = "aecm",
             .version = 1,
             .kind = AURA_ALGO_KIND_DSP,
             .io = {.consumes_audio = true, .produces_audio = true, .consumes_ref = true},
             .param_specs = k_aecm_specs,
             .param_spec_count = sizeof(k_aecm_specs) / sizeof(k_aecm_specs[0]),
             .create = ae_audio_create_node},
    .kind = AE_AUDIO_2TO1,
    .create = aecm_create,
    .init = aecm_init,
    .process = aecm_process,
    .reset = aecm_reset,
    .deinit = aecm_deinit,
    .destroy = aecm_destroy,
};

static const ae_audio_desc_t k_desc_ns = {
    .base = {.name = "ns",
             .version = 1,
             .kind = AURA_ALGO_KIND_DSP,
             .io = {.consumes_audio = true, .produces_audio = true},
             .param_specs = k_ns_specs,
             .param_spec_count = sizeof(k_ns_specs) / sizeof(k_ns_specs[0]),
             .create = ae_audio_create_node},
    .kind = AE_AUDIO_1TO1,
    .create = ns_create,
    .init = ns_init,
    .process = ns_process,
    .reset = ns_reset,
    .deinit = ns_deinit,
    .destroy = ns_destroy,
};

static const ae_audio_desc_t k_desc_agc = {
    .base = {.name = "agc",
             .version = 1,
             .kind = AURA_ALGO_KIND_DSP,
             .io = {.consumes_audio = true, .produces_audio = true},
             .param_specs = k_agc_specs,
             .param_spec_count = sizeof(k_agc_specs) / sizeof(k_agc_specs[0]),
             .create = ae_audio_create_node},
    .kind = AE_AUDIO_1TO1,
    .create = agc_create,
    .init = agc_init,
    .process = agc_process,
    .reset = agc_reset,
    .deinit = agc_deinit,
    .destroy = agc_destroy,
};

static const ae_audio_desc_t k_desc_agc2 = {
    .base = {.name = "agc2",
             .version = 1,
             .kind = AURA_ALGO_KIND_DSP,
             .io = {.consumes_audio = true, .produces_audio = true},
             .param_specs = k_agc2_specs,
             .param_spec_count = sizeof(k_agc2_specs) / sizeof(k_agc2_specs[0]),
             .create = ae_audio_create_node},
    .kind = AE_AUDIO_1TO1,
    .create = agc2_create,
    .init = agc2_init,
    .process = agc2_process,
    .reset = agc2_reset,
    .deinit = agc2_deinit,
    .destroy = agc2_destroy,
};

static const ae_audio_desc_t k_desc_hs = {
    .base = {.name = "hs",
             .version = 1,
             .kind = AURA_ALGO_KIND_DSP,
             .io = {.consumes_audio = true, .produces_audio = true},
             .param_specs = k_hs_specs,
             .param_spec_count = sizeof(k_hs_specs) / sizeof(k_hs_specs[0]),
             .create = ae_audio_create_node},
    .kind = AE_AUDIO_1TO1,
    .create = hs_create,
    .init = hs_init,
    .process = hs_process,
    .reset = hs_reset,
    .deinit = hs_deinit,
    .destroy = hs_destroy,
};

static const ae_audio_desc_t k_desc_ts = {
    .base = {.name = "ts",
             .version = 1,
             .kind = AURA_ALGO_KIND_DSP,
             .io = {.consumes_audio = true, .produces_audio = true},
             .param_specs = k_ts_specs,
             .param_spec_count = sizeof(k_ts_specs) / sizeof(k_ts_specs[0]),
             .create = ae_audio_create_node},
    .kind = AE_AUDIO_1TO1,
    .create = ts_create,
    .init = ts_init,
    .process = ts_process,
    .reset = ts_reset,
    .deinit = ts_deinit,
    .destroy = ts_destroy,
};

static const ae_audio_desc_t k_desc_bf = {
    .base = {.name = "bf",
             .version = 1,
             .kind = AURA_ALGO_KIND_DSP,
             .io = {.consumes_audio = true, .produces_audio = true, .changes_channels = true},
             .shape_out = bf_shape_out,
             .param_specs = k_bf_specs,
             .param_spec_count = sizeof(k_bf_specs) / sizeof(k_bf_specs[0]),
             .create = ae_audio_create_node},
    .kind = AE_AUDIO_NIN1,
    .create = bf_create,
    .init = bf_init,
    .process = bf_process,
    .reset = bf_reset,
    .deinit = bf_deinit,
    .destroy = bf_destroy,
};

static const ae_audio_desc_t k_desc_src = {
    .base = {.name = "src",
             .version = 1,
             .kind = AURA_ALGO_KIND_DSP,
             .io = {.consumes_audio = true, .produces_audio = true, .changes_frame_rate = true},
             .shape_out = src_shape_out,
             .param_specs = k_src_specs,
             .param_spec_count = sizeof(k_src_specs) / sizeof(k_src_specs[0]),
             .create = ae_audio_create_node},
    .kind = AE_AUDIO_RESAMPLE,
    .create = src_create,
    .init = src_init,
    .process = src_process,
    .reset = src_reset,
    .deinit = src_deinit,
    .destroy = src_destroy,
};

static const ae_audio_desc_t *const k_audio_descs[] = {
    &k_desc_aec, &k_desc_aecm, &k_desc_ns, &k_desc_agc, &k_desc_agc2,
    &k_desc_hs,  &k_desc_ts,   &k_desc_bf, &k_desc_src,
};

aura_err_t ae_audio_register_all(void)
{
    for (size_t i = 0; i < sizeof(k_audio_descs) / sizeof(k_audio_descs[0]); i++) {
        aura_err_t rc = aura_algo_register(&k_audio_descs[i]->base);
        if (rc != AURA_OK) {
            AURA_LOGE(TAG, "register %s failed: %s", k_audio_descs[i]->base.name,
                      aura_strerror(rc));
            return rc;
        }
    }
    return AURA_OK;
}
