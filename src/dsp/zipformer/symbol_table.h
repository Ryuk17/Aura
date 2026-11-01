/* Aura — symbol table（tokens.txt 解析，KWS/ASR 共用）
 *
 * 只保留 sherpa-mnn symbol-table 里我们需要的子集：id ↔ 符号的双向映射，
 * 加 keywords 行的解析（唤醒词文件里每行是空格分隔的符号串，`@名字` 后缀
 * 为显示名）。BPE 解码等其余能力等 ASR 需要时再补。
 *
 * tokens.txt 格式（kaldi 风格）：每行 "sym id"；只有 id 的行表示符号是空格。
 * id >= vocab_size 的条目不收录 —— 模型永远不会输出它们（见 symbol_table.c）。
 */
#ifndef AURA_DSP_ZIPFORMER_SYMBOL_TABLE_H
#define AURA_DSP_ZIPFORMER_SYMBOL_TABLE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aura_symbol_table aura_symbol_table_t;

/* 解析 tokens.txt。vocab_size = joiner 输出维（metadata 的 vocab_size）。
 * 失败返回 NULL（文件打不开/内存不足）。 */
aura_symbol_table_t *aura_symbol_table_create(const char *tokens_path,
                                              int32_t vocab_size);
void                 aura_symbol_table_destroy(aura_symbol_table_t *st);

/* id → 符号；id 不存在（或超出 vocab）返回 NULL。 */
const char *aura_symbol_table_symbol(const aura_symbol_table_t *st, int32_t id);

/* 符号 → id；不存在返回 -1。 */
int32_t aura_symbol_table_lookup(const aura_symbol_table_t *st, const char *sym);

/* 解析一行 keywords（空格分隔的符号串，`@名字` 后缀可选）：
 * ids 写 token id 序列（最多 max_ids 个）；name 可传 NULL，否则写 `@` 后的
 * 显示名（无 `@` 时写空串）。
 * 返回 token 个数；0 = 空行；-1 = 含未知符号或超长（该词条不可用）。 */
int32_t aura_symbol_table_tokenize(const aura_symbol_table_t *st, const char *line,
                                   int32_t *ids, int32_t max_ids, char *name,
                                   int32_t name_cap);

#ifdef __cplusplus
}
#endif

#endif /* AURA_DSP_ZIPFORMER_SYMBOL_TABLE_H */
