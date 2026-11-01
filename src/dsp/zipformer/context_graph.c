/* Aura — 关键词上下文图实现（AC 自动机，sherpa context-graph.cc 的 C 移植）
 *
 * 结构：trie 节点 + fail 指针 + output 弧，全存扁平数组。
 *   - nodes[]：节点元信息（token / 各级分数 / fail / output / 词条元信息），
 *     根在 [0]，token=-1，fail 指向自己；
 *   - edges[]：{token, node, next} 边数组，同一父节点的边串成兄弟链表
 *     （头插，与插入顺序无关），查找沿链表线性扫；
 *   - phrases_pool：词条显示名的字符串池（create 时复制进来，destroy 一次释放）。
 *
 * 分数与 sherpa 逐条对齐（Build / FillFailOutput / ForwardOneStep /
 * IsMatched），测试用例直接搬 context-graph-test.cc 的 TestBasic/TestCustomize。
 */
#include "dsp/zipformer/context_graph.h"

#include <stdlib.h>
#include <string.h>

#include "utils/logger/logger.h"

#define TAG "dsp.cg"

typedef struct {
    int32_t token;
    int32_t node;
    int32_t next; /* 同一父节点下的下一条边（-1 = 末尾）—— 兄弟链表 */
} cg_edge_t;

typedef struct {
    int32_t token;
    float   token_score;
    float   node_score;
    float   output_score;
    int32_t level;
    float   ac_threshold;
    bool    is_end;
    const char *phrase; /* 指向 phrases_pool 内的地址（无则为 NULL） */

    int32_t first_edge; /* 该节点的边链表头（-1 = 无子） */
    int32_t fail;       /* 节点下标 */
    int32_t output;     /* 节点下标，-1 = 无 */
} cg_node_t;

struct aura_context_graph {
    cg_node_t *nodes;
    int32_t    num_nodes;
    int32_t    cap_nodes;
    cg_edge_t *edges;
    int32_t    num_edges;
    int32_t    cap_edges;
    char      *phrases_pool;
    int32_t    pool_used;
    int32_t    pool_cap;
};

int32_t aura_context_graph_root(const aura_context_graph_t *g)
{
    (void)g;
    return 0;
}

/* 在 nodes[parent] 下找 token 的边，返回子节点下标（-1 = 无）。
 * 边是兄弟链表（插入头插），与插入顺序无关 —— 词条交错插入也不会破坏结构。 */
static int32_t cg_find_child(const aura_context_graph_t *g, int32_t parent,
                             int32_t token)
{
    for (int32_t ei = g->nodes[parent].first_edge; ei >= 0;
         ei = g->edges[ei].next) {
        if (g->edges[ei].token == token) {
            return g->edges[ei].node;
        }
    }
    return -1;
}

static cg_node_t *cg_new_node(aura_context_graph_t *g, int32_t token)
{
    if (g->num_nodes >= g->cap_nodes) {
        return NULL;
    }
    cg_node_t *n = &g->nodes[g->num_nodes++];
    memset(n, 0, sizeof(*n));
    n->token      = token;
    n->first_edge = -1;
    n->fail       = 0; /* 建树阶段先都指根，FillFailOutput 里修正 */
    n->output     = -1;
    return n;
}

static bool cg_add_edge(aura_context_graph_t *g, int32_t parent, int32_t token,
                        int32_t child)
{
    if (g->num_edges >= g->cap_edges) {
        return false;
    }
    cg_edge_t *e = &g->edges[g->num_edges++];
    e->token = token;
    e->node  = child;
    e->next  = g->nodes[parent].first_edge; /* 头插 */
    g->nodes[parent].first_edge = e - g->edges;
    return true;
}

/* 把 phrase 复制进字符串池（不去重 —— 词条数是个位数，去重纯属炫技）。 */
static const char *cg_store_phrase(aura_context_graph_t *g, const char *phrase)
{
    if (phrase == NULL || phrase[0] == '\0') {
        return NULL;
    }
    size_t len = strlen(phrase) + 1;
    if (g->pool_used + (int32_t)len > g->pool_cap) {
        return NULL;
    }
    char *dst = g->phrases_pool + g->pool_used;
    memcpy(dst, phrase, len);
    g->pool_used += (int32_t)len;
    return dst;
}

/* Build：词条逐个插入 trie；重边取 score 大者（sherpa 同语义）。 */
static void cg_build(aura_context_graph_t *g,
                     const int32_t *const *token_ids,
                     const int32_t *token_id_lens, int32_t num_keywords,
                     float context_score, float ac_threshold,
                     const float *scores, const char *const *phrases,
                     const float *ac_thresholds)
{
    for (int32_t i = 0; i < num_keywords; i++) {
        int32_t parent = 0;
        float   score  = (scores == NULL || scores[i] == 0.0f)
                             ? context_score
                             : scores[i];
        float   threshold = (ac_thresholds == NULL || ac_thresholds[i] == 0.0f)
                                ? ac_threshold
                                : ac_thresholds[i];
        const char *phrase = (phrases == NULL) ? NULL : phrases[i];

        for (int32_t j = 0; j < token_id_lens[i]; j++) {
            int32_t token = token_ids[i][j];
            bool    is_end = (j == token_id_lens[i] - 1);
            int32_t child  = cg_find_child(g, parent, token);

            if (child < 0) {
                cg_node_t *n = cg_new_node(g, token);
                if (n == NULL) {
                    AURA_LOGE(TAG, "节点超容（%d），词条 %d 截断", g->cap_nodes, i);
                    return;
                }
                child = g->num_nodes - 1;
                if (!cg_add_edge(g, parent, token, child)) {
                    AURA_LOGE(TAG, "边超容（%d）", g->cap_edges);
                    return;
                }
                n->token_score  = score;
                n->node_score   = g->nodes[parent].node_score + score;
                n->output_score = is_end ? n->node_score : 0.0f;
                n->level        = j + 1;
                n->is_end       = is_end;
                n->ac_threshold = is_end ? threshold : 0.0f;
                n->phrase       = is_end ? cg_store_phrase(g, phrase) : NULL;
            } else {
                cg_node_t *n = &g->nodes[child];
                if (score > n->token_score) {
                    n->token_score = score;
                }
                n->node_score = g->nodes[parent].node_score + n->token_score;
                /* 与 sherpa 逐条对齐：合并边的 is_end 取或，output_score 跟着
                 * 新 node_score 走（一个词条是另一个的前缀时，短词条的终态
                 * 分数会随长词条走高）；终点信息只在当前词条结尾处覆写。 */
                bool merged_end = (j == token_id_lens[i] - 1) || n->is_end;
                n->output_score = merged_end ? n->node_score : 0.0f;
                n->is_end       = merged_end;
                if (j == token_id_lens[i] - 1) {
                    n->ac_threshold = threshold;
                    n->phrase       = cg_store_phrase(g, phrase);
                }
            }
            parent = child;
        }
    }
}

/* FillFailOutput：BFS 建 fail 指针与 output 弧（sherpa 同语义）。
 * output 弧 = 沿 fail 链向上最近的一个接受态节点。 */
static void cg_fill_fail_output(aura_context_graph_t *g)
{
    int32_t *queue = (int32_t *)malloc((size_t)g->num_nodes * sizeof(int32_t));
    if (queue == NULL) {
        AURA_LOGE(TAG, "fail 队列分配失败");
        return;
    }
    int32_t head = 0, tail = 0;

    /* 根的直接孩子：fail = 根 */
    for (int32_t ei = g->nodes[0].first_edge; ei >= 0; ei = g->edges[ei].next) {
        int32_t child         = g->edges[ei].node;
        g->nodes[child].fail  = 0;
        queue[tail++]         = child;
    }

    while (head < tail) {
        int32_t cur = queue[head++];
        for (int32_t ei = g->nodes[cur].first_edge; ei >= 0;
             ei = g->edges[ei].next) {
            int32_t tok   = g->edges[ei].token;
            int32_t child = g->edges[ei].node;

            /* fail：从父的 fail 链上找第一条同 token 的边 */
            int32_t fail = g->nodes[cur].fail;
            int32_t hit  = cg_find_child(g, fail, tok);
            while (hit < 0 && fail != 0) {
                fail = g->nodes[fail].fail;
                hit  = cg_find_child(g, fail, tok);
            }
            g->nodes[child].fail = (hit >= 0) ? hit : 0;

            /* output：沿 fail 链向上最近一个接受态 */
            int32_t output = g->nodes[child].fail;
            while (output != 0 && !g->nodes[output].is_end) {
                output = g->nodes[output].fail;
            }
            if (output != 0 && g->nodes[output].is_end) {
                g->nodes[child].output = output;
                g->nodes[child].output_score += g->nodes[output].output_score;
            } else {
                g->nodes[child].output = -1;
            }

            queue[tail++] = child;
        }
    }
    free(queue);
}

aura_context_graph_t *aura_context_graph_create(
    const int32_t *const *token_ids, const int32_t *token_id_lens,
    int32_t num_keywords, float context_score, float ac_threshold,
    const float *scores, const char *const *phrases, const float *ac_thresholds)
{
    if (token_ids == NULL || token_id_lens == NULL || num_keywords <= 0) {
        return NULL;
    }

    /* 上限：节点 ≤ 1 + Σ词长，边 = 节点 - 1；短语池 ≤ Σ(词名长+1)。 */
    int32_t total_tokens = 0;
    size_t  phrase_bytes = 0;
    for (int32_t i = 0; i < num_keywords; i++) {
        if (token_id_lens[i] <= 0) {
            return NULL;
        }
        total_tokens += token_id_lens[i];
        if (phrases != NULL && phrases[i] != NULL) {
            phrase_bytes += strlen(phrases[i]) + 1;
        }
    }

    aura_context_graph_t *g =
        (aura_context_graph_t *)calloc(1, sizeof(*g));
    if (g == NULL) {
        return NULL;
    }
    g->cap_nodes = total_tokens + 1;
    g->cap_edges = total_tokens;
    g->pool_cap  = (int32_t)(phrase_bytes > 0 ? phrase_bytes : 1);
    g->nodes = (cg_node_t *)calloc((size_t)g->cap_nodes, sizeof(cg_node_t));
    g->edges = (cg_edge_t *)calloc((size_t)g->cap_edges, sizeof(cg_edge_t));
    g->phrases_pool = (char *)malloc((size_t)g->pool_cap);
    if (g->nodes == NULL || g->edges == NULL || g->phrases_pool == NULL) {
        aura_context_graph_destroy(g);
        return NULL;
    }

    /* 根：token=-1、fail=0（自己）、无词条信息 */
    g->num_nodes            = 1;
    g->nodes[0].token       = -1;
    g->nodes[0].first_edge  = -1;
    g->nodes[0].output      = -1;
    g->nodes[0].phrase      = NULL;

    cg_build(g, token_ids, token_id_lens, num_keywords, context_score,
             ac_threshold, scores, phrases, ac_thresholds);
    cg_fill_fail_output(g);
    return g;
}

void aura_context_graph_destroy(aura_context_graph_t *g)
{
    if (g == NULL) {
        return;
    }
    free(g->nodes);
    free(g->edges);
    free(g->phrases_pool);
    free(g);
}

float aura_context_graph_forward(aura_context_graph_t *g, int32_t state,
                                 int32_t token, int32_t *next_state,
                                 int32_t *matched_state)
{
    if (g == NULL || next_state == NULL || state < 0 || state >= g->num_nodes) {
        if (next_state != NULL) {
            *next_state = 0;
        }
        if (matched_state != NULL) {
            *matched_state = -1;
        }
        return 0.0f;
    }

    int32_t node  = -1;
    float   score = 0.0f;

    int32_t child = cg_find_child(g, state, token);
    if (child >= 0) {
        node  = child;
        score = g->nodes[node].token_score;
    } else {
        node = g->nodes[state].fail;
        child = cg_find_child(g, node, token);
        while (child < 0 && node != 0) {
            node  = g->nodes[node].fail;
            child = cg_find_child(g, node, token);
        }
        if (child >= 0) {
            node = child;
        }
        /* child < 0 时 node 必是根 —— 分数贡献 = 根 node_score - 当前 node_score */
        score = g->nodes[node].node_score - g->nodes[state].node_score;
    }

    *next_state = node;
    int32_t matched = g->nodes[node].is_end
                          ? node
                          : g->nodes[node].output; /* -1 或接受节点 */
    if (matched_state != NULL) {
        *matched_state = matched;
    }
    return score + g->nodes[node].output_score;
}

int32_t aura_context_graph_is_matched(const aura_context_graph_t *g, int32_t state)
{
    if (g == NULL || state < 0 || state >= g->num_nodes) {
        return -1;
    }
    if (g->nodes[state].is_end) {
        return state;
    }
    return g->nodes[state].output; /* -1 = 无 */
}

int32_t aura_context_graph_level(const aura_context_graph_t *g, int32_t state)
{
    return (g != NULL && state >= 0 && state < g->num_nodes)
               ? g->nodes[state].level
               : 0;
}

float aura_context_graph_threshold(const aura_context_graph_t *g, int32_t state)
{
    return (g != NULL && state >= 0 && state < g->num_nodes)
               ? g->nodes[state].ac_threshold
               : 0.0f;
}

const char *aura_context_graph_phrase(const aura_context_graph_t *g, int32_t state)
{
    return (g != NULL && state >= 0 && state < g->num_nodes)
               ? g->nodes[state].phrase
               : NULL;
}

float aura_context_graph_node_score(const aura_context_graph_t *g, int32_t state)
{
    return (g != NULL && state >= 0 && state < g->num_nodes)
               ? g->nodes[state].node_score
               : 0.0f;
}
