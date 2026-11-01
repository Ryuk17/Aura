/* Aura — 声纹节点（eres2netv2 embedding + 注册/验证内存库，NN_ASYNC）
 *
 * 线程模型（NN_ASYNC 契约）：
 *   - frame_in（process 任务）：PCM 追加进**常驻环**（6s，环形覆盖最旧）；
 *     从不返回 true —— 没有"窗"概念，job 不由音频驱动；
 *   - on_event（bus 任务）：VAD_SPEECH_END → 投 VERIFY 命令（小 job）；
 *   - control SET_CONFIG（调用线程，注册 API 走这条路）：ENROLL 时**就地**
 *     PCM→fbank（用临时特征实例，不碰 worker 的），把特征装进 job 投递；
 *     REMOVE/CLEAR 投小 job；
 *   - async_run（worker）：VERIFY = 锁内切环尾 0.8~3s → fbank → embedding →
 *     L2 归一化 → 与内存库余弦比对 → 投结果；ENROLL = embedding 入库
 *     （同名取平均）；REMOVE/CLEAR = 改库。**库只归 worker**；
 *   - result_consume（bus 任务）：发 VOICEPRINT_RESULT（passed / score×100 /
 *     payload=spk_id）。状态机按 passed 迁 VOICEPRINT_OK/FAIL。
 *
 * 判定口径：余弦相似度 ≥ threshold 通过；库为空 = 未注册说话人（一律不过）。
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "algorithm/aura_nn_adapter.h"
#include "dsp/aura_feature.h"
#include "inference/inference.h"
#include "utils/logger/logger.h"
#include "voiceprint/aura_voiceprint.h"
#include "voiceprint/vp_lib.h"

#define TAG "vp"

/* EMB_DIM / SPK_ID_MAX / LIB_MAX 在 vp_lib.h（注册/验证共用同一套常量） */
enum {
    AURA_VP_RING_S     = 6,   /* 环时长（秒） */
    AURA_VP_MAX_FRAMES = 300, /* 切片封顶 = 3s @10ms */
    AURA_VP_PCM_BUF    = 4096,
    AURA_VP_PATH_MAX   = 512,
};

/* job 类别（注册已离线化，不再有 ENROLL 任务） */
enum {
    AURA_VP_JOB_VERIFY = 0, /* 验证（数据在环里，job 无载荷） */
    AURA_VP_JOB_REMOVE,     /* 删除说话人（job 带 spk_id） */
    AURA_VP_JOB_CLEAR,      /* 清库 */
};

/* job：定长小结构。 */
typedef struct {
    aura_nn_job_head_t head;
    char spk_id[AURA_VP_SPK_ID_MAX];
} vp_job_t;

typedef struct {
    int32_t type;
    bool    passed;
    int32_t score; /* 余弦 × 100 */
    char    spk_id[AURA_VP_SPK_ID_MAX];
} vp_result_t;

/* 库条目 */
typedef struct {
    char  id[AURA_VP_SPK_ID_MAX];
    float emb[AURA_VP_EMB_DIM];
} vp_entry_t;

/* SET_CONFIG 载荷定义在 aura_voiceprint.h（注册 API 与调用方共用）。 */

/* ============================== 节点状态 ============================== */

typedef struct {
    vp_job_t job; /* 必须第一成员 */

    /* worker 专属 */
    aura_feature_t *feat; /* 切片 → fbank（单句离线用） */
    float           emb[AURA_VP_EMB_DIM];
    vp_entry_t      lib[AURA_VP_LIB_MAX];
    int32_t         lib_count;
    float           threshold;
    float           max_utt_s;
    float           min_utt_s;

    /* 环（process 写；worker 在 ring_lock 内读） */
    aura_mutex_t *ring_lock;
    int16_t       ring[AURA_VP_RING_S * 16000];
    uint32_t      ring_head;
    uint32_t      ring_len;

    /* worker 的切片暂存（192KB —— worker 任务栈只有 16KB，绝不能放栈上；
     * 注意类型必须与 PCM 一致：int32 会按 4 字节搬移，别名成 int16 读时
     * 高半字是零 —— 能量窗口与 fbank 看到的都是"隔一个采样一个 0"的坏音频） */
    int16_t slice_buf[AURA_VP_RING_S * 16000];

    /* worker 的 fbank 暂存（96KB —— 同上：栈上放不下。vp_embed 之前把它放
     * 栈上，验证路径一跑就栈爆，实测破坏相邻堆 → KWS 节点的 fbank vector
     * 被写坏 → 段错误，崩溃栈显示在无关的 AcceptWaveform 里） */
    float feat_buf[AURA_VP_MAX_FRAMES * 80];

    /* process 专属（frame_in 碰） */
    int16_t pcm_buf[AURA_VP_PCM_BUF];
} vp_state_t;

/* ============================== 装配 ============================== */

static vp_state_t *vp_state(aura_nn_node_t *self)
{
    return (vp_state_t *)aura_nn_state(self);
}

static aura_err_t vp_init(aura_nn_node_t *self, aura_infer_model_t *model)
{
    if (model == NULL) {
        AURA_LOGE(TAG, "%s: 需要 embedding 模型（desc.model_file）", self->base.name);
        return AURA_ERR_MODEL;
    }
    vp_state_t *st = vp_state(self);
    memset(st, 0, sizeof(*st));

    /* 输入契约：x f32 [1, -1, 80]（动态 T，逐句 resize）；输出 [1, 192] */
    aura_tensor_info_t ti;
    if (aura_model_input_count(model) != 1 ||
        aura_model_input_info(model, 0, &ti) != AURA_OK || ti.dtype != AURA_DTYPE_F32 ||
        ti.dim_count != 3 || ti.dims[2] != 80) {
        AURA_LOGE(TAG, "%s: embedding 模型输入契约不符（期望 [1,-1,80] f32）",
                  self->base.name);
        return AURA_ERR_MODEL;
    }

    aura_feature_cfg_t fc;
    aura_feature_cfg_default(&fc);
    fc.sample_rate = (int32_t)self->rate;
    st->feat = aura_feature_create(&fc);
    if (st->feat == NULL) {
        return AURA_ERR_NOMEM;
    }

    st->ring_lock = aura_osal_mutex_create();
    if (st->ring_lock == NULL) {
        return AURA_ERR_NOMEM;
    }

    st->threshold = aura_algo_params_f32(&self->params, "threshold", 0.6f);
    st->max_utt_s = aura_algo_params_f32(&self->params, "max_utterance_seconds", 3.0f);
    st->min_utt_s = aura_algo_params_f32(&self->params, "min_utterance_seconds", 0.8f);
    if (st->max_utt_s > AURA_VP_MAX_FRAMES / 100.0f) {
        st->max_utt_s = AURA_VP_MAX_FRAMES / 100.0f;
    }

    /* 库文件：注册离线完成（tools/voiceprint_enroll 写文件），这里只读。
     * 文件缺失 = 空库（合法状态：未注册说话人一律拒绝）。 */
    const aura_algo_param_t *pa = aura_algo_params_get(&self->params, "library_file");
    char lib_path[AURA_VP_PATH_MAX];
    if (pa != NULL && pa->type == AURA_ALGO_PARAM_STR) {
        snprintf(lib_path, sizeof(lib_path), "%s", pa->v.s);
    } else {
        snprintf(lib_path, sizeof(lib_path), "%s/voiceprint/library.bin",
                 (aura_nn_model_root() != NULL) ? aura_nn_model_root() : ".");
    }
    aura_err_t lrc =
        aura_vp_lib_load(lib_path, (aura_vp_entry_t *)st->lib, AURA_VP_LIB_MAX,
                         &st->lib_count);
    if (lrc == AURA_ERR_NOT_FOUND) {
        st->lib_count = 0; /* 空库 */
    } else if (lrc != AURA_OK) {
        AURA_LOGE(TAG, "%s: 库文件读取失败: %s", self->base.name, lib_path);
        return lrc;
    }

    AURA_LOGI(TAG, "%s 就绪: emb=%d 阈值=%.2f 切片 %.1f~%.1fs 库=%d 人（%s）",
              self->base.name, AURA_VP_EMB_DIM, (double)st->threshold,
              (double)st->min_utt_s, (double)st->max_utt_s, st->lib_count, lib_path);
    return AURA_OK;
}

/* ============================== 命令投递 ============================== */

static void vp_push_job(aura_nn_node_t *self, const vp_job_t *j)
{
    if (aura_osal_queue_push(self->job_q, j, AURA_NO_WAIT) != AURA_OK) {
        AURA_LOGW(TAG, "%s: job 队列满，命令丢弃", self->base.name);
    }
}

static void vp_make_job(vp_job_t *j, int32_t kind)
{
    memset(j, 0, sizeof(*j));
    j->head.magic = AURA_NN_JOB_MAGIC;
    j->head.kind  = kind;
}

/* ============================== frame_in / 事件 / 控制 ============================== */

static bool vp_frame_in(aura_nn_node_t *self, const aura_audio_frame_t *frame)
{
    vp_state_t *st = vp_state(self);
    if (frame->frame_count > AURA_VP_PCM_BUF) {
        return false;
    }
    memcpy(st->pcm_buf, frame->data, (size_t)frame->frame_count * 2);

    /* 追加进环（覆盖最旧的） */
    uint32_t cap = AURA_VP_RING_S * 16000;
    aura_osal_mutex_lock(st->ring_lock);
    for (uint32_t i = 0; i < frame->frame_count; i++) {
        st->ring[st->ring_head] = st->pcm_buf[i];
        st->ring_head = (st->ring_head + 1) % cap;
        if (st->ring_len < cap) {
            st->ring_len++;
        }
    }
    aura_osal_mutex_unlock(st->ring_lock);
    return false; /* 永不"攒够一窗"：job 由事件/控制驱动 */
}

static void vp_on_event(aura_nn_node_t *self, const aura_event_t *event)
{
    if (event->type != AURA_EVENT_VAD_SPEECH_END) {
        return;
    }
    /* job（96KB）绝不能放本线程的栈上：on_event 跑在 **4KB** 的 bus 任务栈里，
     * 栈爆实测会破坏相邻堆（KWS 节点的 fbank vector 被写坏 → 段错误）。
     * 写进节点 state 的 job 区（堆上）：frame_in 永不触发模板投递，
     * 这块区域只有 on_event 一个写者。 */
    vp_state_t *st = vp_state(self);
    vp_make_job(&st->job, AURA_VP_JOB_VERIFY);
    vp_push_job(self, &st->job);
}

static aura_err_t vp_control(aura_nn_node_t *self, aura_node_cmd_t cmd, const void *arg)
{
    /* 模板的 nn_control 已处理 START/STOP/RESET/FLUSH（RESET/FLUSH 走队列）；
     * SET_CONFIG 到这里。 */
    if (cmd != AURA_CMD_SET_CONFIG) {
        return AURA_ERR_UNSUPPORTED;
    }
    const aura_vp_set_config_t *cfg = (const aura_vp_set_config_t *)arg;
    if (cfg == NULL) {
        return AURA_ERR_INVALID_ARG;
    }

    vp_job_t j;
    vp_make_job(&j, AURA_VP_JOB_REMOVE); /* 占位，下面按 op 覆写 */

    switch (cfg->op) {
    case AURA_VP_OP_REMOVE:
        if (cfg->spk_id == NULL) {
            return AURA_ERR_INVALID_ARG;
        }
        j.head.kind = AURA_VP_JOB_REMOVE;
        snprintf(j.spk_id, sizeof(j.spk_id), "%s", cfg->spk_id);
        vp_push_job(self, &j);
        return AURA_OK;

    case AURA_VP_OP_CLEAR:
        j.head.kind = AURA_VP_JOB_CLEAR;
        vp_push_job(self, &j);
        return AURA_OK;

    default:
        return AURA_ERR_INVALID_ARG;
    }
}

/* ============================== worker ============================== */

static void vp_post_result(aura_nn_node_t *self, bool passed, int32_t score,
                           const char *spk_id)
{
    vp_result_t r;
    memset(&r, 0, sizeof(r));
    r.type   = 0;
    r.passed = passed;
    r.score  = score;
    snprintf(r.spk_id, sizeof(r.spk_id), "%s", (spk_id != NULL) ? spk_id : "");
    aura_nn_post_result(self, &r, sizeof(r));
}

/* 切环：取环内全部有效样本，再挑能量窗口；返回挑出的采样数（0 = 太短不判）。
 * 调用方持锁。 */
static uint32_t vp_slice_window(vp_state_t *st, int16_t *out)
{
    uint32_t cap = AURA_VP_RING_S * 16000;
    uint32_t n   = st->ring_len;
    if (n < (uint32_t)(st->min_utt_s * 16000.0f)) {
        return 0; /* 太短不判 */
    }
    /* 环内容线性化到 out（最多 6s） */
    uint32_t start = (st->ring_head + cap - n) % cap;
    for (uint32_t i = 0; i < n; i++) {
        out[i] = st->ring[(start + i) % cap];
    }
    /* 只保留有效语音：能量最高的 max_s 窗口（与离线注册同一函数，见 vp_lib.c） */
    uint32_t off = aura_vp_energy_window(out, n, 16000, st->max_utt_s);
    if (off > 0) {
        memmove(out, out + off, sizeof(int16_t) * (n - off));
        n -= off;
    }
    return n;
}

/* PCM → embedding（L2 归一化后）。frames = 采样数；成功返回帧数（≥1），失败 -1。 */
static int32_t vp_embed(aura_nn_node_t *self, vp_state_t *st, const int16_t *pcm,
                        uint32_t frames, float *emb_out)
{
    aura_feature_reset(st->feat);
    float *fpcm = (float *)malloc(sizeof(float) * frames);
    if (fpcm == NULL) {
        return -1;
    }
    for (uint32_t i = 0; i < frames; i++) {
        fpcm[i] = (float)pcm[i] * (1.0f / 32768.0f);
    }
    (void)aura_feature_accept(st->feat, fpcm, (int32_t)frames);
    free(fpcm);

    int32_t n = 0;
    while (aura_feature_ready(st->feat) > 0 && n < AURA_VP_MAX_FRAMES) {
        if (aura_feature_pop(st->feat, st->feat_buf + (size_t)n * 80) <= 0) {
            break;
        }
        n++;
    }
    if (n < 2) {
        return -1; /* 特征太少 */
    }

    aura_infer_model_t *model = aura_nn_model(self);
    aura_tensor_t *x = aura_model_input(model, NULL);
    int32_t dims[3] = {1, n, 80};
    if (x == NULL || aura_tensor_resize(x, dims, 3) != AURA_OK ||
        aura_tensor_write(x, st->feat_buf, (uint64_t)n * 80 * 4) != AURA_OK ||
        aura_model_run(model) != AURA_OK) {
        AURA_LOGE(TAG, "embedding 前向失败");
        return -1;
    }
    aura_tensor_t *e = aura_model_output(model, NULL);
    if (e == NULL || aura_tensor_read(e, st->emb, AURA_VP_EMB_DIM * 4) != AURA_OK) {
        AURA_LOGE(TAG, "embedding 回读失败");
        return -1;
    }

    /* L2 归一化 */
    float norm = 0.0f;
    for (int32_t i = 0; i < AURA_VP_EMB_DIM; i++) {
        norm += st->emb[i] * st->emb[i];
    }
    norm = sqrtf(norm);
    if (norm < 1e-8f) {
        return -1;
    }
    for (int32_t i = 0; i < AURA_VP_EMB_DIM; i++) {
        emb_out[i] = st->emb[i] / norm;
    }
    return n;
}

/* 与库比对：返回最佳余弦与命中说话人下标（-1 = 未命中）。 */
static float vp_compare(vp_state_t *st, const float *emb, int32_t *best_idx)
{
    float   best = -1.0f;
    int32_t idx  = -1;
    for (int32_t i = 0; i < st->lib_count; i++) {
        float dot = 0.0f;
        for (int32_t d = 0; d < AURA_VP_EMB_DIM; d++) {
            dot += emb[d] * st->lib[i].emb[d];
        }
        if (dot > best) {
            best = dot;
            idx  = i;
        }
    }
    *best_idx = idx;
    return best;
}

static aura_err_t vp_async_run(aura_nn_node_t *self, const void *job)
{
    vp_state_t *st = vp_state(self);
    const vp_job_t *j = (const vp_job_t *)job;

    switch (j->head.kind) {
    case AURA_VP_JOB_VERIFY: {
        aura_osal_mutex_lock(st->ring_lock);
        uint32_t n = vp_slice_window(st, st->slice_buf);
        aura_osal_mutex_unlock(st->ring_lock);
        if (n == 0) {
            AURA_LOGD(TAG, "%s: 切片太短，跳过验证", self->base.name);
            return AURA_OK;
        }
        float emb[AURA_VP_EMB_DIM];
        if (vp_embed(self, st, st->slice_buf, n, emb) < 0) {
            return AURA_ERR_MODEL;
        }
        if (st->lib_count == 0) {
            vp_post_result(self, false, 0, "(未注册)");
            return AURA_OK;
        }
        int32_t idx;
        float   best = vp_compare(st, emb, &idx);
        bool    pass = (best >= st->threshold);
        vp_post_result(self, pass, (int32_t)(best * 100.0f),
                       (idx >= 0) ? st->lib[idx].id : "");
        AURA_LOGI(TAG, "%s: 验证 spk='%s' cos=%.3f %s", self->base.name,
                  (idx >= 0) ? st->lib[idx].id : "?", (double)best,
                  pass ? "通过" : "拒绝");
        return AURA_OK;
    }

    case AURA_VP_JOB_REMOVE:
        for (int32_t i = 0; i < st->lib_count; i++) {
            if (strcmp(st->lib[i].id, j->spk_id) == 0) {
                st->lib[i] = st->lib[st->lib_count - 1];
                st->lib_count--;
                AURA_LOGI(TAG, "%s: 删除 '%s'（剩 %d 条）", self->base.name,
                          j->spk_id, st->lib_count);
                return AURA_OK;
            }
        }
        return AURA_ERR_NOT_FOUND;

    case AURA_VP_JOB_CLEAR:
        st->lib_count = 0;
        AURA_LOGI(TAG, "%s: 说话人库已清空", self->base.name);
        return AURA_OK;

    default:
        return AURA_OK;
    }
}

/* ============================== result_consume ============================== */

static aura_err_t vp_result_consume(aura_nn_node_t *self, const void *result,
                                    uint32_t len)
{
    if (len < sizeof(vp_result_t)) {
        return AURA_ERR_INVALID_ARG;
    }
    const vp_result_t *r = (const vp_result_t *)result;
    aura_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type                 = AURA_EVENT_VOICEPRINT_RESULT;
    ev.u.voiceprint.passed  = r->passed;
    ev.u.voiceprint.score   = r->score;
    snprintf(ev.payload, sizeof(ev.payload), "%s", r->spk_id);
    aura_nn_emit(self, AURA_EVENT_VOICEPRINT_RESULT, &ev);
    return AURA_OK;
}

/* ============================== 复位 / 收尾 ============================== */

static aura_err_t vp_algo_reset(aura_nn_node_t *self)
{
    /* worker 线程里跑：清环（与 process 的写入错开 —— 但环锁在此处拿） */
    vp_state_t *st = vp_state(self);
    aura_osal_mutex_lock(st->ring_lock);
    st->ring_len  = 0;
    st->ring_head = 0;
    aura_osal_mutex_unlock(st->ring_lock);
    return AURA_OK;
}

static void vp_deinit(aura_nn_node_t *self)
{
    vp_state_t *st = vp_state(self);
    if (st->feat != NULL) {
        aura_feature_destroy(st->feat);
        st->feat = NULL;
    }
    if (st->ring_lock != NULL) {
        aura_osal_mutex_destroy(st->ring_lock);
        st->ring_lock = NULL;
    }
}

/* ============================== 描述表 ============================== */

static const aura_algo_param_spec_t vp_param_specs[] = {
    {"threshold", AURA_ALGO_PARAM_F32, 0.0f, 1.0f, "余弦判定阈值（默认 0.6）"},
    {"max_utterance_seconds", AURA_ALGO_PARAM_F32, 0.5f, 6.0f, "切片封顶（默认 3s，超长取尾）"},
    {"min_utterance_seconds", AURA_ALGO_PARAM_F32, 0.2f, 6.0f, "切片下限（默认 0.8s，太短不判）"},
    {"library_file", AURA_ALGO_PARAM_STR, 0, 0,
     "说话人库文件（默认 model_root/voiceprint/library.bin；离线工具生成）"},
};

static aura_nn_desc_t vp_desc = {
    .name            = "voiceprint",
    .kind            = AURA_ALGO_KIND_NN_ASYNC,
    .io_kind         = AURA_NN_IO_OBSERVER,
    .form            = AURA_INFER_FORM_MODULE,
    .model_file      = "speaker_recognition/3dspeaker_speech_eres2netv2_sv_zh-cn_16k-common.mnn",
    .wants_events    = true, /* VAD_SPEECH_END 触发验证 */
    .private_group   = true, /* worker 独占推理组 */
    .state_size      = sizeof(vp_state_t),
    .job_size        = sizeof(vp_job_t),
    .job_queue_depth = 4,
    .algo_init       = vp_init,
    .frame_in        = vp_frame_in,
    .async_run       = vp_async_run,
    .result_consume  = vp_result_consume,
    .algo_reset      = vp_algo_reset,
    .on_event        = vp_on_event,
    .algo_control   = vp_control,
    .algo_deinit     = vp_deinit,
    .param_specs     = vp_param_specs,
    .param_spec_count = sizeof(vp_param_specs) / sizeof(vp_param_specs[0]),
};

aura_err_t aura_voiceprint_register(void)
{
    if (aura_algo_find("voiceprint") != NULL) {
        return AURA_OK;
    }
    return aura_nn_register_all(&vp_desc, 1);
}
