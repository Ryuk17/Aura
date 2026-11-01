/* Aura — zipformer 流式引擎实现（v1 与 v2 双形态）
 *
 * 照搬 sherpa-mnn 的契约，用本仓库的 inference C 接口重写。按 metadata 的
 * `model_type` 分派两种状态布局：
 *
 *   zipformer2（KWS 3.3M）：每个 block **每层** 6 个状态（cached_key /
 *     cached_nonlin_attn / cached_val1 / cached_val2 / cached_conv1 /
 *     cached_conv2，见 online-zipformer2-transducer-model.cc），全部之后接
 *     embed_states [1,128,3,19] 与 processed_lens [1] int32，共 6m+2 个
 *     （m = 各 block 层数之和）；形状取自 encoder_dims / query_head_dims /
 *     value_head_dims / num_heads / num_encoder_layers / cnn_module_kernels /
 *     left_context_len。
 *
 *   zipformer（ASR bilingual）：每个 **block** 7 个状态（cached_len int32 [L,1] /
 *     cached_avg [L,1,D] / cached_key [L,left,1,key_dim] / cached_val / cached_val2
 *     [L,left,1,D/4] / cached_conv1 / cached_conv2 [L,1,D,k-1]，见
 *     online-zipformer-transducer-model.cc），共 7×nblock 个，**没有 processed_lens**；
 *     key 维取 attention_dims。
 *
 *   - 解码循环：encoder 整 chunk 跑（batch=1，不做 sherpa 的 Stack/UnStack），
 *     输出帧逐帧走 decoder+joiner —— 本文件只提供"砖块"（decode_frame），
 *     贪心/束搜索/关键词受限解码由调用方拼（KWS 节点、ASR 节点各自拼）。
 *
 * 内存：全部缓冲在 create 时一次分配（特征环 + 状态 + encoder_out），
 * 运行期零 malloc/free。
 */
#include "dsp/zipformer/aura_zipformer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils/logger/logger.h"

#define TAG "dsp.zf"

/* 单个状态张量：输入/输出句柄 + 形状 + 读写缓冲。
 * processed_lens 是 int32（sherpa 用 MNNUtilsCreateTensor<int>），其余全是 f32；
 * int32 与 float 同宽，缓冲统一按字节分配。 */
typedef struct {
    aura_tensor_t *in;
    aura_tensor_t *out;
    char           name[48]; /* v1 名字绑定的规范名（v2 不用） */
    int32_t        dims[AURA_TENSOR_MAX_DIMS];
    int32_t        dim_count;
    uint64_t       bytes; /* 缓冲字节数（= 元素数 × 4） */
    float         *buf;
    bool           is_int;
} zf_state_t;

struct aura_zipformer {
    aura_infer_model_t *encoder;
    aura_infer_model_t *decoder;
    aura_infer_model_t *joiner;

    /* 超参（metadata，装配期一次性读齐） */
    int32_t T;             /* chunk 输入帧数 */
    int32_t chunk_shift;   /* 每次前移帧数（decode_chunk_len） */
    int32_t out_frames;    /* chunk_shift / 4（zipformer2 下采样固定 4） */
    int32_t vocab_size;
    int32_t context_size;
    int32_t feature_dim;
    int32_t enc_dim;       /* encoder_out 每帧宽度 = joiner 输入宽 */
    int32_t dec_dim;       /* decoder_out 宽度 = joiner 另一输入宽 */

    int32_t     num_states;
    zf_state_t *states;
    bool        has_processed_lens; /* v2 有 processed_lens 状态，v1 没有 */

    /* 特征环形缓冲：cap = T + chunk_shift，保证"读 T 帧"与"写新帧"永不重叠 */
    float  *feat_buf;
    int32_t feat_cap;
    int32_t feat_start;
    int32_t feat_count;
    int64_t processed_frames; /* 已消费帧数 = processed_lens 张量的值 */

    /* 每 chunk 的暂存 */
    float *x_buf;   /* T × feature_dim：环形 → 连续的 encoder 输入 */
    float *enc_out; /* out_frames × enc_dim：encoder 输出回读 */
    float *dec_buf; /* dec_dim：decoder 输出回读 */
};

/* ------------------------------------------------------------ 小工具 */

static void zf_set_shape(zf_state_t *s, int32_t d0, int32_t d1, int32_t d2,
                         int32_t d3)
{
    s->dim_count = 0;
    if (d0 > 0) s->dims[s->dim_count++] = d0;
    if (d1 > 0) s->dims[s->dim_count++] = d1;
    if (d2 > 0) s->dims[s->dim_count++] = d2;
    if (d3 > 0) s->dims[s->dim_count++] = d3;
    s->bytes = 4;
    for (int32_t i = 0; i < s->dim_count; i++) {
        s->bytes *= (uint64_t)s->dims[i];
    }
}

/* 状态张量对齐（dtype / 形状 / resize / 字节数）。s->in 必须已找到。
 * 装配期校验：动态维（-1）resize 成 metadata 推出的实际值，静态维必须完全一致
 * —— 不一致说明模型与代码里的形状假设对不上，装配期报错而不是跑出垃圾数。
 * 输出：优先同名（sherpa 导出时状态同名进出），转换器改了名就回退位置绑定。 */
static bool zf_align_state(aura_infer_model_t *encoder, int32_t out_idx,
                           zf_state_t *s)
{
    aura_tensor_info_t ti;
    if (aura_tensor_get_info(s->in, &ti) != AURA_OK) {
        AURA_LOGE(TAG, "取不到状态张量信息");
        return false;
    }

    /* dtype：cached_len（v1）/processed_lens（v2）是 s32，其余 f32 */
    aura_dtype_t want = s->is_int ? AURA_DTYPE_S32 : AURA_DTYPE_F32;
    if (ti.dtype != want) {
        AURA_LOGE(TAG, "状态张量 %s dtype %s，期望 %s", ti.name,
                  aura_dtype_name(ti.dtype), aura_dtype_name(want));
        return false;
    }

    /* 形状对齐 */
    bool dyn = false;
    for (int32_t d = 0; d < ti.dim_count; d++) {
        if (ti.dims[d] < 0) {
            dyn = true;
        } else if (d < s->dim_count && ti.dims[d] != s->dims[d]) {
            AURA_LOGE(TAG, "状态张量 %s 形状与 metadata 推出的不符（声明维 %d = %d，"
                           "期望 %d）", ti.name, d, ti.dims[d], s->dims[d]);
            return false;
        }
    }
    if (dyn) {
        if (aura_tensor_resize(s->in, s->dims, s->dim_count) != AURA_OK) {
            AURA_LOGE(TAG, "resize 状态张量 %s 失败", ti.name);
            return false;
        }
    }
    if (aura_tensor_bytes(s->in) != s->bytes) {
        AURA_LOGE(TAG, "状态张量 %s 实际 %llu B，期望 %llu B", ti.name,
                  (unsigned long long)aura_tensor_bytes(s->in),
                  (unsigned long long)s->bytes);
        return false;
    }

    /* 输出张量：优先同名；v1 的转换器给状态输出加了 new_ 前缀（new_cached_*），
     * 由 zf_bind_state_named 预先找好；再找不到就回退位置绑定
     * （out_idx = 状态下标 + 1，[encoder_out, 状态们] 同序 —— v2 实测如此）。 */
    if (s->out == NULL) {
        s->out = aura_model_output(encoder, ti.name);
    }
    if (s->out == NULL) {
        AURA_LOGD(TAG, "状态 %s 无同名输出，回退位置绑定（输出 %d）", ti.name, out_idx);
        aura_tensor_info_t oi;
        if (aura_model_output_info(encoder, out_idx, &oi) != AURA_OK ||
            (s->out = aura_model_output(encoder, oi.name)) == NULL) {
            AURA_LOGE(TAG, "找不到状态张量 %s 的输出（位置 %d）", ti.name, out_idx);
            return false;
        }
    }
    return true;
}

/* v2：位置绑定（KWS 模型的输入顺序 = sherpa 的状态向量顺序，实测如此）。 */
static bool zf_bind_state_pos(aura_infer_model_t *encoder, int32_t input_idx,
                              zf_state_t *s)
{
    aura_tensor_info_t ti;
    if (aura_model_input_info(encoder, input_idx, &ti) != AURA_OK) {
        AURA_LOGE(TAG, "取不到 encoder 输入 %d 的信息", input_idx);
        return false;
    }
    s->in = aura_model_input(encoder, ti.name);
    if (s->in == NULL) {
        AURA_LOGE(TAG, "找不到 encoder 输入 %d (%s)", input_idx, ti.name);
        return false;
    }
    return zf_align_state(encoder, input_idx, s);
}

/* v1：名字绑定（转换器把输入按类型分组重排过，位置不可靠；
 * 名字用 sherpa 导出的规范名 cached_<type>_<block>）。 */
static bool zf_bind_state_named(aura_infer_model_t *encoder, int32_t out_idx,
                                const char *name, zf_state_t *s)
{
    s->in = aura_model_input(encoder, name);
    if (s->in == NULL) {
        AURA_LOGE(TAG, "找不到状态张量 %s（v1 需要规范名）", name);
        return false;
    }
    /* 输出：转换器把状态输出命名成 new_cached_<type>_<block>（实测），
     * 先按这个找；没有 new_ 前缀的转换器由 zf_align_state 再试同名/位置。 */
    char out_name[56];
    snprintf(out_name, sizeof(out_name), "new_%s", name);
    s->out = aura_model_output(encoder, out_name);
    return zf_align_state(encoder, out_idx, s);
}

/* ------------------------------------------------------------ 生命周期 */

/* 用 enum 而不是 static const：C 里 const 变量不是常量表达式，
 * 拿它开数组就成了 VLA，而 goto fail 不允许跳过 VLA 的作用域。 */
enum { AURA_ZF_MAX_BLOCKS = 16 };

aura_zipformer_t *aura_zipformer_create(const aura_zipformer_cfg_t *cfg)
{
    if (cfg == NULL || cfg->encoder_path == NULL || cfg->decoder_path == NULL ||
        cfg->joiner_path == NULL || cfg->feature_dim <= 0) {
        AURA_LOGE(TAG, "配置非法（路径/feature_dim）");
        return NULL;
    }

    aura_zipformer_t *z = (aura_zipformer_t *)calloc(1, sizeof(*z));
    if (z == NULL) {
        return NULL;
    }
    z->feature_dim = cfg->feature_dim;

    z->encoder = aura_infer_load_in_group(cfg->group, cfg->encoder_path,
                                          AURA_INFER_FORM_MODULE);
    z->decoder = aura_infer_load_in_group(cfg->group, cfg->decoder_path,
                                          AURA_INFER_FORM_MODULE);
    z->joiner  = aura_infer_load_in_group(cfg->group, cfg->joiner_path,
                                          AURA_INFER_FORM_MODULE);
    if (z->encoder == NULL || z->decoder == NULL || z->joiner == NULL) {
        AURA_LOGE(TAG, "模型加载失败: enc=%p dec=%p joiner=%p",
                  (void *)z->encoder, (void *)z->decoder, (void *)z->joiner);
        goto fail;
    }

    /* ---- model_type 分派（v1 = zipformer，v2 = zipformer2，状态布局不同） */
    char mt[32];
    bool is_v1 = false, is_v2 = false;
    if (aura_model_meta(z->encoder, "model_type", mt, sizeof(mt)) == AURA_OK) {
        is_v1 = (strcmp(mt, "zipformer") == 0);
        is_v2 = (strcmp(mt, "zipformer2") == 0);
    }
    if (!is_v1 && !is_v2) {
        AURA_LOGE(TAG, "model_type 未知（不是 zipformer/zipformer2），无法确定状态布局");
        goto fail;
    }

    /* ---- metadata 读齐（一个缺失都不能硬编码兜底） */
    int32_t encoder_dims[AURA_ZF_MAX_BLOCKS], query_head_dims[AURA_ZF_MAX_BLOCKS],
        value_head_dims[AURA_ZF_MAX_BLOCKS], num_heads[AURA_ZF_MAX_BLOCKS],
        num_encoder_layers[AURA_ZF_MAX_BLOCKS], cnn_module_kernels[AURA_ZF_MAX_BLOCKS],
        left_context_len[AURA_ZF_MAX_BLOCKS], attention_dims[AURA_ZF_MAX_BLOCKS];
    int32_t nblk = aura_model_meta_i32vec(z->encoder, "encoder_dims", encoder_dims,
                                          AURA_ZF_MAX_BLOCKS);
    int32_t nq = aura_model_meta_i32vec(z->encoder, "query_head_dims", query_head_dims,
                                        AURA_ZF_MAX_BLOCKS);
    int32_t nv = aura_model_meta_i32vec(z->encoder, "value_head_dims", value_head_dims,
                                        AURA_ZF_MAX_BLOCKS);
    int32_t nh = aura_model_meta_i32vec(z->encoder, "num_heads", num_heads, AURA_ZF_MAX_BLOCKS);
    int32_t nl = aura_model_meta_i32vec(z->encoder, "num_encoder_layers",
                                        num_encoder_layers, AURA_ZF_MAX_BLOCKS);
    int32_t nk = aura_model_meta_i32vec(z->encoder, "cnn_module_kernels",
                                        cnn_module_kernels, AURA_ZF_MAX_BLOCKS);
    int32_t nL = aura_model_meta_i32vec(z->encoder, "left_context_len",
                                        left_context_len, AURA_ZF_MAX_BLOCKS);
    int32_t na = aura_model_meta_i32vec(z->encoder, "attention_dims", attention_dims,
                                        AURA_ZF_MAX_BLOCKS);
    if (nblk == 0 || nl != nblk || nk != nblk || nL != nblk) {
        AURA_LOGE(TAG, "encoder metadata 缺失或长度不一（encoder_dims=%d）", nblk);
        goto fail;
    }
    if (is_v2 && (nq != nblk || nv != nblk || nh != nblk)) {
        AURA_LOGE(TAG, "zipformer2 缺 query/value_head_dims/num_heads");
        goto fail;
    }
    if (is_v1 && na != nblk) {
        AURA_LOGE(TAG, "zipformer 缺 attention_dims");
        goto fail;
    }

    z->T           = aura_model_meta_i32(z->encoder, "T", 0);
    z->chunk_shift = aura_model_meta_i32(z->encoder, "decode_chunk_len", 0);
    if (z->T <= 0 || z->chunk_shift <= 0 || z->chunk_shift > z->T ||
        z->chunk_shift % 4 != 0) {
        AURA_LOGE(TAG, "T/decode_chunk_len 非法: T=%d chunk_shift=%d", z->T,
                  z->chunk_shift);
        goto fail;
    }
    z->out_frames = z->chunk_shift / 4;

    z->vocab_size   = aura_model_meta_i32(z->decoder, "vocab_size", 0);
    z->context_size = aura_model_meta_i32(z->decoder, "context_size", 0);
    if (z->vocab_size <= 0 || z->context_size <= 0) {
        AURA_LOGE(TAG, "decoder metadata 缺失: vocab=%d context=%d", z->vocab_size,
                  z->context_size);
        goto fail;
    }

    /* ---- 状态形状（两种布局，见文件头注释） */
    for (int32_t b = 0; b < nblk; b++) {
        if (num_encoder_layers[b] <= 0 || left_context_len[b] <= 0 ||
            encoder_dims[b] <= 0 || cnn_module_kernels[b] <= 0) {
            AURA_LOGE(TAG, "block %d 超参非法", b);
            goto fail;
        }
    }
    if (is_v2) {
        int32_t m = 0;
        for (int32_t b = 0; b < nblk; b++) {
            m += num_encoder_layers[b];
        }
        z->num_states = 6 * m + 2; /* 6/层 + embed_states + processed_lens */
        z->has_processed_lens = true;
    } else {
        z->num_states = 7 * nblk; /* 7/block，无 processed_lens */
        z->has_processed_lens = false;
    }
    z->states = (zf_state_t *)calloc((size_t)z->num_states, sizeof(*z->states));
    if (z->states == NULL) {
        goto fail;
    }

    int32_t si = 0;
    if (is_v2) {
        for (int32_t b = 0; b < nblk; b++) {
            int32_t key_dim    = query_head_dims[b] * num_heads[b];
            int32_t value_dim  = value_head_dims[b] * num_heads[b];
            int32_t nonlin_dim = 3 * encoder_dims[b] / 4;
            int32_t left       = left_context_len[b];
            int32_t conv_len   = cnn_module_kernels[b] / 2;
            for (int32_t j = 0; j < num_encoder_layers[b]; j++, si += 6) {
                zf_set_shape(&z->states[si + 0], left, 1, key_dim, 0);      /* cached_key */
                zf_set_shape(&z->states[si + 1], 1, 1, left, nonlin_dim);   /* cached_nonlin_attn */
                zf_set_shape(&z->states[si + 2], left, 1, value_dim, 0);    /* cached_val1 */
                zf_set_shape(&z->states[si + 3], left, 1, value_dim, 0);    /* cached_val2 */
                zf_set_shape(&z->states[si + 4], 1, encoder_dims[b], conv_len, 0); /* cached_conv1 */
                zf_set_shape(&z->states[si + 5], 1, encoder_dims[b], conv_len, 0); /* cached_conv2 */
            }
        }
        int32_t embed_dim = (((z->feature_dim - 1) / 2) - 1) / 2; /* 80 → 19 */
        zf_set_shape(&z->states[si + 0], 1, 128, 3, embed_dim);   /* embed_states */
        zf_set_shape(&z->states[si + 1], 1, 0, 0, 0);             /* processed_lens */
        z->states[si + 1].is_int = true;
    } else {
        /* v1（sherpa online-zipformer-transducer-model.cc）：状态按 **block** 排，
         * 每 block 7 个 —— cached_len 是 int32。左上下文/维数逐 block 不同。
         * 名字 = sherpa 导出的规范名（转换器会按名字重排输入顺序，绑定靠它）。 */
        for (int32_t b = 0; b < nblk; b++, si += 7) {
            int32_t L         = num_encoder_layers[b];
            int32_t D         = encoder_dims[b];
            int32_t key_dim   = attention_dims[b];
            int32_t value_dim = D / 4;
            int32_t left      = left_context_len[b];
            int32_t conv_len  = cnn_module_kernels[b] - 1;
            snprintf(z->states[si + 0].name, sizeof(z->states[si + 0].name),
                     "cached_len_%d", b);
            snprintf(z->states[si + 1].name, sizeof(z->states[si + 1].name),
                     "cached_avg_%d", b);
            snprintf(z->states[si + 2].name, sizeof(z->states[si + 2].name),
                     "cached_key_%d", b);
            snprintf(z->states[si + 3].name, sizeof(z->states[si + 3].name),
                     "cached_val_%d", b);
            snprintf(z->states[si + 4].name, sizeof(z->states[si + 4].name),
                     "cached_val2_%d", b);
            snprintf(z->states[si + 5].name, sizeof(z->states[si + 5].name),
                     "cached_conv1_%d", b);
            snprintf(z->states[si + 6].name, sizeof(z->states[si + 6].name),
                     "cached_conv2_%d", b);
            zf_set_shape(&z->states[si + 0], L, 1, 0, 0);            /* cached_len (int) */
            zf_set_shape(&z->states[si + 1], L, 1, D, 0);            /* cached_avg */
            zf_set_shape(&z->states[si + 2], L, left, 1, key_dim);   /* cached_key */
            zf_set_shape(&z->states[si + 3], L, left, 1, value_dim); /* cached_val */
            zf_set_shape(&z->states[si + 4], L, left, 1, value_dim); /* cached_val2 */
            zf_set_shape(&z->states[si + 5], L, 1, D, conv_len);     /* cached_conv1 */
            zf_set_shape(&z->states[si + 6], L, 1, D, conv_len);     /* cached_conv2 */
            z->states[si + 0].is_int = true;
        }
    }

    /* ---- 张量绑定与装配期校验（形状/dtype 全在这时对死） */
    int32_t enc_in_n  = aura_model_input_count(z->encoder);
    int32_t enc_out_n = aura_model_output_count(z->encoder);
    if (enc_in_n != 1 + z->num_states || enc_out_n != 1 + z->num_states) {
        AURA_LOGE(TAG, "encoder 张量数不符: %d in / %d out，期望 %d / %d", enc_in_n,
                  enc_out_n, 1 + z->num_states, 1 + z->num_states);
        goto fail;
    }
    /* 诊断：v1 的转换器会重排输入输出顺序，绑定靠名字。把输出名单打出来，
     * 换模型/换转换器时这里就是排查的第一现场。 */
    if (is_v1) {
        for (int32_t i = 0; i < enc_out_n; i++) {
            aura_tensor_info_t oi;
            if (aura_model_output_info(z->encoder, i, &oi) == AURA_OK) {
                AURA_LOGI(TAG, "v1 encoder 输出[%d] = %s", i, oi.name);
            }
        }
    }
    for (int32_t i = 0; i < z->num_states; i++) {
        bool ok = is_v1
                      ? zf_bind_state_named(z->encoder, i + 1, z->states[i].name,
                                            &z->states[i])
                      : zf_bind_state_pos(z->encoder, i + 1, &z->states[i]);
        if (!ok) {
            goto fail;
        }
        z->states[i].buf = (float *)calloc(1, (size_t)z->states[i].bytes);
        if (z->states[i].buf == NULL) {
            goto fail;
        }
    }

    /* encoder 输入 x = [1, T, feature_dim] */
    {
        aura_tensor_info_t ti;
        if (aura_model_input_info(z->encoder, 0, &ti) != AURA_OK) {
            AURA_LOGE(TAG, "取不到 encoder 输入 x 的信息");
            goto fail;
        }
        int32_t want[3] = {1, z->T, z->feature_dim};
        bool    dyn     = false;
        for (int32_t d = 0; d < ti.dim_count; d++) {
            if (ti.dims[d] < 0) {
                dyn = true;
            } else if (d < 3 && ti.dims[d] != want[d]) {
                AURA_LOGE(TAG, "encoder 输入 x 形状不符（声明维 %d = %d，期望 %d）", d,
                          ti.dims[d], want[d]);
                goto fail;
            }
        }
        if (dyn) {
            aura_tensor_t *t = aura_model_input(z->encoder, ti.name);
            if (t == NULL || aura_tensor_resize(t, want, 3) != AURA_OK) {
                AURA_LOGE(TAG, "resize encoder 输入 x 失败");
                goto fail;
            }
        }
    }

    /* decoder：y = [1, context_size] s32。
     * **输出形状加载时是动态的**（MODULE 形态下 probe 显示 (scalar)，跑过一次
     * 前向才落地 [1, 320]）—— 所以 dec_dim 不从这里取，取 joiner 的第二个输入
     * （那就是 decoder_out，声明为静态），一致性在第一次 decode_frame 时验。 */
    {
        aura_tensor_info_t ti;
        if (aura_model_input_count(z->decoder) != 1 ||
            aura_model_input_info(z->decoder, 0, &ti) != AURA_OK ||
            ti.dtype != AURA_DTYPE_S32 ||
            ti.dim_count != 2 || ti.dims[1] != z->context_size) {
            AURA_LOGE(TAG, "decoder 输入契约不符（期望 y[1,%d] s32）", z->context_size);
            goto fail;
        }
        if (aura_model_output_count(z->decoder) != 1) {
            AURA_LOGE(TAG, "decoder 输出契约不符");
            goto fail;
        }
    }

    /* joiner：2 个 f32 输入 [1,enc_dim]/[1,dec_dim]；输出形状也是动态的，
     * vocab 维度在第一次 decode_frame 时按字节数校验。 */
    {
        aura_tensor_info_t ti0, ti1;
        if (aura_model_input_count(z->joiner) != 2 ||
            aura_model_input_info(z->joiner, 0, &ti0) != AURA_OK ||
            aura_model_input_info(z->joiner, 1, &ti1) != AURA_OK) {
            AURA_LOGE(TAG, "joiner 输入契约不符（期望 2 个 f32）");
            goto fail;
        }
        z->enc_dim = ti0.dims[ti0.dim_count - 1];
        z->dec_dim = ti1.dims[ti1.dim_count - 1];
        if (z->enc_dim <= 0 || z->dec_dim <= 0) {
            AURA_LOGE(TAG, "joiner 输入维非法: enc=%d dec=%d", z->enc_dim, z->dec_dim);
            goto fail;
        }
        if (aura_model_output_count(z->joiner) != 1) {
            AURA_LOGE(TAG, "joiner 输出契约不符");
            goto fail;
        }
    }

    /* ---- 运行期缓冲一次配齐。
     * 环容量 = T + 16×shift（≈5.5s）：全局 Express 锁会让组间前向互相等待，
     * 下游 worker（ASR）批量解码时会成串饿着 SYNC 节点（KWS）—— 大环把这些
     * 调度脉冲吸收掉。实测 3×shift（1.4s）在全链负载下溢出丢帧，
     * 尾静音被丢 → KWS 命中所需的 trailing blanks 永远数不满 → 零命中。 */
    z->feat_cap = z->T + 16 * z->chunk_shift;
    z->feat_buf = (float *)calloc((size_t)z->feat_cap * (size_t)z->feature_dim,
                                  sizeof(float));
    z->x_buf = (float *)calloc((size_t)z->T * (size_t)z->feature_dim, sizeof(float));
    z->enc_out = (float *)calloc((size_t)z->out_frames * (size_t)z->enc_dim,
                                 sizeof(float));
    z->dec_buf = (float *)calloc((size_t)z->dec_dim, sizeof(float));
    if (z->feat_buf == NULL || z->x_buf == NULL || z->enc_out == NULL ||
        z->dec_buf == NULL) {
        goto fail;
    }

    AURA_LOGI(TAG, "zipformer2 就绪: T=%d shift=%d out_frames=%d vocab=%d "
                   "context=%d enc_dim=%d dec_dim=%d states=%d",
              z->T, z->chunk_shift, z->out_frames, z->vocab_size, z->context_size,
              z->enc_dim, z->dec_dim, z->num_states);
    return z;

fail:
    aura_zipformer_destroy(z);
    return NULL;
}

void aura_zipformer_destroy(aura_zipformer_t *z)
{
    if (z == NULL) {
        return;
    }
    if (z->encoder != NULL) {
        aura_infer_unload(z->encoder);
    }
    if (z->decoder != NULL) {
        aura_infer_unload(z->decoder);
    }
    if (z->joiner != NULL) {
        aura_infer_unload(z->joiner);
    }
    if (z->states != NULL) {
        for (int32_t i = 0; i < z->num_states; i++) {
            free(z->states[i].buf);
        }
        free(z->states);
    }
    free(z->feat_buf);
    free(z->x_buf);
    free(z->enc_out);
    free(z->dec_buf);
    free(z);
}

/* ------------------------------------------------------------ 超参访问 */

int32_t aura_zipformer_T(const aura_zipformer_t *z) { return z ? z->T : 0; }
int32_t aura_zipformer_chunk_shift(const aura_zipformer_t *z)
{
    return z ? z->chunk_shift : 0;
}
int32_t aura_zipformer_output_frames(const aura_zipformer_t *z)
{
    return z ? z->out_frames : 0;
}
int32_t aura_zipformer_vocab_size(const aura_zipformer_t *z)
{
    return z ? z->vocab_size : 0;
}
int32_t aura_zipformer_context_size(const aura_zipformer_t *z)
{
    return z ? z->context_size : 0;
}
int32_t aura_zipformer_encoder_dim(const aura_zipformer_t *z)
{
    return z ? z->enc_dim : 0;
}

/* ------------------------------------------------------------ 流式接口 */

void aura_zipformer_reset(aura_zipformer_t *z)
{
    if (z == NULL) {
        return;
    }
    aura_zipformer_reset_states(z);
    z->feat_start       = 0;
    z->feat_count       = 0;
    z->processed_frames = 0;
}

void aura_zipformer_reset_states(aura_zipformer_t *z)
{
    if (z == NULL) {
        return;
    }
    for (int32_t i = 0; i < z->num_states; i++) {
        memset(z->states[i].buf, 0, (size_t)z->states[i].bytes);
    }
}

int32_t aura_zipformer_accept(aura_zipformer_t *z, const float *features,
                              int32_t n_frames)
{
    if (z == NULL || (features == NULL && n_frames > 0) || n_frames < 0) {
        return AURA_ERR_INVALID_ARG;
    }
    for (int32_t i = 0; i < n_frames; i++) {
        if (z->feat_count == z->feat_cap) {
            /* 溢出只发生在调用方从不检查 ready 的情况下 —— 丢最旧一帧保时序，
             * 不静默（logger 自带限速）。 */
            AURA_LOGW(TAG, "特征缓冲溢出（cap=%d），丢最旧一帧", z->feat_cap);
            z->feat_start = (z->feat_start + 1) % z->feat_cap;
            z->feat_count--;
        }
        int32_t w = (z->feat_start + z->feat_count) % z->feat_cap;
        memcpy(z->feat_buf + (size_t)w * (size_t)z->feature_dim,
               features + (size_t)i * (size_t)z->feature_dim,
               sizeof(float) * (size_t)z->feature_dim);
        z->feat_count++;
    }
    return z->feat_count;
}

int32_t aura_zipformer_num_frames(const aura_zipformer_t *z)
{
    return z ? z->feat_count : 0;
}

bool aura_zipformer_ready(const aura_zipformer_t *z)
{
    /* sherpa 的 IsReady 是严格大于：processed + T < ready。
     * 这里 feat_count 已经扣掉 consumed，故等价为 feat_count > T。 */
    return z != NULL && z->feat_count > z->T;
}

int32_t aura_zipformer_decode_chunk(aura_zipformer_t *z)
{
    if (z == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    if (!aura_zipformer_ready(z)) {
        AURA_LOGW(TAG, "未攒够帧就 decode（count=%d, T=%d）", z->feat_count, z->T);
        return AURA_ERR_STATE;
    }

    /* x：环形缓冲 → 连续 T 帧 */
    for (int32_t i = 0; i < z->T; i++) {
        int32_t idx = (z->feat_start + i) % z->feat_cap;
        memcpy(z->x_buf + (size_t)i * (size_t)z->feature_dim,
               z->feat_buf + (size_t)idx * (size_t)z->feature_dim,
               sizeof(float) * (size_t)z->feature_dim);
    }
    aura_tensor_t *x = aura_model_input(z->encoder, NULL);
    if (x == NULL || aura_tensor_write(x, z->x_buf, aura_tensor_bytes(x)) != AURA_OK) {
        AURA_LOGE(TAG, "写 encoder 输入 x 失败");
        return AURA_ERR_MODEL;
    }

    /* processed_lens（仅 v2）= 本 chunk 开始时已处理的帧数（sherpa 同语义） */
    if (z->has_processed_lens) {
        *(int32_t *)z->states[z->num_states - 1].buf = (int32_t)z->processed_frames;
    }

    for (int32_t i = 0; i < z->num_states; i++) {
        if (aura_tensor_write(z->states[i].in, z->states[i].buf,
                              z->states[i].bytes) != AURA_OK) {
            AURA_LOGE(TAG, "写 encoder 状态 %d 失败", i);
            return AURA_ERR_MODEL;
        }
    }

    if (aura_model_run(z->encoder) != AURA_OK) {
        AURA_LOGE(TAG, "encoder 前向失败");
        return AURA_ERR_MODEL;
    }

    /* 回读 encoder_out 与全部新状态 */
    aura_tensor_t *eo = aura_model_output(z->encoder, NULL);
    if (eo == NULL) {
        AURA_LOGE(TAG, "取不到 encoder_out");
        return AURA_ERR_MODEL;
    }
    uint64_t want = (uint64_t)z->out_frames * (uint64_t)z->enc_dim * 4;
    if (aura_tensor_bytes(eo) != want ||
        aura_tensor_read(eo, z->enc_out, want) != AURA_OK) {
        AURA_LOGE(TAG, "encoder_out 回读失败（实际 %llu B，期望 %llu B）",
                  (unsigned long long)aura_tensor_bytes(eo), (unsigned long long)want);
        return AURA_ERR_MODEL;
    }
    for (int32_t i = 0; i < z->num_states; i++) {
        if (aura_tensor_bytes(z->states[i].out) != z->states[i].bytes ||
            aura_tensor_read(z->states[i].out, z->states[i].buf,
                             z->states[i].bytes) != AURA_OK) {
            AURA_LOGE(TAG, "回读 encoder 状态 %d 失败", i);
            return AURA_ERR_MODEL;
        }
    }

    z->processed_frames += z->chunk_shift;
    z->feat_start = (z->feat_start + z->chunk_shift) % z->feat_cap;
    z->feat_count -= z->chunk_shift;
    return z->out_frames;
}

aura_err_t aura_zipformer_decode_frame(aura_zipformer_t *z, int32_t t,
                                       const int32_t *ys, float *logits)
{
    if (z == NULL || t < 0 || t >= z->out_frames || ys == NULL || logits == NULL) {
        return AURA_ERR_INVALID_ARG;
    }

    /* decoder：y = [1, context_size] s32 */
    aura_tensor_t *y = aura_model_input(z->decoder, NULL);
    if (y == NULL ||
        aura_tensor_write(y, ys, (uint64_t)z->context_size * 4) != AURA_OK) {
        AURA_LOGE(TAG, "写 decoder 输入失败");
        return AURA_ERR_MODEL;
    }
    if (aura_model_run(z->decoder) != AURA_OK) {
        AURA_LOGE(TAG, "decoder 前向失败");
        return AURA_ERR_MODEL;
    }
    aura_tensor_t *do_ = aura_model_output(z->decoder, NULL);
    /* 输出形状加载时是动态的，跑完第一帧才落地 —— 与 joiner 输入维对账在此。 */
    if (do_ == NULL || aura_tensor_bytes(do_) != (uint64_t)z->dec_dim * 4 ||
        aura_tensor_read(do_, z->dec_buf, (uint64_t)z->dec_dim * 4) != AURA_OK) {
        AURA_LOGE(TAG, "decoder_out 形状对不上（实际 %llu B，期望 %llu B）",
                  (unsigned long long)(do_ ? aura_tensor_bytes(do_) : 0),
                  (unsigned long long)(uint64_t)z->dec_dim * 4);
        return AURA_ERR_MODEL;
    }

    /* joiner：encoder 第 t 帧 + decoder_out */
    aura_tensor_t *j0 = aura_model_input(z->joiner, NULL);
    aura_tensor_t *j1 = NULL;
    {
        aura_tensor_info_t ti;
        if (aura_model_input_info(z->joiner, 1, &ti) == AURA_OK) {
            j1 = aura_model_input(z->joiner, ti.name);
        }
    }
    if (j0 == NULL || j1 == NULL ||
        aura_tensor_write(j0, z->enc_out + (size_t)t * (size_t)z->enc_dim,
                          (uint64_t)z->enc_dim * 4) != AURA_OK ||
        aura_tensor_write(j1, z->dec_buf, (uint64_t)z->dec_dim * 4) != AURA_OK) {
        AURA_LOGE(TAG, "写 joiner 输入失败");
        return AURA_ERR_MODEL;
    }
    if (aura_model_run(z->joiner) != AURA_OK) {
        AURA_LOGE(TAG, "joiner 前向失败");
        return AURA_ERR_MODEL;
    }
    aura_tensor_t *lo = aura_model_output(z->joiner, NULL);
    /* 同 decoder_out：joiner 输出形状动态，落地后按字节数对 vocab_size。 */
    if (lo == NULL || aura_tensor_bytes(lo) != (uint64_t)z->vocab_size * 4 ||
        aura_tensor_read(lo, logits, (uint64_t)z->vocab_size * 4) != AURA_OK) {
        AURA_LOGE(TAG, "logits 形状对不上（实际 %llu B，期望 %llu B）",
                  (unsigned long long)(lo ? aura_tensor_bytes(lo) : 0),
                  (unsigned long long)(uint64_t)z->vocab_size * 4);
        return AURA_ERR_MODEL;
    }
    return AURA_OK;
}
