/* Aura — KWS 唤醒词节点（zipformer2 transducer 关键词束搜索）
 *
 * 形态：NN_SYNC + PASSTHROUGH + 私有推理组（chunk 解码 20–50ms > 10ms 帧预算）。
 * 描述表不声明 model_file —— encoder/decoder/joiner 三个模型由流式引擎
 * （dsp/zipformer/aura_zipformer.h）自己加载，模板只负责装配/事件/统计。
 *
 * 解码语义照搬 sherpa-mnn 的 keyword-spotter-transducer-impl.h +
 * transducer-keyword-decoder.cc（含默认参数：max_active_paths=4、
 * num_trailing_blanks=1、keywords_score=1.0、keywords_threshold=0.25、
 * 1.5s 尾静音自动复位）：
 *
 *   - 攒够 T+1 帧特征 → 一次 chunk 前向（T 帧、前移 chunk_shift）；
 *   - 每个输出帧：各假设 logits 过 log-softmax 加先验，取全局 top-K；
 *     非 blank(0)/非 unk 的 token 推入假设历史并在 context_graph（AC 自动机）
 *     上前进一步，否则计尾静音；回到根 = 从头开始匹配（历史清零）；
 *   - 每帧后看最佳假设：处于接受态且尾静音 > num_trailing_blanks 且
 *     匹配 token 概率均值 ≥ 词条阈值 → KWS_HIT 事件（score 0–100，
 *     payload = 词条显示名）→ 束复位回单个空白假设。
 *
 * 分层：本文件在 algorithm 层（依赖 aura_algorithm 模板 + aura_zipformer
 * 引擎），不碰 MNN 头、不碰 pipeline 内部。
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "algorithm/aura_nn_adapter.h"
#include "dsp/aura_feature.h"
#include "dsp/zipformer/aura_zipformer.h"
#include "dsp/zipformer/context_graph.h"
#include "dsp/zipformer/symbol_table.h"
#include "utils/logger/logger.h"

#define TAG "kws"

/* ------------------------------------------------------------ 常量 */

enum {
    AURA_KWS_MAX_PATHS     = 16,  /* 束宽上限（配置 max_active_paths 不得超它） */
    AURA_KWS_YS_MAX        = 64,  /* 假设 token 历史上限（词条 ≤ 32 个 token） */
    AURA_KWS_VOCAB_MAX     = 256, /* 词表上限（KWS 模型 197；更大直接装配报错） */
    AURA_KWS_MAX_KEYWORDS  = 16,
    AURA_KWS_KW_TOKENS_MAX = 32,
    AURA_KWS_KW_NAME_MAX   = 64,
    AURA_KWS_PCM_BUF       = 4096, /* 单帧样本上限（16k/128ms） */
    AURA_KWS_PATH_MAX      = 512,
};

/* ------------------------------------------------------------ 类型 */

typedef struct {
    int32_t ys[AURA_KWS_YS_MAX];        /* token 历史（紧凑存放，满了丢最旧） */
    float   ys_probs[AURA_KWS_YS_MAX];  /* 与 ys 对齐：该 token 被选中时的概率 */
    int32_t ys_ts[AURA_KWS_YS_MAX];     /* 与 ys 对齐：输出帧号 */
    int32_t ys_len;
    int32_t context_state;              /* AC 自动机节点下标 */
    int32_t num_trailing_blanks;
    float   log_prob;
} kws_hyp_t;

typedef struct {
    /* 引擎与数据 */
    aura_zipformer_t     *zf;
    aura_feature_t       *feat;
    aura_symbol_table_t  *sym;
    aura_context_graph_t *graph;

    int32_t unk_id;

    /* 配置快照 */
    int32_t max_active_paths;
    int32_t num_trailing_blanks;
    int32_t reset_blanks; /* auto_reset_seconds 换算成输出帧数 */
    float   keywords_score;
    float   keywords_threshold;

    /* 束搜索 */
    kws_hyp_t hyps[AURA_KWS_MAX_PATHS];
    kws_hyp_t next_buf[AURA_KWS_MAX_PATHS]; /* 展开临时区：节点任务线程栈只有 8KB，
                                             * 12.5KB 的假设数组绝不能放栈上 */
    int32_t   num_hyps;
    int64_t   frame_offset; /* 累计输出帧号（时间戳换算用） */
    int32_t   last_score;   /* 最近一次命中的分数（诊断） */

    /* 缓冲 */
    float pcm_buf[AURA_KWS_PCM_BUF];
    float feat_buf[128];
    float logits[AURA_KWS_VOCAB_MAX];
    float logprobs[AURA_KWS_MAX_PATHS][AURA_KWS_VOCAB_MAX]; /* 每假设一份 */
} kws_state_t;

/* ------------------------------------------------------------ 小工具 */

static kws_state_t *kws_state(aura_nn_node_t *self)
{
    return (kws_state_t *)aura_nn_state(self);
}

static const char *kws_param_str(const aura_algo_params_t *p, const char *name,
                                 const char *def)
{
    const aura_algo_param_t *pa = aura_algo_params_get(p, name);
    const char *v = (pa != NULL && pa->type == AURA_ALGO_PARAM_STR) ? pa->v.s : def;
    return (v != NULL) ? v : "";
}

/* 模型路径解析：相对路径 → model_root/相对路径（aura_nn 的统一模型根）。 */
static void kws_resolve_path(const char *root, const char *file, char *out, size_t cap)
{
    if (file == NULL || file[0] == '\0') {
        out[0] = '\0';
        return;
    }
    if (file[0] == '/' || file[0] == '\\' || file[1] == ':') {
        snprintf(out, cap, "%s", file); /* 绝对路径原样用 */
        return;
    }
    if (root != NULL && root[0] != '\0') {
        snprintf(out, cap, "%s/%s", root, file);
    } else {
        snprintf(out, cap, "%s", file);
    }
}

/* 假设历史追加一个 token（紧凑数组，满了丢最旧 —— 只有尾部参与匹配与解码）。 */
static void kws_hyp_push(kws_hyp_t *h, int32_t token, float prob, int32_t frame_idx)
{
    if (h->ys_len >= AURA_KWS_YS_MAX) {
        memmove(h->ys, h->ys + 1, sizeof(h->ys[0]) * (size_t)(AURA_KWS_YS_MAX - 1));
        memmove(h->ys_probs, h->ys_probs + 1,
                sizeof(h->ys_probs[0]) * (size_t)(AURA_KWS_YS_MAX - 1));
        memmove(h->ys_ts, h->ys_ts + 1,
                sizeof(h->ys_ts[0]) * (size_t)(AURA_KWS_YS_MAX - 1));
        h->ys_len = AURA_KWS_YS_MAX - 1;
    }
    h->ys[h->ys_len]       = token;
    h->ys_probs[h->ys_len] = prob;
    h->ys_ts[h->ys_len]    = frame_idx;
    h->ys_len++;
}

/* 取历史末尾的 context_size 个 token（不足补 -1，sherpa 的 BuildDecoderInput 语义）。 */
static void kws_hyp_ctx(const kws_hyp_t *h, int32_t ctx_size, int32_t *out)
{
    for (int32_t i = 0; i < ctx_size; i++) {
        int32_t pos = h->ys_len - ctx_size + i;
        out[i] = (pos >= 0) ? h->ys[pos] : -1;
    }
}

/* 空白假设（sherpa 的 blank_hyp：末位 blank=0）。 */
static void kws_hyp_blank(kws_hyp_t *h, int32_t graph_root)
{
    memset(h, 0, sizeof(*h));
    h->context_state = graph_root;
    kws_hyp_push(h, 0, 0.0f, 0); /* 末位 blank */
}

/* 假设去重（按 token 历史，sherpa 的 Hypotheses::Add 同语义）：
 * 相同历史只留 log_prob 大者。 */
static bool kws_hyp_same(const kws_hyp_t *a, const kws_hyp_t *b)
{
    if (a->ys_len != b->ys_len) {
        return false;
    }
    return memcmp(a->ys, b->ys, sizeof(a->ys[0]) * (size_t)a->ys_len) == 0;
}

static void kws_hyp_merge(kws_hyp_t *hyps, int32_t *num, const kws_hyp_t *cand)
{
    for (int32_t i = 0; i < *num; i++) {
        if (kws_hyp_same(&hyps[i], cand)) {
            if (cand->log_prob > hyps[i].log_prob) {
                hyps[i] = *cand;
            }
            return;
        }
    }
    if (*num < AURA_KWS_MAX_PATHS) {
        hyps[*num] = *cand;
        (*num)++;
    }
}

/* ------------------------------------------------------------ 复位 */

static void kws_reset_stream(kws_state_t *st)
{
    aura_zipformer_reset(st->zf);
    aura_feature_reset(st->feat);
    st->num_hyps = 0;
    kws_hyp_blank(&st->hyps[st->num_hyps++], aura_context_graph_root(st->graph));
    st->frame_offset = 0;
}

/* 尾静音自动复位（sherpa 的 Reset 语义）：只换编码器状态与假设，
 * **特征缓冲保留** —— 复位后本 chunk 照常解码，不丢已攒的帧。 */
static void kws_auto_reset(kws_state_t *st)
{
    aura_zipformer_reset_states(st->zf);
    st->num_hyps = 0;
    kws_hyp_blank(&st->hyps[st->num_hyps++], aura_context_graph_root(st->graph));
    st->frame_offset = 0;
}

/* ------------------------------------------------------------ 装配 */

static aura_err_t kws_init(aura_nn_node_t *self, aura_infer_model_t *model)
{
    (void)model; /* 无模型算法：引擎自己加载三个模型 */
    kws_state_t *st = kws_state(self);
    memset(st, 0, sizeof(*st));

    /* ---- 路径解析（相对路径落在 model_root 下） */
    const char *root = aura_nn_model_root();
    char enc[AURA_KWS_PATH_MAX], dec[AURA_KWS_PATH_MAX], joi[AURA_KWS_PATH_MAX];
    char tokens[AURA_KWS_PATH_MAX], keywords[AURA_KWS_PATH_MAX];
    kws_resolve_path(root,
                     kws_param_str(&self->params, "encoder_file",
                                   "kws/kws-zipformer-wenetspeech-3.3M-MNN/"
                                   "encoder-epoch-12-avg-2-chunk-16-left-64.mnn"),
                     enc, sizeof(enc));
    kws_resolve_path(root,
                     kws_param_str(&self->params, "decoder_file",
                                   "kws/kws-zipformer-wenetspeech-3.3M-MNN/"
                                   "decoder-epoch-12-avg-2-chunk-16-left-64.mnn"),
                     dec, sizeof(dec));
    kws_resolve_path(root,
                     kws_param_str(&self->params, "joiner_file",
                                   "kws/kws-zipformer-wenetspeech-3.3M-MNN/"
                                   "joiner-epoch-12-avg-2-chunk-16-left-64.mnn"),
                     joi, sizeof(joi));
    kws_resolve_path(root,
                     kws_param_str(&self->params, "tokens_file",
                                   "kws/kws-zipformer-wenetspeech-3.3M-MNN/tokens.txt"),
                     tokens, sizeof(tokens));
    kws_resolve_path(root,
                     kws_param_str(&self->params, "keywords_file",
                                   "kws/kws-zipformer-wenetspeech-3.3M-MNN/keywords.txt"),
                     keywords, sizeof(keywords));

    /* ---- 配置快照（默认值与 sherpa KeywordSpotterConfig 一致） */
    st->max_active_paths   = aura_algo_params_i32(&self->params, "max_active_paths", 4);
    st->num_trailing_blanks = aura_algo_params_i32(&self->params, "num_trailing_blanks", 1);
    st->keywords_score     = aura_algo_params_f32(&self->params, "keywords_score", 1.0f);
    st->keywords_threshold = aura_algo_params_f32(&self->params, "keywords_threshold", 0.25f);
    float auto_reset_s     = aura_algo_params_f32(&self->params, "auto_reset_seconds", 1.5f);
    if (st->max_active_paths <= 0 || st->max_active_paths > AURA_KWS_MAX_PATHS) {
        AURA_LOGE(TAG, "max_active_paths=%d 非法（1..%d）", st->max_active_paths,
                  AURA_KWS_MAX_PATHS);
        return AURA_ERR_INVALID_ARG;
    }
    if (st->num_trailing_blanks < 0 || auto_reset_s <= 0.0f) {
        AURA_LOGE(TAG, "num_trailing_blanks/auto_reset_seconds 非法");
        return AURA_ERR_INVALID_ARG;
    }
    /* 尾静音自动复位阈值：秒 → 输出帧数（每输出帧 = 4 输入帧 = 40ms） */
    st->reset_blanks = (int32_t)(auto_reset_s * 1000.0f / 40.0f);

    /* ---- 引擎（装配期校验：metadata/形状/dtype 全在这里对死） */
    aura_zipformer_cfg_t zc;
    memset(&zc, 0, sizeof(zc));
    zc.encoder_path = enc;
    zc.decoder_path = dec;
    zc.joiner_path  = joi;
    zc.feature_dim  = 80;
    zc.group        = self->group; /* private_group=TRUE → 本节点独占的推理组 */
    st->zf = aura_zipformer_create(&zc);
    if (st->zf == NULL) {
        AURA_LOGE(TAG, "zipformer 引擎装配失败（enc=%s）", enc);
        return AURA_ERR_MODEL;
    }

    /* ---- 符号表（vocab 以引擎为准，tokens.txt 里越界的条目不收录） */
    int32_t vocab = aura_zipformer_vocab_size(st->zf);
    if (vocab > AURA_KWS_VOCAB_MAX) {
        AURA_LOGE(TAG, "vocab=%d 超上限 %d", vocab, AURA_KWS_VOCAB_MAX);
        return AURA_ERR_MODEL;
    }
    st->sym = aura_symbol_table_create(tokens, vocab);
    if (st->sym == NULL) {
        AURA_LOGE(TAG, "符号表解析失败: %s", tokens);
        return AURA_ERR_IO;
    }
    st->unk_id = aura_symbol_table_lookup(st->sym, "<unk>");
    if (st->unk_id < 0) {
        AURA_LOGW(TAG, "词表没有 <unk>，按 -1 处理（不会与任何 token 相等）");
    }

    /* ---- keywords 文件 → context graph（AC 自动机） */
    FILE *fp = fopen(keywords, "rb");
    if (fp == NULL) {
        AURA_LOGE(TAG, "打不开 keywords 文件: %s", keywords);
        return AURA_ERR_IO;
    }
    int32_t  kw_ids[AURA_KWS_MAX_KEYWORDS][AURA_KWS_KW_TOKENS_MAX];
    int32_t  kw_len[AURA_KWS_MAX_KEYWORDS];
    char     kw_name[AURA_KWS_MAX_KEYWORDS][AURA_KWS_KW_NAME_MAX];
    const int32_t *kw_ptr[AURA_KWS_MAX_KEYWORDS];
    const char     *kw_name_ptr[AURA_KWS_MAX_KEYWORDS];
    int32_t  num_kw = 0;
    char     line[512];
    while (fgets(line, sizeof(line), fp) != NULL && num_kw < AURA_KWS_MAX_KEYWORDS) {
        int32_t n = aura_symbol_table_tokenize(st->sym, line, kw_ids[num_kw],
                                               AURA_KWS_KW_TOKENS_MAX,
                                               kw_name[num_kw],
                                               AURA_KWS_KW_NAME_MAX);
        if (n <= 0) {
            if (n < 0) {
                AURA_LOGW(TAG, "keywords 行不可用，跳过: %s", line);
            }
            continue;
        }
        kw_len[num_kw]      = n;
        kw_ptr[num_kw]      = kw_ids[num_kw];
        kw_name_ptr[num_kw] = kw_name[num_kw];
        num_kw++;
    }
    fclose(fp);
    if (num_kw == 0) {
        AURA_LOGE(TAG, "keywords 文件没有可用词条: %s", keywords);
        return AURA_ERR_INVALID_ARG;
    }
    st->graph = aura_context_graph_create(kw_ptr, kw_len, num_kw, st->keywords_score,
                                          st->keywords_threshold, NULL, kw_name_ptr,
                                          NULL);
    if (st->graph == NULL) {
        AURA_LOGE(TAG, "context graph 构建失败");
        return AURA_ERR_NOMEM;
    }
    for (int32_t i = 0; i < num_kw; i++) {
        AURA_LOGI(TAG, "关键词[%d]: %s（%d 个 token）", i,
                  kw_name[i][0] != '\0' ? kw_name[i] : "(无名)", kw_len[i]);
    }

    /* ---- 特征前端（训练侧参数：snip_edges=false、high_freq=-400） */
    aura_feature_cfg_t fc;
    aura_feature_cfg_default(&fc);
    fc.sample_rate = (int32_t)self->rate;
    st->feat = aura_feature_create(&fc);
    if (st->feat == NULL) {
        AURA_LOGE(TAG, "特征前端创建失败");
        return AURA_ERR_NOMEM;
    }

    kws_reset_stream(st);
    AURA_LOGI(TAG, "%s 就绪: T=%d vocab=%d 关键词=%d 束宽=%d", self->base.name,
              aura_zipformer_T(st->zf), vocab, num_kw, st->max_active_paths);
    return AURA_OK;
}

/* ------------------------------------------------------------ 束搜索 */

/* 发 KWS_HIT 事件（score 0–100，payload = 词条显示名）。 */
static void kws_emit_hit(aura_nn_node_t *self, const kws_hyp_t *hyp,
                         int32_t matched, float ys_prob)
{
    kws_state_t *st = kws_state(self);
    aura_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type        = AURA_EVENT_KWS_HIT;
    ev.u.kws.score = (int32_t)(ys_prob * 100.0f + 0.5f);
    const char *p  = aura_context_graph_phrase(st->graph, matched);
    snprintf(ev.payload, sizeof(ev.payload), "%s", (p != NULL) ? p : "(无名)");
    st->last_score = ev.u.kws.score;
    AURA_LOGI(TAG, "%s 命中 '%s' score=%d（历史 %d token）", self->base.name,
              ev.payload, ev.u.kws.score, hyp->ys_len);
    aura_nn_emit(self, AURA_EVENT_KWS_HIT, &ev);
}

/* 一个输出帧的束搜索（sherpa transducer-keyword-decoder.cc 的 Decode 内层循环）。 */
static void kws_beam_step(aura_nn_node_t *self, kws_state_t *st, int32_t t)
{
    const int32_t vocab = aura_zipformer_vocab_size(st->zf);
    const int32_t ctx   = aura_zipformer_context_size(st->zf);
    const int32_t root  = aura_context_graph_root(st->graph);
    const int32_t K     = st->max_active_paths;
    const int32_t nh    = st->num_hyps;

    /* 1. 各假设前向：decoder+joiner → logits → log-softmax（数值稳定版） */
    for (int32_t h = 0; h < nh; h++) {
        int32_t ys_ctx[AURA_KWS_KW_NAME_MAX]; /* context_size ≤ 8，数组足够 */
        kws_hyp_ctx(&st->hyps[h], ctx, ys_ctx);
        if (aura_zipformer_decode_frame(st->zf, t, ys_ctx, st->logits) != AURA_OK) {
            AURA_LOGE(TAG, "decode_frame 失败（t=%d, hyp=%d）", t, h);
            return;
        }
        float mx = st->logits[0];
        for (int32_t k = 1; k < vocab; k++) {
            if (st->logits[k] > mx) mx = st->logits[k];
        }
        float sum = 0.0f;
        for (int32_t k = 0; k < vocab; k++) {
            st->logprobs[h][k] = expf(st->logits[k] - mx);
            sum += st->logprobs[h][k];
        }
        float lse = logf(sum);
        for (int32_t k = 0; k < vocab; k++) {
            st->logprobs[h][k] = st->logits[k] - mx - lse;
        }
    }

    /* 2. 全局 top-K：(h,k) 候选分数 = 假设先验 + 本帧 logprob。
     * K 很小，用 K 遍线性选择，不做全量排序。 */
    int32_t sel_h[AURA_KWS_MAX_PATHS];
    int32_t sel_k[AURA_KWS_MAX_PATHS];
    float   sel_s[AURA_KWS_MAX_PATHS];
    int32_t n_sel = 0;
    for (int32_t pass = 0; pass < K; pass++) {
        float   best_s = -INFINITY;
        int32_t best_h = -1, best_k = -1;
        for (int32_t h = 0; h < nh; h++) {
            for (int32_t k = 0; k < vocab; k++) {
                float s = st->hyps[h].log_prob + st->logprobs[h][k];
                /* 已被前几遍选走的不再参与（分数相同则跳过，保证不重复选） */
                bool taken = false;
                for (int32_t q = 0; q < n_sel; q++) {
                    if (sel_h[q] == h && sel_k[q] == k) {
                        taken = true;
                        break;
                    }
                }
                if (!taken && s > best_s) {
                    best_s = s;
                    best_h = h;
                    best_k = k;
                }
            }
        }
        if (best_h < 0) {
            break;
        }
        sel_h[n_sel] = best_h;
        sel_k[n_sel] = best_k;
        sel_s[n_sel] = best_s;
        n_sel++;
    }

    /* 3. 展开新假设（sherpa 同语义）。next 用节点状态里的缓冲 ——
     * 节点任务线程栈只有 8KB，放栈上会溢出（实测表现为整条任务卡死）。 */
    kws_hyp_t *next     = st->next_buf;
    int32_t    num_next = 0;
    for (int32_t q = 0; q < n_sel; q++) {
        int32_t h = sel_h[q];
        int32_t k = sel_k[q];

        kws_hyp_t cand = st->hyps[h];
        cand.log_prob = sel_s[q];

        if (k != 0 && k != st->unk_id) {
            kws_hyp_push(&cand, k, expf(st->logprobs[h][k]),
                         (int32_t)(st->frame_offset + t));
            cand.num_trailing_blanks = 0;

            int32_t next_state = cand.context_state;
            int32_t matched    = -1;
            float   ctx_score  = aura_context_graph_forward(
                st->graph, cand.context_state, k, &next_state, &matched);
            cand.context_state = next_state;
            cand.log_prob += ctx_score;
            /* 回到根 = 该 token 不属于任何词条的当前位置 —— 从头开始匹配，
             * 忘掉历史（sherpa：context_state->token == -1 时清空 ys）。 */
            if (next_state == root) {
                cand.ys_len = 0;
            }
            (void)matched;
        } else {
            cand.num_trailing_blanks++;
        }
        kws_hyp_merge(next, &num_next, &cand);
    }
    memcpy(st->hyps, next, sizeof(kws_hyp_t) * (size_t)num_next);
    st->num_hyps = num_next;

    /* 4. 最佳假设的匹配判定（sherpa 同语义） */
    const kws_hyp_t *best = &st->hyps[0];
    for (int32_t h = 1; h < st->num_hyps; h++) {
        if (st->hyps[h].log_prob > best->log_prob) {
            best = &st->hyps[h];
        }
    }
    int32_t matched = aura_context_graph_is_matched(st->graph, best->context_state);
    if (matched >= 0) {
        int32_t level = aura_context_graph_level(st->graph, matched);
        float   ys_prob = 0.0f;
        int32_t n = (level < best->ys_len) ? level : best->ys_len;
        for (int32_t i = 0; i < n; i++) {
            ys_prob += best->ys_probs[best->ys_len - n + i];
        }
        if (n > 0) {
            ys_prob /= (float)n;
        }
        float threshold = aura_context_graph_threshold(st->graph, matched);
        if (best->num_trailing_blanks > st->num_trailing_blanks &&
            ys_prob >= threshold) {
            kws_emit_hit(self, best, matched, ys_prob);
            /* 命中即复位：束回到单个空白假设（sherpa 同语义） */
            st->num_hyps = 0;
            kws_hyp_blank(&st->hyps[st->num_hyps++], root);
        }
    }
}

/* ------------------------------------------------------------ 回调 */

static bool kws_frame_in(aura_nn_node_t *self, const aura_audio_frame_t *frame)
{
    kws_state_t *st = kws_state(self);
    if (frame->frame_count > AURA_KWS_PCM_BUF) {
        return false;
    }
    /* S16 → float [-1,1]，喂特征前端；产出的 fbank 帧逐帧喂引擎 */
    const int16_t *pcm = (const int16_t *)frame->data;
    for (uint32_t i = 0; i < frame->frame_count; i++) {
        st->pcm_buf[i] = (float)pcm[i] * (1.0f / 32768.0f);
    }
    if (aura_feature_accept(st->feat, st->pcm_buf, (int32_t)frame->frame_count) !=
        AURA_OK) {
        return false;
    }
    while (aura_feature_ready(st->feat) > 0) {
        if (aura_feature_pop(st->feat, st->feat_buf) > 0) {
            aura_zipformer_accept(st->zf, st->feat_buf, 1);
        }
    }
    return aura_zipformer_ready(st->zf);
}

static aura_err_t kws_infer_out(aura_nn_node_t *self)
{
    kws_state_t *st = kws_state(self);

    /* 尾静音自动复位（sherpa DecodeStreams 开头同位置：1.5s 静音后复位状态与
     * 假设，但保留特征缓冲 —— 复位后本 chunk 照常解码） */
    if (st->num_hyps > 0 && st->hyps[0].num_trailing_blanks > st->reset_blanks) {
        AURA_LOGD(TAG, "%s: 尾静音自动复位", self->base.name);
        kws_auto_reset(st);
    }

    int32_t n = aura_zipformer_decode_chunk(st->zf);
    if (n < 0) {
        AURA_LOGE(TAG, "decode_chunk 失败: %d", n);
        return AURA_ERR_MODEL;
    }
    for (int32_t t = 0; t < n; t++) {
        kws_beam_step(self, st, t);
    }
    st->frame_offset += n;
    return AURA_OK;
}

static aura_err_t kws_algo_reset(aura_nn_node_t *self)
{
    kws_reset_stream(kws_state(self));
    return AURA_OK;
}

static void kws_deinit(aura_nn_node_t *self)
{
    kws_state_t *st = kws_state(self);
    if (st->zf != NULL) {
        aura_zipformer_destroy(st->zf);
        st->zf = NULL;
    }
    if (st->feat != NULL) {
        aura_feature_destroy(st->feat);
        st->feat = NULL;
    }
    if (st->graph != NULL) {
        aura_context_graph_destroy(st->graph);
        st->graph = NULL;
    }
    if (st->sym != NULL) {
        aura_symbol_table_destroy(st->sym);
        st->sym = NULL;
    }
}

/* ------------------------------------------------------------ 描述表 */

static const aura_algo_param_spec_t kws_param_specs[] = {
    {"encoder_file", AURA_ALGO_PARAM_STR, 0, 0,
     "encoder 模型路径（相对 model_root；默认 3.3M KWS 模型）"},
    {"decoder_file", AURA_ALGO_PARAM_STR, 0, 0,
     "decoder 模型路径（相对 model_root）"},
    {"joiner_file", AURA_ALGO_PARAM_STR, 0, 0,
     "joiner 模型路径（相对 model_root）"},
    {"tokens_file", AURA_ALGO_PARAM_STR, 0, 0,
     "tokens.txt 路径（相对 model_root）"},
    {"keywords_file", AURA_ALGO_PARAM_STR, 0, 0,
     "keywords 文件：每行空格分隔的符号串，@名字 后缀为显示名"},
    {"keywords_score", AURA_ALGO_PARAM_F32, 0, 100,
     "关键词 per-token 分数加成（sherpa 默认 1.0）"},
    {"keywords_threshold", AURA_ALGO_PARAM_F32, 0, 1,
     "命中所需匹配 token 概率均值（sherpa 默认 0.25）"},
    {"max_active_paths", AURA_ALGO_PARAM_I32, 1, AURA_KWS_MAX_PATHS,
     "束宽（sherpa 默认 4）"},
    {"num_trailing_blanks", AURA_ALGO_PARAM_I32, 0, 100,
     "命中所需尾静音输出帧数（sherpa 默认 1）"},
    {"auto_reset_seconds", AURA_ALGO_PARAM_F32, 0.1f, 60,
     "尾静音自动复位时长（sherpa 默认 1.5s）"},
};

static aura_nn_desc_t kws_desc = {
    .name        = "kws",
    .kind        = AURA_ALGO_KIND_NN_SYNC,
    .io_kind     = AURA_NN_IO_PASSTHROUGH,
    .form        = AURA_INFER_FORM_MODULE,
    .model_file  = NULL, /* 引擎自装三模型 */
    .wants_events = false,
    .private_group = true, /* chunk 解码 20–50ms，独占推理组不与 VAD 抢池 */
    .state_size  = sizeof(kws_state_t),
    .algo_init   = kws_init,
    .frame_in    = kws_frame_in,
    .infer_out   = kws_infer_out,
    .algo_reset  = kws_algo_reset,
    .algo_deinit = kws_deinit,
    .param_specs = kws_param_specs,
    .param_spec_count = sizeof(kws_param_specs) / sizeof(kws_param_specs[0]),
};

aura_err_t aura_kws_register(void)
{
    /* 幂等：注册表是进程级的，重名时"谁先注册谁生效"（与 trickroom 注册同向，
     * 见 aura_dsp_trickroom_register 的注释）。 */
    if (aura_algo_find("kws") != NULL) {
        return AURA_OK;
    }
    return aura_nn_register_all(&kws_desc, 1);
}
