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
 * 已落：aec3 / bf / ns / agc / src —— 即 Phase 2 的链 aec3,bf,ns,agc,src，
 * 外加 Phase 1 就在用的 vad。其余（AECM/AGC2/HS/IE/TS/DR）按同样模式补即可，
 * 没有结构性障碍，只是需要各自的 cfg 字段与 shim。
 */
#ifndef AURA_DSP_TRICKROOM_ENABLED
#error "aura_dsp_trickroom.c 只在 AURA_WITH_TRICKROOM=ON 时编译"
#endif

#include <math.h>
#include <string.h>

#include "audio_engine_aec.h"
#include "audio_engine_agc.h"
#include "audio_engine_bf.h"
#include "audio_engine_def.h"
#include "audio_engine_ns.h"
#include "audio_engine_src.h"
#include "audio_engine_vad.h"

#include "dsp/aura_dsp_adapter.h"
#include "utils/logger/logger.h"

#define TAG "dsp.tr"

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
TR_LIFECYCLE(AudioEngine_Bf, BfInitConfig, BfRtConfig)
TR_LIFECYCLE(AudioEngine_Ns, NsInitConfig, NsRtConfig)
TR_LIFECYCLE(AudioEngine_Agc, AgcInitConfig, AgcRtConfig)
TR_LIFECYCLE(AudioEngine_Resample, ResampleInitConfig, ResampleRtConfig)
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

/* 2→1：近端 + 参考 → 输出。参考由适配层从 ref 队列按帧取（缺则喂静音）。
 * **参考流的通道数必须与近端一致**（2 路近端就喂 2 路交错参考）：引擎把 farend
 * 按 capture 的 StreamConfig 读取后内部下混成单路 render（audio_engine_aec.cpp
 * 的 render_buffer_->CopyFrom），喂 1 路会让它按 2 路去读，取到的是越界的
 * 后半段。适配层的 dsp_process_ref_audio 已强制 ref.channels == in_channels。 */
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
    if (p->in_channels < 1 || p->in_channels > 2) {
        AURA_LOGE(TAG, "aec3: 捕获通道数 %u 不支持（引擎限 1..2）", p->in_channels);
        return AURA_ERR_INVALID_ARG;
    }
    AecInitConfig *c = (AecInitConfig *)cfg;
    memset(c, 0, sizeof(*c));
    c->sample_rate = (int)p->sample_rate;
    /* render 通道数**必须等于** capture 通道数，这不是我们的偏好而是引擎的硬约束：
     * audio_engine_aec.cpp 用同一个 stream_config_（= capture 通道数）去
     * CopyFrom 渲染缓冲，而 AudioBuffer::CopyFrom 是按**缓冲自己的**通道数分派的 ——
     * 缓冲建的是 1 路时它走"入参即单声道，取前 input_num_frames_ 个样本"的分支，
     * 于是 2 路交错的参考会被读成 [f0,f0,f1,f1,…]：抽掉一半、时间拉伸两倍。
     * 捕获路径正常、渲染路径错位，表现是回声根本对不齐（ERLE 只有十几 dB、
     * 且随调度抖动），而**不报任何错**。
     * AEC3 内部会自己判断"这两路是真立体声还是复制"，复制内容会退回单路处理，
     * 所以让参考走两路没有副作用。 */
    c->num_render_channels  = (int)p->in_channels;
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

/* ================================ BF ================================
 * N→1：多麦合成单通道波束 + 目标存在标志（NIN1_FLAG）。
 *
 * 注意 Process 的 in_samples 是**每通道**样本数（= rate/100），输出恒为
 * in_samples 个单通道样本，且没有 max_out 参数 —— 输出缓冲不够就是越界写，
 * 所以 out_frame_len 必须等于 frame_len（NIN1_FLAG 的工厂分支保证了这点）。 */

/* 默认麦阵几何（米）：2 麦沿 x 轴 ±10mm。**占位几何** —— 拿到板子实测坐标后
 * 只改这张表。几何不进配置参数：文本配置里没有数组语法（见
 * docs/algorithm_unified_api.md 第 8 节 F32_ARR 后置项），而坐标写错会让
 * 波束指到错误方向且**不报任何错**，正是最该放在代码里被 review 的一类值。 */
static const BfMicPosition g_bf_mic_pos_2[2] = {
    { -0.010f, 0.0f, 0.0f },
    { 0.010f, 0.0f, 0.0f },
};

static int shim_bf(void *h, const int16_t *in, const int16_t *ref, int in_samples, int16_t *out,
                   int max_out, int *out_samples, int *flag)
{
    (void)ref;
    (void)max_out;
    (void)out_samples;
    return AudioEngine_Bf_Process((BfHandle)h, in, in_samples, out, flag);
}

static aura_err_t bf_init_cfg(const aura_algo_params_t *p, void *cfg, uint32_t cap)
{
    if (cap < sizeof(BfInitConfig)) {
        return AURA_ERR_INVALID_ARG;
    }
    if (p->in_channels != 2) {
        /* 默认几何只定义了 2 麦。补齐坐标前不要放行 3 路以上 —— 拿一组重复的
         * 坐标去骗过 Init，得到的是指向错误的波束，比拒绝装配糟得多。 */
        AURA_LOGE(TAG, "bf: 默认几何只有 2 麦，当前 %u 路（补 g_bf_mic_pos_* 后再放开）",
                  p->in_channels);
        return AURA_ERR_INVALID_ARG;
    }
    BfInitConfig *c = (BfInitConfig *)cfg;
    memset(c, 0, sizeof(*c));
    c->sample_rate = (int)p->sample_rate;
    c->num_channels = (int)p->in_channels;
    c->frame_len    = (int)p->sample_rate / 100;
    c->mic_pos      = (BfMicPosition *)g_bf_mic_pos_2; /* Init 内部拷贝，可指向静态表 */
    c->target_azimuth   = aura_algo_params_f32(p, "target_azimuth", (float)(M_PI / 2.0));
    c->target_elevation = aura_algo_params_f32(p, "target_elevation", 0.0f);
    return AURA_OK;
}

static aura_err_t bf_rt_cfg(const aura_algo_params_t *p, void *cfg, uint32_t cap)
{
    if (cap < sizeof(BfRtConfig)) {
        return AURA_ERR_INVALID_ARG;
    }
    BfRtConfig *c = (BfRtConfig *)cfg;
    memset(c, 0, sizeof(*c));
    /* SetParam 是整组覆盖：这里必须把 Init 时的取值原样再写一遍，
     * 否则"Init 里设了方位角、SetParam 又把它归零"，而两次调用看起来都对。 */
    c->target_azimuth   = aura_algo_params_f32(p, "target_azimuth", (float)(M_PI / 2.0));
    c->target_elevation = aura_algo_params_f32(p, "target_elevation", 0.0f);
    return AURA_OK;
}

/* ============================== AGC（经典）==============================
 * 1→1，且**只能单声道**：经典 AGC 内部只有一个 WebRtcAgc 实例，没有通道字段
 * （audio_engine_agc.cpp: `const int16_t* bands[] = { audio_in };`）。2 路输入
 * 不会报错，只会静默地只处理 CH0 —— 所以在这里拒绝，让装配期就失败。
 * 链上它排在 BF 之后，正常情况就是 1 路。 */
static int shim_agc(void *h, const int16_t *in, const int16_t *ref, int in_samples, int16_t *out,
                    int max_out, int *out_samples, int *flag)
{
    (void)ref;
    (void)flag;
    return AudioEngine_Agc_Process((AgcHandle)h, in, in_samples, out, max_out, out_samples);
}

static aura_err_t agc_init_cfg(const aura_algo_params_t *p, void *cfg, uint32_t cap)
{
    if (cap < sizeof(AgcInitConfig)) {
        return AURA_ERR_INVALID_ARG;
    }
    if (p->in_channels != 1) {
        AURA_LOGE(TAG, "agc: 经典 AGC 是单声道实现，当前 %u 路（链上应排在 bf 之后）",
                  p->in_channels);
        return AURA_ERR_INVALID_ARG;
    }
    AgcInitConfig *c = (AgcInitConfig *)cfg;
    memset(c, 0, sizeof(*c));
    c->sample_rate = (int)p->sample_rate;
    /* 默认值与 TrickRoom 自己的单测一致（AdaptiveDigital + 满幅 mic 范围），
     * 这样"引擎能跑成什么样"与"框架跑成什么样"可直接对照。 */
    c->agc_mode  = aura_algo_params_i32(p, "agc_mode", 2);
    c->min_level = 0;
    c->max_level = 255;
    return AURA_OK;
}

static aura_err_t agc_rt_cfg(const aura_algo_params_t *p, void *cfg, uint32_t cap)
{
    if (cap < sizeof(AgcRtConfig)) {
        return AURA_ERR_INVALID_ARG;
    }
    AgcRtConfig *c = (AgcRtConfig *)cfg;
    memset(c, 0, sizeof(*c));
    /* 这三个字段用 -1 表示"保持当前"，所以默认值必须显式写成引擎单测的那组，
     * 不能靠 memset 的 0 —— 0 dB 压缩 + 关闭 limiter 是"没在调音量"，
     * 而不是"用默认值"。 */
    c->compression_gain_db = aura_algo_params_i32(p, "compression_gain_db", 9);
    c->limiter_enable      = aura_algo_params_i32(p, "limiter_enable", 1);
    c->target_level_dbfs   = aura_algo_params_i32(p, "target_level_dbfs", 3);
    return AURA_OK;
}

/* ================================ SRC ================================
 * 变采样率（RESAMPLE）：适配层不覆盖它的输出 pts，由它按输入 pts + 已消费样本
 * 自算（见 node.h 的 pts 契约），并同步改写帧头 sample_rate。 */
static int shim_src(void *h, const int16_t *in, const int16_t *ref, int in_samples, int16_t *out,
                    int max_out, int *out_samples, int *flag)
{
    (void)ref;
    (void)flag;
    return AudioEngine_Resample_Process((ResampleHandle)h, in, in_samples, out, max_out,
                                        out_samples);
}

static aura_err_t src_init_cfg(const aura_algo_params_t *p, void *cfg, uint32_t cap)
{
    if (cap < sizeof(ResampleInitConfig)) {
        return AURA_ERR_INVALID_ARG;
    }
    int out_rate = aura_algo_params_i32(p, "out_sample_rate", 16000);
    if (out_rate <= 0 || out_rate > 192000) {
        return AURA_ERR_INVALID_ARG;
    }
    if ((uint32_t)out_rate == p->sample_rate) {
        /* 恒等重采样：引擎接受，但它只是把帧原样搬一遍。多半是配置写错
         * （比如把链首采样率也改成了目标值），说一声。 */
        AURA_LOGW(TAG, "src: 输入输出采样率都是 %u，这一步是恒等变换", p->sample_rate);
    }
    if (p->in_channels != 1 && p->in_channels != 2) {
        AURA_LOGE(TAG, "src: 引擎只支持 1|2 通道，当前 %u", p->in_channels);
        return AURA_ERR_INVALID_ARG;
    }
    ResampleInitConfig *c = (ResampleInitConfig *)cfg;
    memset(c, 0, sizeof(*c));
    c->src_sample_rate = (int)p->sample_rate;
    c->dst_sample_rate = out_rate;
    c->num_channels    = (int)p->in_channels;
    return AURA_OK;
}

static aura_err_t src_rt_cfg(const aura_algo_params_t *p, void *cfg, uint32_t cap)
{
    (void)p;
    if (cap < sizeof(ResampleRtConfig)) {
        return AURA_ERR_INVALID_ARG;
    }
    memset(cfg, 0, sizeof(ResampleRtConfig)); /* reserved，保持 0 */
    return AURA_OK;
}

/* ============================== 参数规格 ============================== */

static const aura_algo_param_spec_t AEC_SPECS[] = {
    { "delay_ms", AURA_ALGO_PARAM_I32, -1.f, 500.f, "回采延迟(ms)，-1=引擎自估" },
};

static const aura_algo_param_spec_t BF_SPECS[] = {
    { "target_azimuth", AURA_ALGO_PARAM_F32, -(float)M_PI, (float)M_PI,
      "目标方位角(rad)，默认 π/2（正前方）" },
    { "target_elevation", AURA_ALGO_PARAM_F32, -(float)(M_PI / 2.0), (float)(M_PI / 2.0),
      "目标俯仰角(rad)，默认 0" },
};

static const aura_algo_param_spec_t NS_SPECS[] = {
    { "suppression_level", AURA_ALGO_PARAM_I32, 0.f, 3.f, "0=6dB 1=12dB 2=18dB 3=21dB" },
};

static const aura_algo_param_spec_t AGC_SPECS[] = {
    { "agc_mode", AURA_ALGO_PARAM_I32, 0.f, 3.f, "0=不变 1=模拟自适应 2=数字自适应 3=固定增益" },
    { "compression_gain_db", AURA_ALGO_PARAM_I32, 0.f, 90.f, "压缩增益(dB)，默认 9" },
    { "limiter_enable", AURA_ALGO_PARAM_I32, 0.f, 1.f, "限幅器开关，默认 1" },
    { "target_level_dbfs", AURA_ALGO_PARAM_I32, 0.f, 31.f, "目标电平(dBFS)，默认 3" },
};

static const aura_algo_param_spec_t SRC_SPECS[] = {
    { "out_sample_rate", AURA_ALGO_PARAM_I32, 8000.f, 48000.f, "输出采样率，默认 16000" },
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
        /* AEC 是唯一用"交错总数"口径的引擎（frame_size = rate/100 × 通道数）。 */
        .interleaved_total = true,
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
        .name          = "bf",
        .io_kind       = AURA_DSP_IO_NIN1_FLAG,
        .map_status    = trickroom_map_status,
        .create        = AudioEngine_Bf_create,
        .destroy       = AudioEngine_Bf_destroy,
        .init          = AudioEngine_Bf_init,
        .set_param     = AudioEngine_Bf_set_param,
        .reset_param   = AudioEngine_Bf_reset_param,
        .deinit        = AudioEngine_Bf_deinit,
        .reset         = AudioEngine_Bf_reset,
        .process       = shim_bf,
        .param_specs   = BF_SPECS,
        .param_spec_count = sizeof(BF_SPECS) / sizeof(BF_SPECS[0]),
        .build_init_cfg = bf_init_cfg,
        .init_cfg_size  = sizeof(BfInitConfig),
        .build_rt_cfg   = bf_rt_cfg,
        .rt_cfg_size    = sizeof(BfRtConfig),
        /* is_target_present 没有对应的标准事件：它是"波束是否对准了目标"，
         * 不是"有没有人说话"，误当成 VAD 用会打断错误。边沿计数仍在
         * stats.flag_rise/fall 里，需要时从统计读。 */
        .flag_rise_type = AURA_EVENT_NONE,
        .flag_fall_type = AURA_EVENT_NONE,
        .drop_on_error  = false, /* 无透传路径（通道数变化），此项不参与 */
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
        .name          = "agc",
        .io_kind       = AURA_DSP_IO_1TO1,
        .map_status    = trickroom_map_status,
        .create        = AudioEngine_Agc_create,
        .destroy       = AudioEngine_Agc_destroy,
        .init          = AudioEngine_Agc_init,
        .set_param     = AudioEngine_Agc_set_param,
        .reset_param   = AudioEngine_Agc_reset_param,
        .deinit        = AudioEngine_Agc_deinit,
        .reset         = AudioEngine_Agc_reset,
        .process       = shim_agc,
        .param_specs   = AGC_SPECS,
        .param_spec_count = sizeof(AGC_SPECS) / sizeof(AGC_SPECS[0]),
        .build_init_cfg = agc_init_cfg,
        .init_cfg_size  = sizeof(AgcInitConfig),
        .build_rt_cfg   = agc_rt_cfg,
        .rt_cfg_size    = sizeof(AgcRtConfig),
        .drop_on_error  = false, /* 增益挂掉：不调音量的语音远好于静音 */
    },
    {
        .name          = "src",
        .io_kind       = AURA_DSP_IO_RESAMPLE,
        .map_status    = trickroom_map_status,
        .create        = AudioEngine_Resample_create,
        .destroy       = AudioEngine_Resample_destroy,
        .init          = AudioEngine_Resample_init,
        .set_param     = AudioEngine_Resample_set_param,
        .reset_param   = AudioEngine_Resample_reset_param,
        .deinit        = AudioEngine_Resample_deinit,
        .reset         = AudioEngine_Resample_reset,
        .process       = shim_src,
        .param_specs   = SRC_SPECS,
        .param_spec_count = sizeof(SRC_SPECS) / sizeof(SRC_SPECS[0]),
        .build_init_cfg = src_init_cfg,
        .init_cfg_size  = sizeof(ResampleInitConfig),
        .build_rt_cfg   = src_rt_cfg,
        .rt_cfg_size    = sizeof(ResampleRtConfig),
        .drop_on_error  = false, /* 无透传路径（帧长变化），此项不参与 */
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

aura_err_t aura_dsp_trickroom_register(void)
{
    uint32_t count = 0;
    aura_dsp_desc_t *descs = aura_dsp_trickroom_descs(&count);

    for (uint32_t i = 0; i < count; i++) {
        /* 幂等：注册表是进程级的，agent init/deinit/init 往返或上层先注册了
         * 同名 mock（host_sim 的 silero 就是这样）时，重名不该让整个 init 失败。
         * 静默跳过是安全的 —— 名字已经被占了，说明这个算法在链上是可用的，
         * "谁提供的实现"由注册顺序决定，这与 aura_algo_register 的重名语义
         * （拒绝覆盖）不冲突：这里根本不尝试覆盖。 */
        if (aura_algo_find(descs[i].name) != NULL) {
            continue;
        }
        aura_err_t rc = aura_dsp_finalize(&descs[i]);
        if (rc != AURA_OK) {
            AURA_LOGE(TAG, "注册 %s 失败: %s", descs[i].name, aura_strerror(rc));
            return rc;
        }
    }
    return AURA_OK;
}
