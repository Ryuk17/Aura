/* Aura — wavtap 观察者节点（实现，见 wavtap.h） */
#include "wavtap.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/algorithm.h"
#include "wav_io.h"

#define TAG "audio_debug"

/* 落盘目录：进程级全局。它影响的是"文件写哪儿"，与节点行为无关，
 * 所以不做成每实例参数（配置语法里也不该出现落盘路径）。 */
static char g_out_dir[512];

#define WAVTAP_CLIP_LEVEL 32112 /* ≈ 0.98 × 32767，与 metrics.c 同一门限 */

typedef struct wavtap_node {
    aura_node_t   base; /* 必须首位：pipeline 用 free(node) 回收 */
    char          instance[AURA_CHAIN_NAME_MAX]; /* 自己的名字（契约要求拷贝） */
    char          wav_path[640];
    wav_writer_t *w;
    bool          open_attempted; /* 开过一次就不再试：目录不存在时会刷屏 */
    wavtap_stats_t st;
} wavtap_node_t;

static void wavtap_open_writer(wavtap_node_t *n, uint32_t rate, uint32_t channels)
{
    n->open_attempted = true;
    if (g_out_dir[0] == '\0') {
        return; /* 只统计不落盘 */
    }
    snprintf(n->wav_path, sizeof(n->wav_path), "%s/%s.wav", g_out_dir, n->instance);
    n->w = wav_writer_open(n->wav_path, rate, channels);
    if (n->w == NULL) {
        fprintf(stderr, "[%s] 无法写 %s（目录存在吗？）—— 本节点只做统计\n", TAG, n->wav_path);
        n->wav_path[0] = '\0';
    }
}

static aura_err_t wavtap_process_audio(aura_node_t *self, const aura_audio_frame_t *f)
{
    wavtap_node_t *n = (wavtap_node_t *)self;
    if (f == NULL || f->fmt != AURA_SAMPLE_S16 || f->channels == 0 || f->frame_count == 0) {
        return AURA_ERR_INVALID_ARG;
    }

    const int16_t *s    = (const int16_t *)f->data;
    const uint32_t ch   = f->channels;
    const uint32_t fc   = f->frame_count;
    const uint32_t nsp  = ch * fc;

    if (n->st.frames_in == 0) {
        n->st.rate     = f->sample_rate;
        n->st.channels = ch;
    }
    n->st.last_rate        = f->sample_rate;
    n->st.last_channels    = ch;
    n->st.last_frame_count = fc;
    n->st.frames_in++;
    n->st.samples_total += fc;

    /* 逐帧能量 + 峰值：一次遍历算完，不为了"整洁"走两遍 —— 每帧 160 样本
     * 看着不多，但这是链上每个节点都在跑的热路径。
     * 除以 nsp（含通道数）是为了与 metrics.h 的口径一致，见那里的注释。 */
    double  sum = 0.0;
    int32_t peak = n->st.peak;
    for (uint32_t i = 0; i < nsp; i++) {
        const int32_t v = s[i];
        const int32_t a = (v < 0) ? -v : v;
        sum += (double)v * (double)v;
        if (a > peak) {
            peak = a;
        }
        if (a >= WAVTAP_CLIP_LEVEL) {
            n->st.clipped++;
        }
    }
    n->st.peak = peak;
    if (n->st.energy_frames < WAVTAP_MAX_FRAMES) {
        n->st.frame_energy[n->st.energy_frames++] = sum / (double)nsp;
    }

    if (n->w == NULL && !n->open_attempted) {
        wavtap_open_writer(n, f->sample_rate, ch);
    }
    if (n->w != NULL) {
        if (wav_writer_write(n->w, s, fc) == AURA_OK) {
            n->st.frames_written++;
        } else {
            n->st.write_errors++;
        }
    }
    return AURA_OK;
}

static void wavtap_deinit(aura_node_t *self)
{
    wavtap_node_t *n = (wavtap_node_t *)self;
    if (n->w != NULL) {
        wav_writer_close(n->w); /* 回填 RIFF/data 长度，否则文件头写着 0 字节 */
        n->w = NULL;
    }
}

static const aura_node_ops_t WAVTAP_OPS = {
    .process_audio = wavtap_process_audio,
    .deinit        = wavtap_deinit,
};

static aura_node_t *wavtap_create(const aura_algo_params_t *params, aura_err_t *err)
{
    wavtap_node_t *n = (wavtap_node_t *)calloc(1, sizeof(*n));
    if (n == NULL) {
        if (err != NULL) {
            *err = AURA_ERR_NOMEM;
        }
        return NULL;
    }
    const char *iname = (params != NULL && params->instance_name != NULL) ? params->instance_name
                                                                         : "wavtap";
    snprintf(n->instance, sizeof(n->instance), "%s", iname);

    aura_node_caps_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.consumes_audio  = true;
    caps.produces_audio  = false;
    caps.consumes_text   = false;
    caps.produces_text   = false;
    caps.consumes_ref_audio = false;
    caps.changes_frame_rate = false;
    /* 名字指向节点自有的缓冲：params->instance_name 只活到装配结束
     * （见 aura_algo_params_t 的契约），直接存指针会变成悬垂。 */
    aura_node_init(&n->base, n->instance, &WAVTAP_OPS, &caps);
    return &n->base;
}

/* kind 借 DSP 一用：wavtap 不跑任何引擎，只借"同步、逐帧"这个语义。
 * 注册表把它们分三类的目的是让装配层知道怎么调度，而 tap 恰恰是最简单的那种
 * 调度 —— 代价是 chain_build 会按 DSP 的规矩要求 frame_ms==10（见其校验），
 * 而调试链本来就是 10ms 帧，这个约束不会真的碍事。 */
static const aura_algo_desc_t WAVTAP_DESC = {
    .name    = "wavtap",
    .version = 1,
    .kind    = AURA_ALGO_KIND_DSP,
    .io = {
        .consumes_audio = true,
        .produces_audio = false,
    },
    .param_specs      = NULL, /* 无私有参数 */
    .param_spec_count = 0,
    .create           = wavtap_create,
};

aura_err_t wavtap_register(void)
{
    if (aura_algo_find(WAVTAP_DESC.name) != NULL) {
        return AURA_OK; /* 幂等：重复 init/多次注册不该失败 */
    }
    return aura_algo_register(&WAVTAP_DESC);
}

void wavtap_set_out_dir(const char *dir)
{
    if (dir == NULL) {
        g_out_dir[0] = '\0';
        return;
    }
    snprintf(g_out_dir, sizeof(g_out_dir), "%s", dir);
}

aura_err_t wavtap_get_stats(const aura_node_t *node, wavtap_stats_t *out)
{
    if (node == NULL || out == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    if (node->ops != &WAVTAP_OPS) {
        return AURA_ERR_INVALID_ARG; /* 不是 wavtap 造的，别硬解释它的 priv */
    }
    *out = ((const wavtap_node_t *)node)->st;
    return AURA_OK;
}
