/* Aura — symbol table（tokens.txt 解析，KWS/ASR 共用）
 *
 * sherpa-mnn symbol-table 的 C 子集，只留我们需要的三件事：
 *   - 解析 kaldi 风格 tokens.txt（每行 "sym id"；只有 id 的行表示符号是空格）；
 *   - id → 符号（命中唤醒词后拼词条文本给人看）、符号 → id（keywords 文件解析）；
 *   - keywords 行解析：空格分隔的符号串，`@名字` 后缀为显示名（sherpa 约定）。
 *
 * 与 sherpa 的两处有意差异：
 *   1. **vocab_size 过滤**：tokens.txt 可能比模型实际词表长（KWS 模型 227 行、
 *      vocab_size=197，#0..#29 永远不会被输出）—— id >= vocab_size 的条目
 *      直接不收录，lookup 永远拿不到它们；
 *   2. 行解析出错是告警 + 跳过该行，不 exit —— 一个坏行不值得崩掉整条链路。
 *
 * 内存：create 时一次性分配（符号字符串池 + 两套数组），运行期零 malloc。
 * 查找是线性扫描 —— 词表 ≤ 几百条，且只在装配/命中拼文本时查，不在热路径上。
 */
#include "dsp/zipformer/symbol_table.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils/logger/logger.h"

#define TAG "dsp.sym"

/* 词表规模没有普适上限（KWS 197、ASR 6254）—— 条数按文件实计、按需分配。 */
typedef struct {
    int32_t      id;
    const char  *sym; /* 指向字符串池 */
} st_entry_t;

struct aura_symbol_table {
    st_entry_t *entries;
    int32_t     num_entries; /* 按 id 升序 */
    int32_t     cap_entries;
    char       *pool; /* 符号字符串池 */
    int32_t     pool_used;
    int32_t     pool_cap;
};

static const char *st_store(aura_symbol_table_t *st, const char *s, size_t len)
{
    if (st->pool_used + (int32_t)len + 1 > st->pool_cap) {
        return NULL;
    }
    char *dst = st->pool + st->pool_used;
    memcpy(dst, s, len);
    dst[len] = '\0';
    st->pool_used += (int32_t)len + 1;
    return dst;
}

/* 二分找 id 的下标（或应插入位置）。 */
static int32_t st_find_id(const aura_symbol_table_t *st, int32_t id)
{
    int32_t lo = 0, hi = st->num_entries - 1;
    while (lo <= hi) {
        int32_t mid = (lo + hi) / 2;
        if (st->entries[mid].id == id) {
            return mid;
        }
        if (st->entries[mid].id < id) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return lo; /* 应插入位置 */
}

aura_symbol_table_t *aura_symbol_table_create(const char *tokens_path,
                                              int32_t vocab_size)
{
    if (tokens_path == NULL || vocab_size <= 0) {
        return NULL;
    }
    FILE *fp = fopen(tokens_path, "rb");
    if (fp == NULL) {
        AURA_LOGE(TAG, "打不开 tokens.txt: %s", tokens_path);
        return NULL;
    }

    /* 词表规模未知：先扫一遍统计"有效条数 + 字符总量"，再按需分配
     * （entries 与字符串池都精确到实际规模，KWS 197 条与 ASR 6254 条共用）。 */
    int32_t total_chars = 0;
    int32_t valid_lines = 0;
    char    line[512];
    while (fgets(line, sizeof(line), fp) != NULL) {
        total_chars += (int32_t)strlen(line) + 1;
        /* 粗略预判（正式解析还会再校验）：非空行、id < vocab_size */
        char *s = line;
        while (*s != '\0' && isspace((unsigned char)*s)) s++;
        if (*s == '\0') {
            continue;
        }
        char *sp = s;
        while (*sp != '\0' && !isspace((unsigned char)*sp)) sp++;
        const char *id_part = (*sp != '\0') ? sp + 1 : s;
        while (*id_part != '\0' && isspace((unsigned char)*id_part)) id_part++;
        int32_t id = (int32_t)strtol(id_part, NULL, 10);
        if (id >= 0 && id < vocab_size) {
            valid_lines++;
        }
    }
    rewind(fp);

    aura_symbol_table_t *st = (aura_symbol_table_t *)calloc(1, sizeof(*st));
    if (st == NULL) {
        fclose(fp);
        return NULL;
    }
    st->cap_entries = valid_lines + 1;
    st->entries = (st_entry_t *)calloc((size_t)st->cap_entries, sizeof(st_entry_t));
    st->pool_cap = total_chars + 1;
    st->pool = (char *)malloc((size_t)st->pool_cap);
    if (st->entries == NULL || st->pool == NULL) {
        free(st->entries);
        free(st->pool);
        free(st);
        fclose(fp);
        return NULL;
    }

    while (fgets(line, sizeof(line), fp) != NULL) {
        /* 去首尾空白 */
        char *s = line;
        while (*s != '\0' && isspace((unsigned char)*s)) s++;
        char *end = s + strlen(s);
        while (end > s && isspace((unsigned char)end[-1])) *--end = '\0';
        if (*s == '\0') {
            continue; /* 空行 */
        }

        /* "sym id"；没有空格 = 该行就是 id，符号是空格（kaldi 约定，
         * sherpa 同语义） */
        char *sp = end;
        while (sp > s && !isspace((unsigned char)*sp)) sp--;
        int32_t     id;
        char       *sym_start = s;
        char       *sym_end   = end;
        if (sp == s) {
            id        = (int32_t)strtol(s, NULL, 10);
            sym_start = (char *)" ";
            sym_end   = sym_start + 1;
        } else {
            *sp      = '\0'; /* 符号是 s..sp-1 */
            sym_end  = sp;
            id       = (int32_t)strtol(sp + 1, NULL, 10);
        }

        if (id < 0 || id >= vocab_size) {
            /* 越界的条目（#0..#29 那类）不收录 —— 模型永远不会输出它们 */
            continue;
        }
        if (st->num_entries >= st->cap_entries) {
            AURA_LOGW(TAG, "符号表条目超预扫上限（%d），后续条目丢弃", st->cap_entries);
            break;
        }

        /* 按 id 升序插入（文件通常已有序，插入就是追加） */
        int32_t pos = st_find_id(st, id);
        if (pos < st->num_entries && st->entries[pos].id == id) {
            AURA_LOGW(TAG, "tokens.txt 有重复 id %d，跳过: %s", id, line);
            continue;
        }
        size_t sym_len = (size_t)(sym_end - sym_start);
        const char *stored = st_store(st, sym_start, sym_len);
        if (stored == NULL) {
            AURA_LOGE(TAG, "符号池超容，tokens.txt 解析中止");
            break;
        }
        memmove(&st->entries[pos + 1], &st->entries[pos],
                sizeof(st_entry_t) * (size_t)(st->num_entries - pos));
        st->entries[pos].id  = id;
        st->entries[pos].sym = stored;
        st->num_entries++;
    }

    fclose(fp);
    AURA_LOGI(TAG, "符号表: %d 个符号（vocab=%d, %s）", st->num_entries, vocab_size,
              tokens_path);
    return st;
}

void aura_symbol_table_destroy(aura_symbol_table_t *st)
{
    if (st == NULL) {
        return;
    }
    free(st->entries);
    free(st->pool);
    free(st);
}

const char *aura_symbol_table_symbol(const aura_symbol_table_t *st, int32_t id)
{
    if (st == NULL) {
        return NULL;
    }
    int32_t pos = st_find_id(st, id);
    return (pos < st->num_entries && st->entries[pos].id == id)
               ? st->entries[pos].sym
               : NULL;
}

int32_t aura_symbol_table_lookup(const aura_symbol_table_t *st, const char *sym)
{
    if (st == NULL || sym == NULL) {
        return -1;
    }
    for (int32_t i = 0; i < st->num_entries; i++) {
        if (strcmp(st->entries[i].sym, sym) == 0) {
            return st->entries[i].id;
        }
    }
    return -1;
}

int32_t aura_symbol_table_tokenize(const aura_symbol_table_t *st, const char *line,
                                   int32_t *ids, int32_t max_ids, char *name,
                                   int32_t name_cap)
{
    if (st == NULL || line == NULL || ids == NULL || max_ids <= 0) {
        return 0;
    }

    /* 名字缓冲先清空：行里没有 @ 时输出空串（调用方不该看到上一条的词名） */
    if (name != NULL && name_cap > 0) {
        name[0] = '\0';
    }

    int32_t n = 0;
    const char *p = line;
    while (*p != '\0') {
        while (*p != '\0' && isspace((unsigned char)*p)) p++;
        if (*p == '\0') {
            break;
        }

        if (*p == '@') {
            /* 显示名：@ 到行尾（去掉首尾空白/换行） */
            if (name != NULL && name_cap > 0) {
                const char *q = p + 1;
                while (*q != '\0' && isspace((unsigned char)*q)) q++;
                size_t len = strlen(q);
                while (len > 0 && isspace((unsigned char)q[len - 1])) {
                    len--; /* 行尾的 \n/\r 不算词名 */
                }
                if (len >= (size_t)name_cap) {
                    len = (size_t)name_cap - 1;
                }
                memcpy(name, q, len);
                name[len] = '\0';
            }
            break;
        }

        const char *tok_end = p;
        while (*tok_end != '\0' && !isspace((unsigned char)*tok_end) &&
               *tok_end != '@') {
            tok_end++;
        }
        char tok[128];
        size_t tok_len = (size_t)(tok_end - p);
        if (tok_len >= sizeof(tok)) {
            tok_len = sizeof(tok) - 1;
        }
        memcpy(tok, p, tok_len);
        tok[tok_len] = '\0';

        int32_t id = aura_symbol_table_lookup(st, tok);
        if (id < 0) {
            AURA_LOGW(TAG, "keywords 行含未知符号 '%s'，该词条不可用", tok);
            return -1;
        }
        if (n >= max_ids) {
            return -1;
        }
        ids[n++] = id;
        p = tok_end;
    }
    return n;
}
