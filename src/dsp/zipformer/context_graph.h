/* Aura — 关键词上下文图（AC 自动机）
 *
 * sherpa-mnn context-graph.cc 的 C 移植，语义逐条对齐（strict 模式，
 * KWS 用的就是它）：
 *   - 把一组关键词（token id 序列）建成 trie + fail 指针 + output 弧；
 *   - ForwardOneStep(state, token) 返回（分数贡献, 新状态, 命中节点）——
 *     KWS 束搜索每推一个 token 走一步，命中节点非空且满足阈值即唤醒；
 *   - 分数规则：每个词条带 per-token score（缺省 = context_score），
 *     node_score = 父 node_score + token_score；output_score 在命中节点上是
 *     node_score，经 fail 链继承后累加。
 *
 * 实现差异（有意为之）：sherpa 用 unordered_map/unique_ptr 建树，这里用**扁平
 * 数组一次性分配**（create 时算好总节点数上限 = 1 + Σ token 数，边同理），
 * 运行期零 malloc —— 与引擎的内存纪律一致；子节点查找是线性扫描（关键词 trie
 * 每个节点的分支极少，几十个词条的规模下线性比哈希快且无内存碎片）。
 *
 * 节点下标为 int32，根恒为 0（token=-1、fail=0）。
 */
#ifndef AURA_DSP_ZIPFORMER_CONTEXT_GRAPH_H
#define AURA_DSP_ZIPFORMER_CONTEXT_GRAPH_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aura_context_graph aura_context_graph_t;

/* token_ids：num_keywords 个词条，各 token_id_lens[i] 个 id。
 * context_score：词条未自带 per-token score 时用这个（sherpa 的 keywords_score）。
 * ac_threshold：词条未自带阈值时用这个。
 * scores / phrases / ac_thresholds：逐词条覆盖，可传 NULL（按缺省）。 */
aura_context_graph_t *aura_context_graph_create(
    const int32_t *const *token_ids, const int32_t *token_id_lens,
    int32_t num_keywords, float context_score, float ac_threshold,
    const float *scores, const char *const *phrases, const float *ac_thresholds);

void aura_context_graph_destroy(aura_context_graph_t *g);

/* 根节点下标（恒 0）。 */
int32_t aura_context_graph_root(const aura_context_graph_t *g);

/* 一步转移（= sherpa ForwardOneStep(strict_mode=true)）：
 *   *next_state = 转移后的节点；该 token 不在任何词条里时回到根 ——
 *   KWS 束搜索看到"回到根"就重置假设历史（从头开始匹配）；
 *   *matched_state = 本次命中/经过的接受节点下标（-1 = 无）。
 *   返回 score 贡献（加到假设的 log_prob 上）。 */
float aura_context_graph_forward(aura_context_graph_t *g, int32_t state,
                                 int32_t token, int32_t *next_state,
                                 int32_t *matched_state);

/* 节点是否处于接受态；返回接受节点下标（-1 = 否）。 */
int32_t aura_context_graph_is_matched(const aura_context_graph_t *g, int32_t state);

/* 接受节点的元信息（只对 is_matched 返回的节点有意义）。 */
int32_t     aura_context_graph_level(const aura_context_graph_t *g, int32_t state);
float       aura_context_graph_threshold(const aura_context_graph_t *g, int32_t state);
const char *aura_context_graph_phrase(const aura_context_graph_t *g, int32_t state);

/* 节点的累计分数（node_score）。sherpa 的 Finalize = -node_score，测试对拍用。 */
float aura_context_graph_node_score(const aura_context_graph_t *g, int32_t state);

#ifdef __cplusplus
}
#endif

#endif /* AURA_DSP_ZIPFORMER_CONTEXT_GRAPH_H */
