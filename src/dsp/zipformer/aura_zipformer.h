/* Aura — zipformer2 流式引擎（encoder + decoder + joiner）
 *
 * KWS 与 ASR 共用：sherpa-onnx 的 zipformer2 transducer 模型族（KWS 3.3M /
 * ASR bilingual）都是"encoder 吃 chunk 特征、decoder 吃上下文 token、joiner
 * 拼两者出 logits"的同一套结构，差异只在权重与超参 —— 而超参全部来自模型
 * metadata（T / decode_chunk_len / 各 block 的层数与左上下文 / vocab_size /
 * context_size），所以一份引擎代码两边都能用。
 *
 * 流式契约（照搬 sherpa-mnn online-zipformer2-transducer-model.cc 与
 * keyword-spotter-transducer-impl.h 的语义）：
 *   - accept 攒 fbank 帧；攒够 **T+1** 帧后 ready（sherpa 的 IsReady 是严格
 *     大于：processed + T < ready）
 *   - decode_chunk 一次吃 T 帧、前移 chunk_shift 帧，产出 chunk_shift/4 个
 *     输出帧（zipformer2 下采样固定 4）；内部维护 6m+2 个状态张量
 *     （m = 各 block 层数之和），跑完原地回读，调用方无感
 *   - decode_frame 是解码循环的砖块：调用方对每个输出帧、每条假设各调一次，
 *     传入该假设历史里**最后 context_size 个 token**（不足补 -1，初始为
 *     [-1, ..., -1, 0]，与 sherpa 的 blank_hyp 一致），拿回 joiner 的原始
 *     logits（vocab_size 个，**未过 softmax** —— KWS 要概率、ASR 要 argmax，
 *     都从这里取，各取所需）
 *   - processed_lens 张量 = 本 chunk 开始时已处理的帧数（sherpa 同语义），
 *     引擎内部维护，调用方不用管
 *
 * 纯 C、只依赖 inference 的 C 接口（dtype 以模型声明为准，f32 之外会装配期报错）。
 * 不依赖 core/pipeline —— 离线工具与测试可以直接用。
 */
#ifndef AURA_DSP_ZIPFORMER_AURA_ZIPFORMER_H
#define AURA_DSP_ZIPFORMER_AURA_ZIPFORMER_H

#include <stdbool.h>
#include <stdint.h>

#include "inference/inference.h"
#include "utils/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aura_zipformer aura_zipformer_t;

typedef struct {
    const char *encoder_path; /* 三个 .mnn 路径，MODULE 形态加载 */
    const char *decoder_path;
    const char *joiner_path;
    int32_t     feature_dim;  /* 80 —— 用于装配期校验 x 与 embed_states 的形状 */
    aura_infer_group_t *group; /* NULL = 默认组（SYNC 节点用） */
} aura_zipformer_cfg_t;

aura_zipformer_t *aura_zipformer_create(const aura_zipformer_cfg_t *cfg);
void              aura_zipformer_destroy(aura_zipformer_t *z);

/* 超参（全部来自模型 metadata，装配期校验） */
int32_t aura_zipformer_T(const aura_zipformer_t *z);             /* chunk 输入帧数 */
int32_t aura_zipformer_chunk_shift(const aura_zipformer_t *z);   /* 每次前移帧数 */
int32_t aura_zipformer_output_frames(const aura_zipformer_t *z); /* 每 chunk 输出帧数 */
int32_t aura_zipformer_vocab_size(const aura_zipformer_t *z);
int32_t aura_zipformer_context_size(const aura_zipformer_t *z);
int32_t aura_zipformer_encoder_dim(const aura_zipformer_t *z); /* encoder_out 每帧维数 */

/* 状态全零 + 特征缓冲清空 + 已处理帧数归零。 */
void aura_zipformer_reset(aura_zipformer_t *z);

/* 只清状态（编码器状态全零），**保留**特征缓冲与已处理帧数 ——
 * KWS 的尾静音自动复位用这个（sherpa 的 Reset 只换 encoder init states，
 * 特征缓冲原样保留，随后照常解码当前 chunk）。 */
void aura_zipformer_reset_states(aura_zipformer_t *z);

/* 攒特征帧（每帧 feature_dim 个 float，紧密排列）。返回攒到的总帧数（负 = 错误）。
 * 缓冲溢出（调用方从不检查 ready 就狂喂）时丢最旧的一帧并告警。 */
int32_t aura_zipformer_accept(aura_zipformer_t *z, const float *features, int32_t n_frames);

int32_t aura_zipformer_num_frames(const aura_zipformer_t *z);
bool    aura_zipformer_ready(const aura_zipformer_t *z);

/* 解码一个 chunk（ready 之后调用）。返回本次产出的输出帧数（负 = 错误）。 */
int32_t aura_zipformer_decode_chunk(aura_zipformer_t *z);

/* 解码第 t 个输出帧（decode_chunk 之后，0 <= t < output_frames）。
 * ys 指向 context_size 个 int32 token；logits 写 vocab_size 个 float。 */
aura_err_t aura_zipformer_decode_frame(aura_zipformer_t *z, int32_t t,
                                       const int32_t *ys, float *logits);

#ifdef __cplusplus
}
#endif

#endif /* AURA_DSP_ZIPFORMER_AURA_ZIPFORMER_H */
