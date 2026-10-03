/* Aura — TrickRoom 描述表（DSP 适配模板的**唯一**一份引擎相关代码）
 *
 * 这里是整个 dsp/ 层唯一 include TrickRoom 头的地方。换 3A 引擎 = 换这一张表：
 * 适配层（攒帧/参考对齐/emit/事件/统计，见 aura_dsp_adapter.c）一行不改。
 *
 * 每个算法只需要写三样东西：
 *   ① 一个 shim —— 把该算法的 Process 签名转成统一的最宽签名（5 行）；
 *   ② 两个 cfg 填充 —— params → InitConfig/RtConfig（先铺默认值，再按名覆盖）；
 *   ③ 表项里的一行 io_kind —— 适配层据此决定该如何调用、如何接线。
 *
 * Phase 2 先落 AEC / NS / VAD 三个（3A 主链路的最小闭环，也是 host_sim 要用的）。
 * 其余 9 个（AECM/AGC/AGC2/BF/SRC/HS/IE/TS/DR）按同样的模式逐个补 —— 没有结构
 * 性障碍，只是需要各自的 cfg 字段与 shim。SRC 补上后，"链上换个采样率"就从
 * 一句描述变成一个表项。
 */
#ifndef AURA_DSP_TRICKROOM_ENABLED
#error "aura_dsp_trickroom.c 只在 AURA_WITH_TRICKROOM=ON 时编译"
#endif

#include <string.h>

#include "audio_engine_aec.h"
#include "audio_engine_def.h"
#include "audio_engine_ns.h"
#include "audio_engine_vad.h"

#include "dsp/aura_dsp_adapter.h"

/* ============================== 生命周期 shim ==============================
 * 引擎的 Init/SetParam 收的是**具体配置结构体指针**，而描述表的字段是
 * `int (*)(void *, const void *)`。直接赋值是函数指针类型不兼容（每个调用点
 * 都靠 ABI 恰好一致在跑），所以这里转发一层 —— 用宏写，一族算法共用一份。
 * 参数是算法符号前缀与它自己的两个配置类型。 */
#define TR_LIFECYCLE(OPS, INIT_CFG, RT_CFG)                                              \
    static void *OPS##_create(void) { return (void *)OPS##_Create(); }                    \
    static int   OPS##_destroy(void *h) { return OPS##_Destroy(h); }                      \
    static int   OPS##_init(void *h, const void *cfg)                                     \
    {                                                                                     \
        return OPS##_Init(h, (const INIT_CFG *)cfg);                                      \
    }                                                                                     \
    static int OPS##_set_param(void *h, const void *cfg)                                  \
    {                                                                                     \
        return OPS##_SetParam(h, (const RT_CFG *)cfg);                                    \
    }                                                                                     \
    static int OPS##_reset_param(void *h, const void *cfg)                                \
    {                                                                                     \
        return OPS##_ResetParam(h, (const RT_CFG *)cfg);                                  \
    }                                                                                     \
    static int OPS##_deinit(void *h) { return OPS##_Deinit(h); }                          \
    static int OPS##_reset(void *h) { return OPS##_Reset(h); }

TR_LIFECYCLE(AudioEngine_Aec, AecInitConfig, AecRtConfig)
TR_LIFECYCLE(AudioEngine_Ns, NsInitConfig, NsRtConfig)
TR_LIFECYCLE(AudioEngine_Vad, VadInitConfig, VadRtConfig)

/* ============================== 状态码映射 ==============================
 * 引擎的枚举值在这里翻译成 aura_err_t。放在引擎侧而不是适配层，
 * 是因为这些值属于引擎的 ABI —— 引擎改了枚举，这里编译期就会漏掉新的 case，
 * 而在适配层硬编码一份副本只会安静地分类错。 */
static aura_err_t trickroom_map_status(int status)
{
    switch (status) {
    case AUDIO_ENGINE_SUCCESS:
        return AURA_OK;
    case AUDIO_ENGINE_ERR_INVALID_HANDLE:
    case AUDIO_ENGINE_ERR_NOT_INITIALIZED:
        return AURA_ERR_STATE;
    case AUDIO_ENGINE_ERR_NULL_POINTER:
    case AUDIO_ENGINE_ERR_INVALID_PARAM:
        return AURA_ERR_INVALID_ARG;
    case AUDIO_ENGINE_ERR_PROCESS_FAILED:
        return AURA_ERR_DSP;
    case AUDIO_ENGINE_ERR_INIT_FAILED:
    case AUDIO_ENGINE_ERR_SET_PARAM_FAILED:
    default:
        return AURA_ERR_FAIL;
    }
}

/* ============================== AEC3 ============================== */

/* 2→1：近端 + 参考 → 输出。参考由适配层从 ref 队列按帧取（缺则喂静音）。 */
static int shim_aec(void *h, const int16_t *in, const int16_t *ref, int in_samples, int16_t *out,
                    int max_out, int *out_samples, int *flag)
{
    (void)flag;
    return AudioEngine_Aec_Process((AecHandle)h, in, ref, in_samples, out, max_out, out_samples);
}

static aura_err_t aec_init_cfg(const aura_algo_params_t *p, void *cfg, uint32_t cap)
{
    if (cap < sizeof(AecInitConfig)) {
        return AURA_ERR_INVALID_ARG;
    }
    AecInitConfig *c = (AecInitConfig *)cfg;
    memset(c, 0, sizeof(*c));
    c->sample_rate          = (int)p->sample_rate;
    c->num_render_channels  = 1;                  /* 参考是回采单通道 */
    c->num_capture_channels = (int)p->in_channels;
    return AURA_OK;
}

static aura_err_t aec_rt_cfg(const aura_algo_params_t *p, void *cfg, uint32_t cap)
{
    if (cap < sizeof(AecRtConfig)) {
        return AURA_ERR_INVALID_ARG;
    }
    AecRtConfig *c = (AecRtConfig *)cfg;
    memset(c, 0, sizeof(*c));
    /* -1 = 保持当前估计。真实延迟要等 AEC 自己收敛报出来，框架不猜 ——
     * 猜错一个 ms 会让回声消除效果断崖式下降。 */
    c->delay_ms = aura_algo_params_i32(p, "delay_ms", -1);
    return AURA_OK;
}

/* ================================ NS ================================ */

/* 1→1 */
static int shim_ns(void *h, const int16_t *in, const int16_t *ref, int in_samples, int16_t *out,
                   int max_out, int *out_samples, int *flag)
{
    (void)ref;
    (void)flag;
    return AudioEngine_Ns_Process((NsHandle)h, in, in_samples, out, max_out, out_samples);
}

static aura_err_t ns_init_cfg(const aura_algo_params_t *p, void *cfg, uint32_t cap)
{
    if (cap < sizeof(NsInitConfig)) {
        return AURA_ERR_INVALID_ARG;
    }
    NsInitConfig *c = (NsInitConfig *)cfg;
    memset(c, 0, sizeof(*c));
    c->sample_rate = (int)p->sample_rate;
    c->num_channels = (int)p->in_channels;
    c->suppression_level = aura_algo_params_i32(p, "suppression_level", 1); /* 0..3，默认 12dB */
    return AURA_OK;
}

static aura_err_t ns_rt_cfg(const aura_algo_params_t *p, void *cfg, uint32_t cap)
{
    (void)p;
    if (cap < sizeof(NsRtConfig)) {
        return AURA_ERR_INVALID_ARG;
    }
    memset(cfg, 0, sizeof(NsRtConfig)); /* reserved，保持 0 */
    return AURA_OK;
}

/* ================================ VAD ================================
 * 1→0：只出标志。适配层把标志的**边沿**转成事件（每帧发事件会刷爆总线，
 * 下游要的本来就是状态变化），并抬高阈值供播放期抑制自打断使用。 */

static int shim_vad(void *h, const int16_t *in, const int16_t *ref, int in_samples, int16_t *out,
                    int max_out, int *out_samples, int *flag)
{
    (void)ref;
    (void)out;
    (void)max_out;
    (void)out_samples;
    return AudioEngine_Vad_Process((VadHandle)h, in, in_samples, flag);
}

static aura_err_t vad_init_cfg(const aura_algo_params_t *p, void *cfg, uint32_t cap)
{
    if (cap < sizeof(VadInitConfig)) {
        return AURA_ERR_INVALID_ARG;
    }
    VadInitConfig *c = (VadInitConfig *)cfg;
    memset(c, 0, sizeof(*c));
    c->sample_rate = (int)p->sample_rate;
    c->frame_len   = (int)p->sample_rate / 100; /* 硬约束：10ms 帧 */
    return AURA_OK;
}

static aura_err_t vad_rt_cfg(const aura_algo_params_t *p, void *cfg, uint32_t cap)
{
    if (cap < sizeof(VadRtConfig)) {
        return AURA_ERR_INVALID_ARG;
    }
    VadRtConfig *c = (VadRtConfig *)cfg;
    memset(c, 0, sizeof(*c));
    c->threshold = aura_algo_params_f32(p, "threshold", 0.5f);
    return AURA_OK;
}

/* ============================== 参数规格 ============================== */

static const aura_algo_param_spec_t AEC_SPECS[] = {
    { "delay_ms", AURA_ALGO_PARAM_I32, -1.f, 500.f, "回采延迟(ms)，-1=引擎自估" },
};

static const aura_algo_param_spec_t NS_SPECS[] = {
    { "suppression_level", AURA_ALGO_PARAM_I32, 0.f, 3.f, "0=6dB 1=12dB 2=18dB 3=21dB" },
};

static const aura_algo_param_spec_t VAD_SPECS[] = {
    { "threshold", AURA_ALGO_PARAM_F32, 0.f, 1.f, "语音概率阈值，默认 0.5" },
};

/* ============================== 描述表 ============================== */

static aura_dsp_desc_t g_trickroom_descs[] = {
    {
        .name          = "aec3",
        .io_kind       = AURA_DSP_IO_2TO1,
        .map_status    = trickroom_map_status,
        .create        = AudioEngine_Aec_create,
        .destroy       = AudioEngine_Aec_destroy,
        .init          = AudioEngine_Aec_init,
        .set_param     = AudioEngine_Aec_set_param,
        .reset_param   = AudioEngine_Aec_reset_param,
        .deinit        = AudioEngine_Aec_deinit,
        .reset         = AudioEngine_Aec_reset,
        .process       = shim_aec,
        .param_specs   = AEC_SPECS,
        .param_spec_count = sizeof(AEC_SPECS) / sizeof(AEC_SPECS[0]),
        .build_init_cfg = aec_init_cfg,
        .init_cfg_size  = sizeof(AecInitConfig),
        .build_rt_cfg   = aec_rt_cfg,
        .rt_cfg_size    = sizeof(AecRtConfig),
        /* AEC 出错时透传近端更危险：未消除的回声会被下游当成用户语音，
         * 而 AEC 出错的常见原因恰恰是参考失配。丢帧让链路安静下来。 */
        .drop_on_error  = true,
    },
    {
        .name          = "ns",
        .io_kind       = AURA_DSP_IO_1TO1,
        .map_status    = trickroom_map_status,
        .create        = AudioEngine_Ns_create,
        .destroy       = AudioEngine_Ns_destroy,
        .init          = AudioEngine_Ns_init,
        .set_param     = AudioEngine_Ns_set_param,
        .reset_param   = AudioEngine_Ns_reset_param,
        .deinit        = AudioEngine_Ns_deinit,
        .reset         = AudioEngine_Ns_reset,
        .process       = shim_ns,
        .param_specs   = NS_SPECS,
        .param_spec_count = sizeof(NS_SPECS) / sizeof(NS_SPECS[0]),
        .build_init_cfg = ns_init_cfg,
        .init_cfg_size  = sizeof(NsInitConfig),
        .build_rt_cfg   = ns_rt_cfg,
        .rt_cfg_size    = sizeof(NsRtConfig),
        .drop_on_error  = false, /* 降噪挂掉：没降噪的语音远好于静音 */
    },
    {
        .name          = "vad",
        .io_kind       = AURA_DSP_IO_1TO0,
        .map_status    = trickroom_map_status,
        .create        = AudioEngine_Vad_create,
        .destroy       = AudioEngine_Vad_destroy,
        .init          = AudioEngine_Vad_init,
        .set_param     = AudioEngine_Vad_set_param,
        .reset_param   = AudioEngine_Vad_reset_param,
        .deinit        = AudioEngine_Vad_deinit,
        .reset         = AudioEngine_Vad_reset,
        .process       = shim_vad,
        .param_specs   = VAD_SPECS,
        .param_spec_count = sizeof(VAD_SPECS) / sizeof(VAD_SPECS[0]),
        .build_init_cfg = vad_init_cfg,
        .init_cfg_size  = sizeof(VadInitConfig),
        .build_rt_cfg   = vad_rt_cfg,
        .rt_cfg_size    = sizeof(VadRtConfig),
        /* 边沿 → 事件：VAD 的消费方（状态机 / barge-in 裁决）要的是"开始说话"
         * 与"说完了"这两个时刻，而不是每 10ms 一次的 0/1。 */
        .flag_rise_type = AURA_EVENT_VAD_SPEECH_START,
        .flag_fall_type = AURA_EVENT_VAD_SPEECH_END,
        .drop_on_error  = false, /* 无音频产出，此项对它无影响 */
    },
};

aura_dsp_desc_t *aura_dsp_trickroom_descs(uint32_t *count)
{
    if (count != NULL) {
        *count = (uint32_t)(sizeof(g_trickroom_descs) / sizeof(g_trickroom_descs[0]));
    }
    return g_trickroom_descs;
}
