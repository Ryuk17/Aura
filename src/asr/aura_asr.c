/* Aura — ASR 流式识别节点（zipformer v1 transducer 贪婪解码，NN_ASYNC）
 *
 * 线程模型（NN_ASYNC 契约，见 aura_nn_adapter.h）：
 *   - frame_in（process 任务）：PCM → fbank → 逐帧喂进**引擎的特征环**（引擎
 *     自己攒 T+1 帧，ready 语义与 sherpa 的 IsReady 一致）；够一 chunk 就返回
 *     true，模板把 state 头部的**纯命令 job**（不带数据）拷进任务队列；
 *   - async_run（worker）：decode_chunk + 逐输出帧贪婪解码，把本 chunk 的
 *     **文本增量**经 aura_nn_post_result 投回 —— 绝不 emit_*；
 *   - result_consume（bus 分发任务）：累积文本（唯一归属）、发 ASR_PARTIAL /
 *     ASR_FINAL 事件、emit_text。累积文本只在这里被碰，无跨线程共享。
 *
 * 解码语义（sherpa-onnx 在线识别的简化子集）：
 *   - 每输出帧：logits argmax；0（blank）计尾静音；其余按 tokens.txt 拼符号；
 *   - BPE 词界：符号里的 ▁ (U+2581) 转空格（bilingual 模型的 tokens.txt 是 BPE）；
 *   - endpoint 三规则（可配）：初段尾静音 2.4s → FINAL；后续段 1.2s；硬上限
 *     20s 强制 FINAL；VAD_SPEECH_END 事件兜底（on_event 投 finalize 命令）；
 *   - RESET/FLUSH：哨兵走队列在 worker 里跑，并投"清文本"结果让
 *     result_consume 清累积 —— 与 process 线程零并发。
 */
#include <stdio.h>
#include <string.h>

#include "algorithm/aura_nn_adapter.h"
#include "core/state_machine/state_machine.h"
#include "dsp/aura_feature.h"
#include "dsp/zipformer/aura_zipformer.h"
#include "dsp/zipformer/symbol_table.h"
#include "utils/logger/logger.h"
#include "utils/profiler/profiler.h"

#define TAG "asr"

enum {
    AURA_ASR_VOCAB_MAX = 8192,
    AURA_ASR_YS_MAX    = 1024, /* token 历史（贪婪解码只回看 context_size） */
    AURA_ASR_TEXT_MAX  = 4096, /* 累积文本上限（result_consume 侧） */
    AURA_ASR_DELTA_MAX = 400,  /* 单 chunk 文本增量上限（结果载荷） */
    AURA_ASR_PCM_BUF   = 4096,
    AURA_ASR_PATH_MAX  = 512,
};

/* 任务类别（job.kind）。job 是**纯命令**（无数据）：特征已在引擎环里。 */
enum {
    AURA_ASR_JOB_DECODE = 0, /* 解码一个 chunk */
    AURA_ASR_JOB_FINALIZE,   /* 收尾：VAD_END 兜底 */
};

/* 结果类别（result 头 4 字节） */
enum {
    AURA_ASR_RES_PARTIAL = 0, /* 文本增量（partial） */
    AURA_ASR_RES_FINAL,       /* 收尾：文本增量 + final */
    AURA_ASR_RES_CLEAR,       /* 清空累积文本（复位） */
};

/* ============================== job / 结果 ============================== */

typedef struct {
    aura_nn_job_head_t head;
} asr_job_t;

typedef struct {
    int32_t type;
    char    text[AURA_ASR_DELTA_MAX + 1]; /* 文本增量（'\0' 结尾） */
    int32_t trailing_blanks;              /* 尾静音（输出帧数） */
} asr_result_t;

/* ============================== 节点状态 ============================== */

typedef struct {
    /* job 必须第一成员：模板按 state 头 job_size 字节拷贝 */
    asr_job_t job;

    /* worker 专属（只有 async_run/algo_reset 碰） */
    aura_zipformer_t    *zf;
    aura_feature_t      *feat;
    aura_symbol_table_t *sym;
    int32_t              vocab;
    int32_t              context_size;
    int32_t              ys[AURA_ASR_YS_MAX];
    int32_t              ys_len;
    int32_t              trailing_blanks;
    bool                 any_final; /* 发过 FINAL：后续段用短静音阈值 */
    float                logits[AURA_ASR_VOCAB_MAX];

    /* process 任务专属（frame_in 碰） */
    float   pcm_buf[AURA_ASR_PCM_BUF];
    float   feat_buf[128];
    int32_t fed_total;     /* 进程侧喂入引擎的 fbank 帧总数（环丢帧不影响它） */
    int32_t last_job_fed;  /* 上次投 job 时的 fed_total（按 chunk 节拍投 job） */

    /* result_consume 专属（bus 任务） */
    char    text[AURA_ASR_TEXT_MAX];
    int32_t text_len;
    bool    first_partial; /* AURA_PROF_ASR_FIRST_PARTIAL 打点只打一次 */

    /* 状态门控（on_event 置位，frame_in 读）：
     * 链是串行语义 —— VAD 圈语音 → KWS 唤醒 → **进入 Listening 后 ASR 才开始
     * 消费音频**。唤醒前 worker 完全空闲（不跟 KWS 抢 CPU），这是全链不阻塞
     * 的关键。bool 跨线程读写是良性的（最坏多处理/少处理一帧）。
     * require_wake=false（链上没有 KWS/状态机的独立用法）时门控关闭。 */
    bool require_wake;
    bool listening;

    /* 配置快照（装配期后只读） */
    int32_t end1_blanks; /* 初段尾静音阈值（输出帧） */
    int32_t end2_blanks; /* 后续段尾静音阈值 */
    int32_t end_hard;    /* 硬上限（输出帧） */
} asr_state_t;

/* ============================== 装配 ============================== */

static asr_state_t *asr_state(aura_nn_node_t *self)
{
    return (asr_state_t *)aura_nn_state(self);
}

static const char *asr_param_str(const aura_algo_params_t *p, const char *name,
                                 const char *def)
{
    const aura_algo_param_t *pa = aura_algo_params_get(p, name);
    const char *v = (pa != NULL && pa->type == AURA_ALGO_PARAM_STR) ? pa->v.s : def;
    return (v != NULL) ? v : "";
}

static void asr_resolve_path(const char *root, const char *file, char *out, size_t cap)
{
    if (file == NULL || file[0] == '\0') {
        out[0] = '\0';
        return;
    }
    if (file[0] == '/' || file[0] == '\\' || file[1] == ':') {
        snprintf(out, cap, "%s", file);
        return;
    }
    if (root != NULL && root[0] != '\0') {
        snprintf(out, cap, "%s/%s", root, file);
    } else {
        snprintf(out, cap, "%s", file);
    }
}

static aura_err_t asr_init(aura_nn_node_t *self, aura_infer_model_t *model)
{
    (void)model; /* 引擎自装三模型（v1 布局按 model_type 自动分派） */
    asr_state_t *st = asr_state(self);
    memset(st, 0, sizeof(*st));

    const char *root = aura_nn_model_root();
    char enc[AURA_ASR_PATH_MAX], dec[AURA_ASR_PATH_MAX], joi[AURA_ASR_PATH_MAX];
    char tokens[AURA_ASR_PATH_MAX];
    asr_resolve_path(root,
                     asr_param_str(&self->params, "encoder_file",
                                   "asr/sherpa-mnn-streaming-zipformer-bilingual-zh-en-"
                                   "2023-02-20/encoder-epoch-99-avg-1.int8.mnn"),
                     enc, sizeof(enc));
    asr_resolve_path(root,
                     asr_param_str(&self->params, "decoder_file",
                                   "asr/sherpa-mnn-streaming-zipformer-bilingual-zh-en-"
                                   "2023-02-20/decoder-epoch-99-avg-1.int8.mnn"),
                     dec, sizeof(dec));
    asr_resolve_path(root,
                     asr_param_str(&self->params, "joiner_file",
                                   "asr/sherpa-mnn-streaming-zipformer-bilingual-zh-en-"
                                   "2023-02-20/joiner-epoch-99-avg-1.int8.mnn"),
                     joi, sizeof(joi));
    asr_resolve_path(root,
                     asr_param_str(&self->params, "tokens_file",
                                   "asr/sherpa-mnn-streaming-zipformer-bilingual-zh-en-"
                                   "2023-02-20/tokens.txt"),
                     tokens, sizeof(tokens));

    /* 门控开关：产品语义 = 唤醒后识别；链上没有 KWS/状态机的独立用法配 false */
    st->require_wake = true;
    {
        const aura_algo_param_t *pa = aura_algo_params_get(&self->params, "require_wake");
        if (pa != NULL && pa->type == AURA_ALGO_PARAM_BOOL) {
            st->require_wake = pa->v.b;
        }
    }

    /* endpoint 阈值（秒 → 输出帧：每输出帧 = 40ms） */
    st->end1_blanks = (int32_t)(aura_algo_params_f32(&self->params, "endpoint_rule1", 2.4f) /
                                0.04f);
    st->end2_blanks = (int32_t)(aura_algo_params_f32(&self->params, "endpoint_rule2", 1.2f) /
                                0.04f);
    st->end_hard    = (int32_t)(aura_algo_params_f32(&self->params, "endpoint_rule3", 20.0f) /
                                0.04f);

    aura_zipformer_cfg_t zc;
    memset(&zc, 0, sizeof(zc));
    zc.encoder_path = enc;
    zc.decoder_path = dec;
    zc.joiner_path  = joi;
    zc.feature_dim  = 80;
    zc.group        = self->group; /* private_group → worker 独占推理组 */
    st->zf = aura_zipformer_create(&zc);
    if (st->zf == NULL) {
        AURA_LOGE(TAG, "zipformer 引擎装配失败（enc=%s）", enc);
        return AURA_ERR_MODEL;
    }
    st->vocab        = aura_zipformer_vocab_size(st->zf);
    st->context_size = aura_zipformer_context_size(st->zf);
    if (st->vocab > AURA_ASR_VOCAB_MAX) {
        AURA_LOGE(TAG, "vocab=%d 超上限 %d", st->vocab, AURA_ASR_VOCAB_MAX);
        return AURA_ERR_MODEL;
    }

    st->sym = aura_symbol_table_create(tokens, st->vocab);
    if (st->sym == NULL) {
        AURA_LOGE(TAG, "符号表解析失败: %s", tokens);
        return AURA_ERR_IO;
    }

    aura_feature_cfg_t fc;
    aura_feature_cfg_default(&fc);
    fc.sample_rate = (int32_t)self->rate;
    st->feat = aura_feature_create(&fc);
    if (st->feat == NULL) {
        return AURA_ERR_NOMEM;
    }

    st->ys[0] = 0; /* 初始历史 = [blank]（BuildDecoderInput 不足补 -1） */
    st->ys_len = 1;

    AURA_LOGI(TAG, "%s 就绪: T=%d vocab=%d endpoint=%d/%d/%d 帧", self->base.name,
              aura_zipformer_T(st->zf), st->vocab, st->end1_blanks, st->end2_blanks,
              st->end_hard);
    return AURA_OK;
}

/* ============================== frame_in ============================== */

static bool asr_frame_in(aura_nn_node_t *self, const aura_audio_frame_t *frame)
{
    asr_state_t *st = asr_state(self);
    /* 状态门控：唤醒（KWS_HIT → Listening）之前不消费音频 —— 串行语义，
     * 唤醒前的算力全部留给 VAD + KWS。 */
    if (st->require_wake && !st->listening) {
        return false;
    }
    if (frame->frame_count > AURA_ASR_PCM_BUF) {
        return false;
    }
    const int16_t *pcm = (const int16_t *)frame->data;
    for (uint32_t i = 0; i < frame->frame_count; i++) {
        st->pcm_buf[i] = (float)pcm[i] * (1.0f / 32768.0f);
    }
    if (aura_feature_accept(st->feat, st->pcm_buf, (int32_t)frame->frame_count) !=
        AURA_OK) {
        return false;
    }
    while (aura_feature_ready(st->feat) > 0) {
        if (aura_feature_pop(st->feat, st->feat_buf) <= 0) {
            break;
        }
        aura_zipformer_accept(st->zf, st->feat_buf, 1);
        st->fed_total++;
    }

    /* 投 job 的节拍 = 每喂入 shift 帧投一个（**按喂入帧数计**，不是引擎计数：
     * 环满丢帧时引擎计数恒等于 cap，差值法会永远到不了 shift —— 实测就是
     * job 停发、环继续丢帧）。首 job 要求环里至少 T 帧（decode 的 ready 是
     * 严格大于 T，配合"每次多喂 shift 帧"保证到达时必有 T+1 帧可用）。 */
    if (st->fed_total - st->last_job_fed < aura_zipformer_chunk_shift(st->zf)) {
        return false;
    }
    /* 引擎的 decode 要求严格大于 T（count > T）—— 这里也要 > T，
     * == T 时投 job 会吃 -10（实测） */
    if (aura_zipformer_num_frames(st->zf) <= aura_zipformer_T(st->zf)) {
        return false;
    }
    st->last_job_fed = st->fed_total;
    st->job.head.magic = AURA_NN_JOB_MAGIC;
    st->job.head.kind  = AURA_ASR_JOB_DECODE;
    return true;
}

/* ============================== worker ============================== */

static void asr_post(aura_nn_node_t *self, int32_t type, const char *text,
                     int32_t trailing)
{
    asr_result_t r;
    memset(&r, 0, sizeof(r));
    r.type = type;
    snprintf(r.text, sizeof(r.text), "%s", (text != NULL) ? text : "");
    r.trailing_blanks = trailing;
    aura_nn_post_result(self, &r, sizeof(r));
}

/* 符号 → 展示文本（BPE 词界 ▁ → 空格）。 */
static void asr_append_symbol(asr_state_t *st, char *buf, size_t cap, size_t *len,
                              int32_t token)
{
    const char *sym = aura_symbol_table_symbol(st->sym, token);
    if (sym == NULL) {
        return;
    }
    for (const char *p = sym; *p != '\0'; p++) {
        char c = *p;
        if ((unsigned char)p[0] == 0xE2 && (unsigned char)p[1] == 0x96 &&
            (unsigned char)p[2] == 0x81) {
            c = ' '; /* ▁ = UTF-8 E2 96 81 */
            p += 2;
        }
        if (*len + 1 < cap) {
            buf[(*len)++] = c;
            buf[*len]     = '\0';
        }
    }
}

/* 解码一个 chunk，产出文本增量。 */
static void asr_decode_chunk(aura_nn_node_t *self, asr_state_t *st, char *delta,
                             size_t delta_cap, size_t *delta_len)
{
    *delta_len = 0;
    delta[0]   = '\0';

    int32_t n = aura_zipformer_decode_chunk(st->zf);
    if (n < 0) {
        AURA_LOGE(TAG, "decode_chunk 失败: %d", n);
        return;
    }

    for (int32_t t = 0; t < n; t++) {
        int32_t ys_ctx[8];
        int32_t cs = st->context_size;
        for (int32_t i = 0; i < cs; i++) {
            int32_t pos = st->ys_len - cs + i;
            ys_ctx[i]   = (pos >= 0) ? st->ys[pos] : -1;
        }
        if (aura_zipformer_decode_frame(st->zf, t, ys_ctx, st->logits) != AURA_OK) {
            AURA_LOGE(TAG, "decode_frame 失败（t=%d）", t);
            return;
        }
        int32_t best   = 0;
        float   best_v = st->logits[0];
        for (int32_t k = 1; k < st->vocab; k++) {
            if (st->logits[k] > best_v) {
                best_v = st->logits[k];
                best   = k;
            }
        }

        if (best == 0) {
            st->trailing_blanks++;
            continue;
        }
        st->trailing_blanks = 0;
        if (st->ys_len < AURA_ASR_YS_MAX) {
            st->ys[st->ys_len++] = best;
        }
        asr_append_symbol(st, delta, delta_cap, delta_len, best);
    }

    /* partial：每 chunk 投一次文本增量 */
    asr_post(self, AURA_ASR_RES_PARTIAL, delta, st->trailing_blanks);
}

static aura_err_t asr_async_run(aura_nn_node_t *self, const void *job)
{
    asr_state_t *st = asr_state(self);
    const asr_job_t *j = (const asr_job_t *)job;
    char    delta[AURA_ASR_DELTA_MAX + 1];
    size_t  delta_len = 0;

    switch (j->head.kind) {
    case AURA_ASR_JOB_DECODE: {
        uint64_t dt0 = aura_osal_time_us();
        asr_decode_chunk(self, st, delta, sizeof(delta), &delta_len);
        AURA_LOGD(TAG, "chunk 解码 %llu ms（尾静音 %d）",
                  (unsigned long long)((aura_osal_time_us() - dt0) / 1000),
                  st->trailing_blanks);

        /* endpoint：尾静音超阈值 → FINAL（含最后增量）。
         * 初段用规则 1，发过 FINAL 之后的段用规则 2；硬上限恒生效。 */
        if (st->trailing_blanks >= st->end_hard ||
            st->trailing_blanks >= (st->any_final ? st->end2_blanks : st->end1_blanks)) {
            asr_post(self, AURA_ASR_RES_FINAL, delta, st->trailing_blanks);
            st->any_final       = true;
            st->trailing_blanks = 0; /* 段边界：尾静音重新计 */
        }
        break;
    }

    case AURA_ASR_JOB_FINALIZE:
        /* VAD_END 兜底：残余增量收尾 */
        asr_post(self, AURA_ASR_RES_FINAL, "", st->trailing_blanks);
        st->any_final       = true;
        st->trailing_blanks = 0;
        break;

    default:
        break;
    }
    return AURA_OK;
}

/* 事件 payload（256B）/文本 chunk（1KB）都装不下 4KB 的累积文本 ——
 * 显式截断，别让编译器的 format-truncation 告警靠运气过。 */
static void asr_payload(char *dst, size_t cap, const char *text)
{
    size_t len = strlen(text);
    if (len >= cap) {
        len = cap - 1;
    }
    memcpy(dst, text, len);
    dst[len] = '\0';
}

/* ============================== result_consume ============================== */

static aura_err_t asr_result_consume(aura_nn_node_t *self, const void *result,
                                     uint32_t len)
{
    asr_state_t *st = asr_state(self);
    if (len < sizeof(asr_result_t)) {
        return AURA_ERR_INVALID_ARG;
    }
    const asr_result_t *r = (const asr_result_t *)result;

    switch (r->type) {
    case AURA_ASR_RES_CLEAR:
        st->text_len      = 0;
        st->text[0]       = '\0';
        st->first_partial = false;
        return AURA_OK;

    case AURA_ASR_RES_PARTIAL: {
        size_t add = strlen(r->text);
        if (add > 0 && (size_t)st->text_len + add < AURA_ASR_TEXT_MAX) {
            memcpy(st->text + st->text_len, r->text, add + 1);
            st->text_len += (int32_t)add;
        }
        aura_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.type           = AURA_EVENT_ASR_PARTIAL;
        ev.u.asr.is_final = false;
        asr_payload(ev.payload, sizeof(ev.payload), st->text);
        aura_nn_emit(self, AURA_EVENT_ASR_PARTIAL, &ev);
        if (!st->first_partial) {
            st->first_partial = true;
            aura_profiler_mark(AURA_PROF_ASR_FIRST_PARTIAL);
        }
        return AURA_OK;
    }

    case AURA_ASR_RES_FINAL: {
        size_t add = strlen(r->text);
        if (add > 0 && (size_t)st->text_len + add < AURA_ASR_TEXT_MAX) {
            memcpy(st->text + st->text_len, r->text, add + 1);
            st->text_len += (int32_t)add;
        }
        aura_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.type           = AURA_EVENT_ASR_FINAL;
        ev.u.asr.is_final = true;
        ev.u.asr.len      = (uint32_t)st->text_len;
        asr_payload(ev.payload, sizeof(ev.payload), st->text);
        aura_nn_emit(self, AURA_EVENT_ASR_FINAL, &ev);

        /* 文本面：FINAL 才推（partial 只发事件，下游 LLM 等整段） */
        aura_text_chunk_t tc;
        memset(&tc, 0, sizeof(tc));
        asr_payload(tc.text, sizeof(tc.text), st->text);
        tc.len = (uint32_t)st->text_len;
        (void)aura_node_emit_text(&self->base, &tc);

        /* 一段结束：清累积，下一段重新开始 */
        st->text_len      = 0;
        st->text[0]       = '\0';
        st->first_partial = false;
        return AURA_OK;
    }

    default:
        return AURA_OK;
    }
}

/* ============================== 复位 / 事件 ============================== */

static aura_err_t asr_algo_reset(aura_nn_node_t *self)
{
    /* worker 线程里跑：清引擎/特征/历史，再投"清文本"给 result_consume */
    asr_state_t *st = asr_state(self);
    aura_zipformer_reset(st->zf);
    aura_feature_reset(st->feat);
    st->ys_len          = 1;
    st->ys[0]           = 0;
    st->trailing_blanks = 0;
    st->any_final       = false;
    asr_post(self, AURA_ASR_RES_CLEAR, "", 0);
    return AURA_OK;
}

static void asr_on_event(aura_nn_node_t *self, const aura_event_t *event)
{
    asr_state_t *st = asr_state(self);

    if (event->type == AURA_EVENT_STATE_CHANGED) {
        bool now_listening = (event->u.state.to == AURA_STATE_LISTENING);
        if (now_listening && !st->listening) {
            /* KWS 命中，进入 Listening：复位引擎（清历史）再开始收音频。
             * 复位哨兵（全零 job）走队列在 worker 里跑 —— 与 process 线程
             * 零并发。 */
            st->listening = true;
            asr_job_t z;
            memset(&z, 0, sizeof(z)); /* magic == 0 = 复位哨兵 */
            if (aura_osal_queue_push(self->job_q, &z, AURA_NO_WAIT) != AURA_OK) {
                AURA_LOGW(TAG, "%s: 复位命令入队失败", self->base.name);
            }
        } else if (!now_listening) {
            st->listening = false;
        }
        return;
    }

    if (event->type != AURA_EVENT_VAD_SPEECH_END) {
        return;
    }
    /* VAD 说这句说完了：投 finalize 命令。bus 分发任务上下文 —— 只推队列，
     * job 头是完整命令，worker 拿到后做残余增量收尾。 */
    asr_job_t j;
    memset(&j, 0, sizeof(j));
    j.head.magic = AURA_NN_JOB_MAGIC;
    j.head.kind  = AURA_ASR_JOB_FINALIZE;
    if (aura_osal_queue_push(self->job_q, &j, AURA_NO_WAIT) != AURA_OK) {
        AURA_LOGW(TAG, "%s: finalize 命令入队失败（队列满）", self->base.name);
    }
}

static void asr_deinit(aura_nn_node_t *self)
{
    asr_state_t *st = asr_state(self);
    if (st->zf != NULL) {
        aura_zipformer_destroy(st->zf);
        st->zf = NULL;
    }
    if (st->feat != NULL) {
        aura_feature_destroy(st->feat);
        st->feat = NULL;
    }
    if (st->sym != NULL) {
        aura_symbol_table_destroy(st->sym);
        st->sym = NULL;
    }
}

/* ============================== 描述表 ============================== */

static const aura_algo_param_spec_t asr_param_specs[] = {
    {"require_wake", AURA_ALGO_PARAM_BOOL, 0, 0,
     "KWS 唤醒后才识别（默认 true；链上没有 KWS 的独立用法配 false）"},
    {"encoder_file", AURA_ALGO_PARAM_STR, 0, 0, "encoder 模型路径（相对 model_root）"},
    {"decoder_file", AURA_ALGO_PARAM_STR, 0, 0, "decoder 模型路径"},
    {"joiner_file", AURA_ALGO_PARAM_STR, 0, 0, "joiner 模型路径"},
    {"tokens_file", AURA_ALGO_PARAM_STR, 0, 0, "tokens.txt 路径"},
    {"endpoint_rule1", AURA_ALGO_PARAM_F32, 0.1f, 60, "初段尾静音时长（默认 2.4s）"},
    {"endpoint_rule2", AURA_ALGO_PARAM_F32, 0.1f, 60, "后续段尾静音时长（默认 1.2s）"},
    {"endpoint_rule3", AURA_ALGO_PARAM_F32, 0.1f, 600, "硬上限（默认 20s）"},
};

static aura_nn_desc_t asr_desc = {
    .name            = "asr",
    .kind            = AURA_ALGO_KIND_NN_ASYNC,
    .io_kind         = AURA_NN_IO_PASSTHROUGH,
    .form            = AURA_INFER_FORM_MODULE,
    .model_file      = NULL, /* 引擎自装三模型 */
    .wants_events    = true, /* VAD_SPEECH_END 兜底收尾 */
    .private_group   = true, /* worker 独占推理组 */
    .produces_text   = true,
    .state_size      = sizeof(asr_state_t),
    .job_size        = sizeof(asr_job_t),
    .job_queue_depth = 8,
    .algo_init       = asr_init,
    .frame_in        = asr_frame_in,
    .async_run       = asr_async_run,
    .result_consume  = asr_result_consume,
    .algo_reset      = asr_algo_reset,
    .on_event        = asr_on_event,
    .algo_deinit     = asr_deinit,
    .param_specs     = asr_param_specs,
    .param_spec_count = sizeof(asr_param_specs) / sizeof(asr_param_specs[0]),
};

aura_err_t aura_asr_register(void)
{
    if (aura_algo_find("asr") != NULL) {
        return AURA_OK;
    }
    return aura_nn_register_all(&asr_desc, 1);
}
