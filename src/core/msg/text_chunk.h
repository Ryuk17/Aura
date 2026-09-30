/* Aura — 文本 chunk（pipeline 的第二种流）
 *
 * 用途：ASR 中间/最终结果 → Agent → LLM token 流 → TTS，全链路按 chunk 流动，
 * 不做整段拼接（否则首包延迟不可控，todo.md 第 6 节 < 1s 无法达标）。
 *
 * 文本本身内联在结构里（SHORT 短文本），避免跨队列传递裸指针带来的生命周期问题；
 * 需要长文本的节点（如 TTS）应自行拷贝进自己的缓冲。
 */
#ifndef AURA_CORE_MSG_TEXT_CHUNK_H
#define AURA_CORE_MSG_TEXT_CHUNK_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef AURA_TEXT_CHUNK_MAX_BYTES
#define AURA_TEXT_CHUNK_MAX_BYTES 1024
#endif

typedef enum {
    AURA_TEXT_PARTIAL = 0, /* 中间结果，可被打断/覆盖 */
    AURA_TEXT_FINAL   = 1, /* 最终结果 */
} aura_text_flag_t;

typedef struct aura_text_chunk {
    uint64_t          pts_us;   /* 对应音频的起点时戳（下游做时序对齐用） */
    uint32_t          seq;      /* 同一 utterance 内的递增序号 */
    uint32_t          len;      /* 有效字节数（不含结尾 '\0'） */
    aura_text_flag_t  flag;
    uint32_t          source;   /* 产生者标识（ASR/LLM/Agent…），见 aura_text_source_t */
    char              text[AURA_TEXT_CHUNK_MAX_BYTES];
} aura_text_chunk_t;

typedef enum {
    AURA_TEXT_SRC_UNKNOWN = 0,
    AURA_TEXT_SRC_ASR,
    AURA_TEXT_SRC_AGENT,
    AURA_TEXT_SRC_LLM,
    AURA_TEXT_SRC_TTS,
    AURA_TEXT_SRC_APP,
} aura_text_source_t;

#ifdef __cplusplus
}
#endif

#endif /* AURA_CORE_MSG_TEXT_CHUNK_H */
