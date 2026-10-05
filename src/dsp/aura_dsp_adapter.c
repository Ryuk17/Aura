#include "dsp/aura_dsp_adapter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils/logger/logger.h"
#include "utils/ringbuf/ringbuf.h"

#define TAG "dsp"

/* ---------------------------------------------------------------------------
 * 缓冲尺寸
 *
 * in_buf 要装下"上一轮攒剩的尾巴 + 一个新帧"，否则长帧输入会被拒。
 * AURA_AUDIO_FRAME_MAX_BYTES 限定单帧 ≤4096 个 int16，故 8192 样本足够。
 * ref_ring 是 AEC 参考环：8k 样本 @48k ≈ 170ms，远超 AEC3 的滤波器跨度。
 * ref_frame 是取出的一帧参考（引擎只认连续内存，环上可能跨界）。
 * 每节点静态占用约 64KB —— 板端 BSS 可接受（DSP 节点个位数）。
 * ------------------------------------------------------------------------ */
#define DSP_MAX_SAMPLES 8192 /* 交错样本总数 */

typedef struct aura_dsp_node {
    aura_node_t            base;
    const aura_dsp_desc_t *desc;
    char                   namebuf[AURA_CHAIN_NAME_MAX];

    /* 工厂收到的参数快照：init 在 pipeline_start 时才调用，那时栈上的 params
     * 早已失效，而 build_init_cfg/build_rt_cfg 还要用它。 */
    aura_algo_params_t params;

    void *h;             /* 引擎句柄 */
    bool  engine_inited; /* Init 成功过 —— Deinit/Destroy 的调用前提 */
    bool  started;

    /* 生效形状（链上传播的结果） */
    uint32_t rate;
    uint32_t out_rate;
    uint32_t in_ch;
    uint32_t out_ch;
    uint32_t frame_len;     /* 输入帧单通道样本数 = rate/100 */
    uint32_t out_frame_len; /* 输出帧单通道样本数 = out_rate/100 */

    /* 攒帧：上游帧长不保证等于引擎要求的 rate/100（NN 节点产出变长 chunk），
     * 这里做一层重切分。以**交错样本**为单位计数。 */
    int16_t  in_buf[DSP_MAX_SAMPLES];
    uint32_t in_fill;

    /* AEC 参考环：参考流来自另一条时钟（播放回采），与近端帧数不保证对齐。
     * 用 utils 的环形缓冲，而不是"数组 + 每帧 memmove 整个尾部"的手写 FIFO ——
     * 后者取一帧的代价是 O(缓冲存量)（每次多搬几百毫秒的样本），环形缓冲是
     * O(帧长) 的一次读。存储内联在节点里（aura_ringbuf_init 的静态分配路径），
     * 全程无 malloc。**不用 TrickRoom 的 ring_buffer.c**：本文件刻意不碰任何
     * 引擎头，AURA_WITH_TRICKROOM=OFF 时（含单测）也要能编能链。 */
    aura_ringbuf_t ref_ring;
    uint8_t        ref_ring_storage[DSP_MAX_SAMPLES * sizeof(int16_t)];
    int16_t        ref_frame[DSP_MAX_SAMPLES]; /* 取参考的目标（引擎只认连续内存） */

    int16_t out_buf[DSP_MAX_SAMPLES];

    /* 标志输出（VAD/BF）的边沿检测 */
    int  last_flag;
    bool flag_valid;

    /* pts 基准：变帧率节点的输出 pts 由这里自算（见 node.h 的 pts 契约）。
     * 普通节点的产出 pts 会被 pipeline 覆盖，这里只是给透传路径一个合理值。 */
    uint64_t base_pts_us;
    bool     base_pts_valid;

    bool             rate_warned;  /* 形状不符只在首次告警，避免每帧刷日志 */
    bool             proc_err_logged; /* 引擎处理失败同样只在首次记日志 */
    aura_dsp_stats_t stats;
} aura_dsp_node_t;

/* ============================== 错误映射 ============================== */

static aura_err_t map_status(const aura_dsp_desc_t *d, int status)
{
    if (status == 0) {
        return AURA_OK;
    }
    return (d->map_status != NULL) ? d->map_status(status) : AURA_ERR_DSP;
}

/* ============================== 形状钩子 ============================== */

/* RESAMPLE 的输出采样率：读 extra 参数，缺省 16000（链路主采样率）。 */
static aura_err_t shape_out_rate(const aura_algo_params_t *params, uint32_t *rate, uint32_t *ch)
{
    (void)ch;
    int32_t out = aura_algo_params_i32(params, "out_sample_rate", 16000);
    if (out <= 0 || out > 192000) {
        return AURA_ERR_INVALID_ARG;
    }
    *rate = (uint32_t)out;
    return AURA_OK;
}

/* NIN1（BF）：多麦合成单通道波束，采样率不变。 */
static aura_err_t shape_out_channels(const aura_algo_params_t *params, uint32_t *rate,
                                     uint32_t *ch)
{
    (void)params;
    (void)rate;
    *ch = 1;
    return AURA_OK;
}

/* ============================== 生命周期 ============================== */

/* 配置块上限：TrickRoom 的 InitConfig/RtConfig 都是几个 int 的 POD，
 * 128 字节有充裕余量；超出则截断到容量并让 build_*_cfg 自己判断。 */
#define DSP_CFG_CAP 128

static aura_err_t dsp_build_and_apply(const aura_dsp_node_t *n,
                                      aura_err_t (*build)(const aura_algo_params_t *, void *,
                                                          uint32_t),
                                      uint32_t size, int (*apply)(void *, const void *),
                                      const char *what)
{
    if (apply == NULL || size == 0) {
        return AURA_OK; /* 该算法没有这一步（如 VAD 无 RtConfig） */
    }
    uint8_t cfg[DSP_CFG_CAP];
    memset(cfg, 0, sizeof(cfg));
    uint32_t cap = (size <= sizeof(cfg)) ? size : (uint32_t)sizeof(cfg);

    /* 没有 build 就用零值配置调 apply：声明了 size 就是"有这一步"，
     * 因为缺一个可选的 builder 而整个跳过 Init，是没人会注意到的静默失效。 */
    if (build != NULL) {
        aura_err_t rc = build(&n->params, cfg, cap);
        if (rc != AURA_OK) {
            AURA_LOGE(TAG, "%s: build %s failed: %s", n->base.name, what, aura_strerror(rc));
            return rc;
        }
    }
    int st = apply(n->h, cfg);
    if (st != 0) {
        return map_status(n->desc, st);
    }
    return AURA_OK;
}

static aura_err_t dsp_init(aura_node_t *self)
{
    aura_dsp_node_t       *n = (aura_dsp_node_t *)self;
    const aura_dsp_desc_t *d = n->desc;

    if (d->create == NULL || d->process == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    n->h = d->create();
    if (n->h == NULL) {
        aura_node_report_error(self, AURA_ERR_DSP, "engine create failed");
        return AURA_ERR_DSP;
    }

    aura_err_t rc = dsp_build_and_apply(n, d->build_init_cfg, d->init_cfg_size, d->init, "init cfg");
    if (rc != AURA_OK) {
        aura_node_report_error(self, rc, "engine init failed");
        return rc;
    }
    n->engine_inited = (d->init != NULL);

    rc = dsp_build_and_apply(n, d->build_rt_cfg, d->rt_cfg_size, d->set_param, "rt cfg");
    if (rc != AURA_OK) {
        aura_node_report_error(self, rc, "engine set_param failed");
        return rc;
    }
    return AURA_OK;
}

static aura_err_t dsp_stop(aura_node_t *self)
{
    aura_dsp_node_t       *n = (aura_dsp_node_t *)self;
    const aura_dsp_desc_t *d = n->desc;
    if (n->h != NULL && d->deinit != NULL && n->engine_inited) {
        d->deinit(n->h);
        n->engine_inited = false;
    }
    n->started = false;
    return AURA_OK;
}

static void dsp_deinit(aura_node_t *self)
{
    aura_dsp_node_t       *n = (aura_dsp_node_t *)self;
    const aura_dsp_desc_t *d = n->desc;
    if (n->h == NULL) {
        return;
    }
    if (d->destroy != NULL) {
        d->destroy(n->h); /* TrickRoom 约定：Destroy 内部保证 Deinit */
    }
    n->h             = NULL;
    n->engine_inited = false;
}

static aura_err_t dsp_reset_state(aura_node_t *self)
{
    aura_dsp_node_t       *n = (aura_dsp_node_t *)self;
    const aura_dsp_desc_t *d = n->desc;
    if (n->h != NULL && d->reset != NULL && n->engine_inited) {
        int st = d->reset(n->h);
        if (st != 0) {
            return map_status(d, st);
        }
    }
    /* 自有缓冲一并清空：FLUSH 语义是"丢弃在途数据"，只清引擎不清适配层，
     * 下一帧就会把打断前的半帧语音接上去。 */
    n->in_fill        = 0;
    aura_ringbuf_reset(&n->ref_ring);
    n->flag_valid     = false;
    n->last_flag      = 0;
    n->base_pts_valid = false;
    return AURA_OK;
}

static aura_err_t dsp_control(aura_node_t *self, aura_node_cmd_t cmd, const void *arg)
{
    (void)arg;
    switch (cmd) {
    case AURA_CMD_START:
        ((aura_dsp_node_t *)self)->started = true;
        return AURA_OK;
    case AURA_CMD_STOP:
        ((aura_dsp_node_t *)self)->started = false;
        return AURA_OK;
    case AURA_CMD_RESET:
    case AURA_CMD_FLUSH:
        return dsp_reset_state(self);
    default:
        return AURA_ERR_UNSUPPORTED;
    }
}

/* ============================== 标志输出 ============================== */

static void dsp_handle_flag(aura_node_t *self, int flag)
{
    aura_dsp_node_t       *n = (aura_dsp_node_t *)self;
    const aura_dsp_desc_t *d = n->desc;
    int                    v = (flag != 0) ? 1 : 0;

    if (n->flag_valid && v == n->last_flag) {
        return; /* 只发边沿：每帧发事件会刷爆总线，下游要的本来就是状态变化 */
    }
    bool rise     = (v != 0);
    n->flag_valid = true;
    n->last_flag  = v;
    if (rise) {
        n->stats.flag_rise++;
    } else {
        n->stats.flag_fall++;
    }
    int type = rise ? d->flag_rise_type : d->flag_fall_type;
    if (type != AURA_EVENT_NONE) {
        aura_node_post_event(self, type, v);
    }
}

/* ============================== 引擎调用 ============================== */

static int dsp_run_engine(aura_dsp_node_t *n, const int16_t *in, const int16_t *ref, int16_t *out,
                          int *out_samples, int *flag)
{
    const aura_dsp_desc_t *d         = n->desc;
    const int              fl        = (int)n->frame_len;          /* 单通道帧长 */
    const int              total     = (int)(n->frame_len * n->in_ch); /* 交错总数 */
    const int              in_samples = d->interleaved_total ? total : fl;
    /* 输出缓冲的交错容量：NS 要求 ≥ in_samples × 通道数，AEC 要求 ≥ frame_size
     * （两者都等于 total）；其余引擎只要够大即可。 */
    const int              max_out   = (int)n->out_frame_len * (int)n->out_ch;

    *out_samples = 0; /* BF/VAD 的引擎不写这个字段，先归零再交给引擎 */

    if (d->io_kind == AURA_DSP_IO_1TO0) {
        return d->process(n->h, in, NULL, in_samples, NULL, 0, out_samples, flag);
    }
    return d->process(n->h, in, ref, in_samples, out, max_out, out_samples, flag);
}

/* 取一帧参考（AEC）：不足则用静音顶替并计数。
 * 不在这里做精确延迟估计 —— 那要等 AEC 自己报估计结果，属于 Phase 2 调参阶段；
 * 现在保证"AEC 永远拿得到一帧参考"，缺参考时降级为不消除而非错位/崩溃。 */
static const int16_t *dsp_take_ref(aura_dsp_node_t *n, uint32_t samples)
{
    static const int16_t silence[DSP_MAX_SAMPLES] = {0};

    if (n->desc->io_kind != AURA_DSP_IO_2TO1) {
        return silence;
    }
    /* 一帧的交错样本数；调用方传的是单通道帧长（见 dsp_run_engine 的 in_samples 口径）。
     * 工厂已保证 frame_len × in_ch ≤ DSP_MAX_SAMPLES，故 ref_frame 装得下。 */
    const uint32_t need = samples * n->in_ch;
    if (need == 0 || aura_ringbuf_avail_read(&n->ref_ring) < need * sizeof(int16_t)) {
        n->stats.ref_missed++;
        return silence;
    }
    aura_ringbuf_read(&n->ref_ring, n->ref_frame, need * sizeof(int16_t));
    /* 取完剩下的 = 当前近端帧配到的参考领先了多少。恒为 0 才叫"近端配当前参考"；
     * 一直有值说明参考流跑在前面，AEC 拿到的是未来的回声，必然消不掉。 */
    const uint32_t left = aura_ringbuf_avail_read(&n->ref_ring) / sizeof(int16_t);
    if (left > n->stats.ref_lead_max) {
        n->stats.ref_lead_max = left;
    }
    return n->ref_frame;
}

static void dsp_emit(aura_node_t *self, uint32_t samples, const int16_t *data, uint32_t rate,
                     uint32_t ch)
{
    aura_audio_frame_t g;
    memset(&g, 0, sizeof(g));
    g.pts_us      = ((aura_dsp_node_t *)self)->base_pts_us;
    g.sample_rate = rate;
    g.channels    = (uint16_t)ch;
    g.frame_count = (uint16_t)samples;
    g.fmt         = AURA_SAMPLE_S16;
    g.data_bytes  = samples * ch * sizeof(int16_t);
    g.data        = (uint8_t *)data;
    aura_node_emit_audio(self, &g);
}

/* 消费 in_buf 里攒够的整帧，逐帧跑引擎并产出。 */
static aura_err_t dsp_drain_frames(aura_dsp_node_t *n, aura_node_t *self)
{
    const aura_dsp_desc_t *d     = n->desc;
    const uint32_t         fl    = n->frame_len;
    const uint32_t         fb    = fl * n->in_ch; /* 一帧的交错样本数 */
    aura_err_t             first = AURA_OK;

    while (n->in_fill >= fb) {
        const int16_t *ref         = dsp_take_ref(n, fl);
        int            out_samples = 0;
        int            flag        = 0;
        int            st          = dsp_run_engine(n, n->in_buf, ref, n->out_buf, &out_samples,
                                        &flag);

        n->stats.frames_in++;
        n->stats.samples_in += fl;

        if (st != 0) {
            aura_err_t rc = map_status(d, st);
            n->stats.process_errors++;
            if (!n->proc_err_logged) {
                /* 只在首次记日志。**不在这里 report_error**：本函数把错误返回给
                 * pipeline，它已经代为上抛并计数 —— 再报一次会让总线每失败一帧
                 * 收到两个 ERROR 事件。 */
                n->proc_err_logged = true;
                AURA_LOGW(TAG, "%s: engine process failed (%d -> %s), degrade", self->name, st,
                          aura_strerror(rc));
            }
            if (first == AURA_OK) {
                first = rc; /* 首个错误返回给 pipeline（多帧出错只上抛一次） */
            }

            /* 降级：把这一帧原样透传下去（3A 挂掉时"没降噪的语音"远好于静音）。
             * 必须在 memmove 之前发 —— 此时帧还躺在 in_buf 头部。
             * 只对形状不变的算法成立：RESAMPLE 帧长会变、NIN1 通道数会变、
             * 1TO0 本就没有产出，这三类出错时只能丢帧。 */
            if (!d->drop_on_error && d->io_kind == AURA_DSP_IO_1TO1) {
                dsp_emit(self, fl, n->in_buf, n->rate, n->in_ch);
            }
        } else {
            if (d->io_kind == AURA_DSP_IO_1TO0) {
                /* VAD 类：产出的是标志不是音频帧。计进 frames_out 会让
                 * "这个节点出了 3 帧音频"成为一句假话。 */
                dsp_handle_flag(self, flag);
            } else {
                n->stats.frames_out++;
                if (d->io_kind == AURA_DSP_IO_NIN1_FLAG) {
                    dsp_handle_flag(self, flag);
                }
                uint32_t nout = (out_samples > 0) ? (uint32_t)out_samples : n->out_frame_len;
                if (d->interleaved_total) {
                    /* 引擎按交错总数写回（AEC 的 *out_samples = in_samples），
                     * 而 emit 要的是每通道帧数。 */
                    nout /= n->in_ch; /* in_ch ≥ 1，工厂已校验 */
                }
                if (nout > n->out_frame_len) {
                    nout = n->out_frame_len; /* 引擎给多了：截断，宁可少样本不可越界 */
                }
                dsp_emit(self, nout, n->out_buf, n->out_rate, n->out_ch);
            }
        }

        /* 摘掉已消费的一帧。放在错误分支之后：这样"出错重试"不可能发生 ——
         * 同一帧反复重试会变成死循环。 */
        memmove(n->in_buf, n->in_buf + fb, (size_t)(n->in_fill - fb) * sizeof(int16_t));
        n->in_fill -= fb;

        /* 输出 pts 逐帧推进（变帧率节点不得不用它；定帧率节点会被 pipeline 覆盖） */
        n->base_pts_us += (uint64_t)fl * 1000000ull / (n->rate ? n->rate : 1);
    }
    return first;
}

/* ============================== 节点回调 ============================== */

static aura_err_t dsp_process_audio(aura_node_t *self, const aura_audio_frame_t *f)
{
    aura_dsp_node_t *n = (aura_dsp_node_t *)self;

    if (f->fmt != AURA_SAMPLE_S16) {
        aura_node_report_error(self, AURA_ERR_UNSUPPORTED, "dsp engine needs s16 pcm");
        return AURA_ERR_UNSUPPORTED;
    }
    if (f->channels != n->in_ch) {
        /* 通道数不符 = 链装配错误（缺 BF 或上游写错形状），不是运行期抖动；
         * 逐帧上报会刷爆总线，故只报一次。 */
        if (!n->rate_warned) {
            n->rate_warned = true;
            aura_node_report_error(self, AURA_ERR_INVALID_ARG, "channel count mismatch");
        }
        return AURA_ERR_INVALID_ARG;
    }
    if (f->sample_rate != n->rate && !n->rate_warned) {
        n->rate_warned = true;
        AURA_LOGW(TAG, "%s: input rate %u != configured %u", self->name, f->sample_rate, n->rate);
    }

    const uint32_t samples = f->frame_count * n->in_ch;
    if (n->in_fill + samples > DSP_MAX_SAMPLES) {
        /* 攒帧缓冲塞不下：上游帧长超限或链上形状错配。丢掉已攒内容重新对齐，
         * 好过静默溢出或持续报错。 */
        aura_node_report_error(self, AURA_ERR_FULL, "dsp input buffer overflow");
        n->in_fill        = 0;
        n->base_pts_valid = false;
        return AURA_ERR_FULL;
    }

    /* 首帧（或上次排空后）以输入 pts 重新对齐基准；否则沿用上一帧推进后的基准 ——
     * 攒帧残留属于同一段连续音频，pts 不该被下一个输入帧重置。 */
    if (!n->base_pts_valid) {
        n->base_pts_us    = f->pts_us;
        n->base_pts_valid = true;
    }

    memcpy(n->in_buf + n->in_fill, f->data, (size_t)samples * sizeof(int16_t));
    n->in_fill += samples;

    aura_err_t rc = dsp_drain_frames(n, self);

    if (n->in_fill == 0 && !self->caps.changes_frame_rate) {
        /* 排空后重新对齐：下一帧按它自己的 pts 起算，长跑不会累积漂移。
         * 变帧率节点**不**这么做 —— 它的输出是一条自算的时间轴，输入 pts 抖一下
         * 就让输出时间轴跟着跳，下游（播放/对齐）会看到回退的 pts。它只在
         * 首帧与 FLUSH 后重新锚定。 */
        n->base_pts_valid = false;
    }
    return rc;
}

/* 参考流投递：仅缓存，不处理 —— AEC 的参考要按 pts 与近端帧配对，
 * 而配对只有 process_audio 才知道（那时才知道在消哪一帧近端）。 */
static aura_err_t dsp_process_ref_audio(aura_node_t *self, const aura_audio_frame_t *f)
{
    aura_dsp_node_t *n = (aura_dsp_node_t *)self;
    if (f->fmt != AURA_SAMPLE_S16 || f->channels != n->in_ch) {
        return AURA_ERR_INVALID_ARG;
    }
    const uint32_t bytes = f->frame_count * n->in_ch * sizeof(int16_t);
    if (bytes > sizeof(n->ref_ring_storage)) {
        return AURA_ERR_INVALID_ARG;
    }
    /* 参考堆积 = 近端比远端跑得慢。丢最老的保留最新的：AEC 要的是"刚刚播放了
     * 什么"，过期的参考比没有参考更糟（会把旧回声当作当前回声去消）。
     * skip 只推读指针不搬数据；腾够空间后 write 必然全落 —— 与 aura_ringbuf_write
     * 自身"写不下就少写（丢新）"的兜底语义不同，这里不依赖那个兜底。 */
    const uint32_t space = aura_ringbuf_avail_write(&n->ref_ring);
    if (bytes > space) {
        const uint32_t drop = bytes - space;
        aura_ringbuf_skip(&n->ref_ring, drop);
        n->stats.ref_stale += drop / sizeof(int16_t); /* 配对从此永久前移，必须可见 */
    }
    const uint32_t wrote = aura_ringbuf_write(&n->ref_ring, f->data, bytes);
    if (wrote != bytes) {
        /* 腾过空间后不该发生；真发生说明容量账算错了，宁可让统计显形。 */
        n->stats.ref_stale += (bytes - wrote) / sizeof(int16_t);
    }
    return AURA_OK;
}

static const aura_node_ops_t DSP_OPS = {
    .init              = dsp_init,
    .stop              = dsp_stop,
    .deinit            = dsp_deinit,
    .control           = dsp_control,
    .process_audio     = dsp_process_audio,
    .process_ref_audio = dsp_process_ref_audio,
};

/* ============================== 工厂 ============================== */

/* 全族共用一个工厂：靠 params->provider 取回自己的描述表。
 * aura_dsp_desc_t 的**首成员就是内嵌的 algo**，故指针可直接互转。 */
static aura_node_t *dsp_create_node(const aura_algo_params_t *params, aura_err_t *err)
{
    if (params == NULL || params->provider == NULL) {
        if (err) {
            *err = AURA_ERR_INVALID_ARG;
        }
        return NULL;
    }
    const aura_dsp_desc_t *d = (const aura_dsp_desc_t *)params->provider;
    if (d->process == NULL) {
        if (err) {
            *err = AURA_ERR_INVALID_ARG;
        }
        return NULL;
    }

    aura_dsp_node_t *n = (aura_dsp_node_t *)calloc(1, sizeof(*n));
    if (n == NULL) {
        if (err) {
            *err = AURA_ERR_NOMEM;
        }
        return NULL;
    }
    n->desc  = d;
    n->rate  = params->sample_rate;
    n->in_ch = params->in_channels;

    /* 参考环（AEC 用）：非 2TO1 形态不读不写，但存储是结构体内联的，一律初始化
     * 成本为零。容量是编译期常量 2 的幂，init 不可能失败。 */
    (void)aura_ringbuf_init(&n->ref_ring, n->ref_ring_storage, sizeof(n->ref_ring_storage));

    /* 生效形状必须有意义：采样率/通道数为 0 只可能来自装配错误。 */
    if (n->rate == 0 || n->in_ch == 0 || n->in_ch > 8) {
        free(n);
        if (err) {
            *err = AURA_ERR_INVALID_ARG;
        }
        return NULL;
    }
    n->frame_len = n->rate / 100; /* TrickRoom 硬约束：10ms 帧 */

    switch (d->io_kind) {
    case AURA_DSP_IO_RESAMPLE:
        n->out_rate      = (uint32_t)aura_algo_params_i32(params, "out_sample_rate", 16000);
        n->out_ch        = n->in_ch;
        n->out_frame_len = n->out_rate / 100;
        break;
    case AURA_DSP_IO_NIN1_FLAG:
        n->out_rate      = n->rate;
        n->out_ch        = 1;
        n->out_frame_len = n->frame_len;
        break;
    default:
        n->out_rate      = n->rate;
        n->out_ch        = n->in_ch;
        n->out_frame_len = n->frame_len;
        break;
    }
    if (n->out_frame_len == 0 || n->out_frame_len * n->out_ch > DSP_MAX_SAMPLES) {
        free(n);
        if (err) {
            *err = AURA_ERR_INVALID_ARG;
        }
        return NULL;
    }

    /* 参数快照：init 在 pipeline_start 时才跑，那时栈上的 params 早已失效。
     * 字符串成员按契约指向程序级生命周期的存储（见 aura_algo_params_t）。 */
    n->params             = *params;
    snprintf(n->namebuf, sizeof(n->namebuf), "%s",
             (params->instance_name != NULL) ? params->instance_name : d->name);
    n->params.instance_name = n->namebuf; /* 不再指向中转瞬即逝的链描述 */
    n->params.provider      = NULL;       /* 快照里不留描述表指针，避免误导 */

    aura_node_caps_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.consumes_audio     = true;
    caps.consumes_ref_audio = (d->io_kind == AURA_DSP_IO_2TO1);
    /* 通道数变化不进 caps：它只影响装配期的形状传播（io.changes_channels），
     * pipeline 运行期不关心通道数，只关心变帧率与 pts 的关系。 */
    caps.changes_frame_rate = (d->io_kind == AURA_DSP_IO_RESAMPLE);
    caps.produces_audio     = (d->io_kind != AURA_DSP_IO_1TO0);
    aura_node_init(&n->base, n->namebuf, &DSP_OPS, &caps);
    return &n->base;
}

/* ============================== 注册 ============================== */

aura_err_t aura_dsp_finalize(aura_dsp_desc_t *desc)
{
    if (desc == NULL || desc->name == NULL || desc->process == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    aura_algo_io_t io;
    memset(&io, 0, sizeof(io));
    io.consumes_audio = true;

    switch (desc->io_kind) {
    case AURA_DSP_IO_1TO1:
        io.produces_audio = true;
        break;
    case AURA_DSP_IO_2TO1:
        io.produces_audio = true;
        io.consumes_ref   = true;
        break;
    case AURA_DSP_IO_NIN1_FLAG:
        io.produces_audio   = true;
        io.changes_channels = true;
        break;
    case AURA_DSP_IO_1TO0:
        io.produces_audio = false; /* 观察者：挂主线旁路，不做音频终点 */
        break;
    case AURA_DSP_IO_RESAMPLE:
        io.produces_audio     = true;
        io.changes_frame_rate = true;
        break;
    default:
        return AURA_ERR_INVALID_ARG;
    }

    desc->algo.name             = desc->name;
    desc->algo.version          = 1;
    desc->algo.kind             = AURA_ALGO_KIND_DSP;
    desc->algo.io               = io;
    desc->algo.shape_out        = (desc->io_kind == AURA_DSP_IO_RESAMPLE) ? shape_out_rate
                                  : (desc->io_kind == AURA_DSP_IO_NIN1_FLAG)
                                      ? shape_out_channels
                                      : NULL;
    desc->algo.param_specs      = desc->param_specs;
    desc->algo.param_spec_count = desc->param_spec_count;
    desc->algo.create           = dsp_create_node;
    return aura_algo_register(&desc->algo);
}

aura_err_t aura_dsp_register_all(aura_dsp_desc_t *descs, uint32_t count)
{
    if (descs == NULL && count > 0) {
        return AURA_ERR_INVALID_ARG;
    }
    for (uint32_t i = 0; i < count; i++) {
        aura_err_t rc = aura_dsp_finalize(&descs[i]);
        if (rc != AURA_OK) {
            AURA_LOGE(TAG, "dsp desc[%u] (%s) finalize failed: %s", i,
                      descs[i].name ? descs[i].name : "?", aura_strerror(rc));
            return rc;
        }
    }
    return AURA_OK;
}

aura_err_t aura_dsp_get_stats(const aura_node_t *node, aura_dsp_stats_t *out)
{
    if (node == NULL || out == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    const aura_dsp_node_t *n = (const aura_dsp_node_t *)node;
    if (n->desc == NULL || n->desc->process == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    *out = n->stats;
    return AURA_OK;
}
