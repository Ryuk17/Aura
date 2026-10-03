#include "core/algorithm.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/config/config.h" /* aura_chain_from_config（见文件末尾） */
#include "utils/logger/logger.h"

#define TAG "algo"

/* ========================= 参数读写 ========================= */

static aura_algo_param_t *param_slot(aura_algo_params_t *p, const char *name, uint32_t *out_idx)
{
    for (uint32_t i = 0; i < p->extra_count; i++) {
        if (p->extra[i].name != NULL && strcmp(p->extra[i].name, name) == 0) {
            if (out_idx != NULL) {
                *out_idx = i;
            }
            return &p->extra[i];
        }
    }
    if (p->extra_count >= AURA_ALGO_MAX_EXTRA_PARAMS) {
        AURA_LOGE(TAG, "param slots exhausted (%d), key '%s' dropped", AURA_ALGO_MAX_EXTRA_PARAMS,
                  name);
        return NULL;
    }
    aura_algo_param_t *slot = &p->extra[p->extra_count];
    memset(slot, 0, sizeof(*slot));
    slot->name = name;
    if (out_idx != NULL) {
        *out_idx = p->extra_count;
    }
    p->extra_count++;
    return slot;
}

aura_err_t aura_algo_params_set_i32(aura_algo_params_t *p, const char *name, int32_t v)
{
    if (p == NULL || name == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    aura_algo_param_t *s = param_slot(p, name, NULL);
    if (s == NULL) {
        return AURA_ERR_FULL;
    }
    s->type = AURA_ALGO_PARAM_I32;
    s->v.i  = v;
    return AURA_OK;
}

aura_err_t aura_algo_params_set_f32(aura_algo_params_t *p, const char *name, float v)
{
    if (p == NULL || name == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    aura_algo_param_t *s = param_slot(p, name, NULL);
    if (s == NULL) {
        return AURA_ERR_FULL;
    }
    s->type = AURA_ALGO_PARAM_F32;
    s->v.f  = v;
    return AURA_OK;
}

aura_err_t aura_algo_params_set_bool(aura_algo_params_t *p, const char *name, bool v)
{
    if (p == NULL || name == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    aura_algo_param_t *s = param_slot(p, name, NULL);
    if (s == NULL) {
        return AURA_ERR_FULL;
    }
    s->type = AURA_ALGO_PARAM_BOOL;
    s->v.b  = v;
    return AURA_OK;
}

aura_err_t aura_algo_params_set_str(aura_algo_params_t *p, const char *name, const char *v)
{
    if (p == NULL || name == NULL || v == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    aura_algo_param_t *s = param_slot(p, name, NULL);
    if (s == NULL) {
        return AURA_ERR_FULL;
    }
    s->type = AURA_ALGO_PARAM_STR;
    s->v.s  = v;
    return AURA_OK;
}

aura_err_t aura_algo_params_set_f32_array(aura_algo_params_t *p, const char *name, const float *v,
                                          uint32_t n)
{
    if (p == NULL || name == NULL || v == NULL || n == 0) {
        return AURA_ERR_INVALID_ARG;
    }
    aura_algo_param_t *s = param_slot(p, name, NULL);
    if (s == NULL) {
        return AURA_ERR_FULL;
    }
    s->type    = AURA_ALGO_PARAM_F32_ARR;
    s->v.fa.v  = v;
    s->v.fa.n  = n;
    return AURA_OK;
}

const aura_algo_param_t *aura_algo_params_get(const aura_algo_params_t *p, const char *name)
{
    if (p == NULL || name == NULL) {
        return NULL;
    }
    for (uint32_t i = 0; i < p->extra_count; i++) {
        if (p->extra[i].name != NULL && strcmp(p->extra[i].name, name) == 0) {
            return &p->extra[i];
        }
    }
    return NULL;
}

int32_t aura_algo_params_i32(const aura_algo_params_t *p, const char *name, int32_t def)
{
    const aura_algo_param_t *v = aura_algo_params_get(p, name);
    return (v != NULL && v->type == AURA_ALGO_PARAM_I32) ? v->v.i : def;
}

float aura_algo_params_f32(const aura_algo_params_t *p, const char *name, float def)
{
    const aura_algo_param_t *v = aura_algo_params_get(p, name);
    return (v != NULL && v->type == AURA_ALGO_PARAM_F32) ? v->v.f : def;
}

bool aura_algo_params_bool(const aura_algo_params_t *p, const char *name, bool def)
{
    const aura_algo_param_t *v = aura_algo_params_get(p, name);
    return (v != NULL && v->type == AURA_ALGO_PARAM_BOOL) ? v->v.b : def;
}

const char *aura_algo_params_str(const aura_algo_params_t *p, const char *name, const char *def)
{
    const aura_algo_param_t *v = aura_algo_params_get(p, name);
    return (v != NULL && v->type == AURA_ALGO_PARAM_STR) ? v->v.s : def;
}

/* ========================= 注册表 ========================= */

/* 注册表规模：算法总数（DSP 12 + NN 6 + 将来扩展）留一倍余量。
 * 静态数组而非动态表：装配期一次性写满，之后只读，没有并发问题。 */
#define AURA_ALGO_REGISTRY_MAX 48

static const aura_algo_desc_t *g_registry[AURA_ALGO_REGISTRY_MAX];
static uint32_t                g_registry_count;

aura_err_t aura_algo_register(const aura_algo_desc_t *desc)
{
    if (desc == NULL || desc->name == NULL || desc->create == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    if (g_registry_count >= AURA_ALGO_REGISTRY_MAX) {
        return AURA_ERR_FULL;
    }
    if ((desc->io.changes_frame_rate || desc->io.changes_channels) && desc->shape_out == NULL) {
        /* 声明了形状变化却不说变成什么：下游形状无从传播，属于描述表错误。 */
        AURA_LOGE(TAG, "algo %s: changes frame rate/channels without shape_out", desc->name);
        return AURA_ERR_INVALID_ARG;
    }
    if (aura_algo_find(desc->name) != NULL) {
        AURA_LOGE(TAG, "algo '%s' already registered", desc->name);
        return AURA_ERR_EXIST;
    }
    g_registry[g_registry_count++] = desc;
    return AURA_OK;
}

aura_err_t aura_algo_register_all(const aura_algo_desc_t *descs, uint32_t count)
{
    if (descs == NULL && count > 0) {
        return AURA_ERR_INVALID_ARG;
    }
    for (uint32_t i = 0; i < count; i++) {
        aura_err_t rc = aura_algo_register(&descs[i]);
        if (rc != AURA_OK) {
            return rc;
        }
    }
    return AURA_OK;
}

const aura_algo_desc_t *aura_algo_find(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (uint32_t i = 0; i < g_registry_count; i++) {
        if (strcmp(g_registry[i]->name, name) == 0) {
            return g_registry[i];
        }
    }
    return NULL;
}

const aura_algo_desc_t *aura_algo_at(uint32_t idx)
{
    return (idx < g_registry_count) ? g_registry[idx] : NULL;
}

uint32_t aura_algo_count(void)
{
    return g_registry_count;
}

void aura_algo_reset(void)
{
    memset(g_registry, 0, sizeof(g_registry));
    g_registry_count = 0;
}

const char *aura_algo_kind_name(aura_algo_kind_t kind)
{
    switch (kind) {
    case AURA_ALGO_KIND_DSP:      return "dsp";
    case AURA_ALGO_KIND_NN_SYNC:  return "nn-sync";
    case AURA_ALGO_KIND_NN_ASYNC: return "nn-async";
    default:                      return "unknown";
    }
}

/* ========================= 链描述 ========================= */

void aura_chain_init(aura_chain_t *chain)
{
    if (chain == NULL) {
        return;
    }
    memset(chain, 0, sizeof(*chain));
}

aura_err_t aura_chain_add(aura_chain_t *chain, const char *algo, const char *name)
{
    if (chain == NULL || algo == NULL || algo[0] == '\0') {
        return AURA_ERR_INVALID_ARG;
    }
    if (chain->count >= AURA_CHAIN_MAX_LINKS) {
        AURA_LOGE(TAG, "chain exceeds %d links at '%s'", AURA_CHAIN_MAX_LINKS, algo);
        return AURA_ERR_FULL;
    }
    if (strlen(algo) >= AURA_CHAIN_NAME_MAX) {
        return AURA_ERR_INVALID_ARG;
    }
    aura_chain_link_t *l = &chain->links[chain->count];
    memset(l, 0, sizeof(*l));
    snprintf(l->algo, sizeof(l->algo), "%s", algo);
    snprintf(l->name, sizeof(l->name), "%s", (name != NULL && name[0] != '\0') ? name : algo);
    chain->count++;
    return AURA_OK;
}

aura_algo_params_t *aura_chain_params(aura_chain_t *chain, uint32_t idx)
{
    if (chain == NULL || idx >= chain->count) {
        return NULL;
    }
    return &chain->links[idx].params;
}

/* 解析器的辅助：跳过空白。 */
static const char *skip_ws(const char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') {
        s++;
    }
    return s;
}

/* 读一个标识符（算法名）：字母/数字/下划线，其余一律不算。
 * 返回标识符后的位置，并把内容写进 buf。 */
static const char *read_ident(const char *s, char *buf, size_t cap, bool *ok)
{
    size_t n = 0;
    *ok      = false;
    s        = skip_ws(s);
    while ((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') ||
           *s == '_' || *s == '-' || *s == '.') {
        if (n + 1 >= cap) {
            return s; /* 超长：ok 保持 false，由调用方报语法错误 */
        }
        buf[n++] = *s++;
    }
    if (n == 0) {
        return s;
    }
    buf[n] = '\0';
    *ok    = true;
    return s;
}

aura_err_t aura_chain_parse(const char *syntax, aura_chain_t *chain)
{
    if (syntax == NULL || chain == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    aura_chain_init(chain);

    const char *s = syntax;
    for (;;) {
        char name[AURA_CHAIN_NAME_MAX];
        bool ok = false;
        s       = read_ident(s, name, sizeof(name), &ok);
        if (!ok) {
            return AURA_ERR_INVALID_ARG; /* 空元素（如 "a,,b"）或链首非法字符 */
        }

        if (strcmp(name, "tee") == 0) {
            s = skip_ws(s);
            if (*s != '(') {
                AURA_LOGE(TAG, "chain: 'tee' expects '('");
                return AURA_ERR_INVALID_ARG;
            }
            s++;
            /* tee(...) 内部至少一个成员，成员之间逗号分隔。 */
            uint32_t members = 0;
            for (;;) {
                bool mok = false;
                s        = read_ident(s, name, sizeof(name), &mok);
                if (!mok) {
                    AURA_LOGE(TAG, "chain: tee() member missing");
                    return AURA_ERR_INVALID_ARG;
                }
                if (aura_chain_add(chain, name, NULL) != AURA_OK) {
                    return AURA_ERR_FULL;
                }
                chain->links[chain->count - 1].observer_hint = true;
                members++;
                s = skip_ws(s);
                if (*s == ',') {
                    s++;
                    continue;
                }
                break;
            }
            if (members == 0) {
                return AURA_ERR_INVALID_ARG;
            }
            s = skip_ws(s);
            if (*s != ')') {
                AURA_LOGE(TAG, "chain: tee() not closed");
                return AURA_ERR_INVALID_ARG;
            }
            s++;
        } else {
            /* 链首元素不可能是观察者；其余元素先按普通链元素记下，
             * 由 chain_build 依据描述表的 io 决定它是主线还是旁路。 */
            if (aura_chain_add(chain, name, NULL) != AURA_OK) {
                return AURA_ERR_FULL;
            }
        }

        s = skip_ws(s);
        if (*s == '\0') {
            break;
        }
        if (*s != ',') {
            return AURA_ERR_INVALID_ARG;
        }
        s++;
    }
    return (chain->count > 0) ? AURA_OK : AURA_ERR_INVALID_ARG;
}

/* ========================= 链组装 ========================= */

/* 校验 extra 里的键都在 param_specs 里声明过；数值越界只告警不拦截
 * （有些参数区间是经验值，硬拦会挡掉实验）。 */
static aura_err_t validate_params(const aura_algo_desc_t *desc, const aura_algo_params_t *params)
{
    for (uint32_t i = 0; i < params->extra_count; i++) {
        const aura_algo_param_t *ep = &params->extra[i];
        if (ep->name == NULL) {
            continue;
        }
        if (desc->param_specs == NULL) {
            continue; /* 未声明规格 = 不校验 */
        }
        const aura_algo_param_spec_t *spec = NULL;
        for (uint32_t j = 0; j < desc->param_spec_count; j++) {
            if (strcmp(desc->param_specs[j].name, ep->name) == 0) {
                spec = &desc->param_specs[j];
                break;
            }
        }
        if (spec == NULL) {
            AURA_LOGE(TAG, "algo %s: unknown param '%s'", desc->name, ep->name);
            return AURA_ERR_INVALID_ARG;
        }
        if (spec->type != ep->type) {
            AURA_LOGE(TAG, "algo %s: param '%s' type mismatch", desc->name, ep->name);
            return AURA_ERR_INVALID_ARG;
        }
        if (spec->f_max > spec->f_min) {
            float v = (ep->type == AURA_ALGO_PARAM_I32) ? (float)ep->v.i : ep->v.f;
            if (ep->type == AURA_ALGO_PARAM_I32 || ep->type == AURA_ALGO_PARAM_F32) {
                if (v < spec->f_min || v > spec->f_max) {
                    AURA_LOGW(TAG, "algo %s: param '%s'=%g out of [%g,%g]", desc->name, ep->name,
                              (double)v, (double)spec->f_min, (double)spec->f_max);
                }
            }
        }
    }
    return AURA_OK;
}

aura_err_t aura_chain_build(aura_pipeline_t *p, const aura_chain_t *chain, aura_node_t **out_nodes,
                            uint32_t *out_count)
{
    if (p == NULL || chain == NULL || chain->count == 0) {
        return AURA_ERR_INVALID_ARG;
    }
    if (chain->count > AURA_CHAIN_MAX_LINKS) {
        return AURA_ERR_INVALID_ARG;
    }
    if (out_nodes != NULL && out_count == NULL) {
        return AURA_ERR_INVALID_ARG;
    }

    const aura_pipeline_cfg_t *pcfg = aura_pipeline_config(p);
    if (pcfg == NULL) {
        return AURA_ERR_STATE;
    }

    /* 链首形状：链描述优先，缺省取 pipeline 配置。 */
    uint32_t shape_rate = (chain->sample_rate != 0) ? chain->sample_rate : pcfg->sample_rate;
    uint32_t shape_ms   = (chain->frame_ms != 0) ? chain->frame_ms : pcfg->frame_ms;
    uint32_t shape_ch   = (chain->channels != 0) ? chain->channels : pcfg->channels;

    uint32_t built        = 0;
    /* 本地的已建节点表：无论调用方是否要句柄，失败回滚都得能释放。
     * 只在成功且 out_nodes != NULL 时拷出去。 */
    aura_node_t *local[AURA_CHAIN_MAX_LINKS];
    aura_node_t *prev_audio    = NULL; /* 最近一个产出音频的主链节点 */
    aura_node_t *prev_text     = NULL; /* 最近一个产出文本的节点 */
    uint32_t     rate_changers = 0;
    aura_err_t   rc            = AURA_OK;

    for (uint32_t i = 0; i < chain->count; i++) {
        const aura_chain_link_t *link = &chain->links[i];
        const aura_algo_desc_t  *desc = aura_algo_find(link->algo);
        if (desc == NULL) {
            AURA_LOGE(TAG, "chain[%u]: algo '%s' not registered", i, link->algo);
            rc = AURA_ERR_NOT_FOUND;
            goto fail;
        }

        /* 生效形状写进工厂入参（链上传播的结果，不是全局配置）。 */
        aura_algo_params_t params = link->params;
        params.sample_rate        = shape_rate;
        params.frame_ms           = shape_ms;
        params.in_channels        = shape_ch;
        params.instance_name      = link->name;
        params.provider           = desc; /* 适配器一族共用一个工厂，靠它取回描述表 */
        if (desc->kind == AURA_ALGO_KIND_DSP && shape_ms != 10) {
            /* TrickRoom 全族硬约束：帧长固定 rate/100（10ms），不支持其它帧长。
             * 不在这里拦，就会退化成运行期"每帧样本数对不上"的静默错误。 */
            AURA_LOGE(TAG, "chain[%u] %s: dsp needs frame_ms==10, got %u", i, link->algo,
                      shape_ms);
            rc = AURA_ERR_INVALID_ARG;
            goto fail;
        }
        rc = validate_params(desc, &params);
        if (rc != AURA_OK) {
            goto fail;
        }

        /* 接线前置校验：观察者必须有主链前驱可挂。 */
        if (link->observer_hint && desc->io.produces_audio) {
            AURA_LOGE(TAG, "chain[%u] %s: listed in tee() but produces audio", i, link->algo);
            rc = AURA_ERR_INVALID_ARG;
            goto fail;
        }
        if (desc->io.consumes_audio && !desc->io.produces_audio && prev_audio == NULL) {
            AURA_LOGE(TAG, "chain[%u] %s: audio consumer with no upstream producer", i,
                      link->algo);
            rc = AURA_ERR_STATE;
            goto fail;
        }
        if (desc->io.changes_frame_rate) {
            if (++rate_changers > 1) {
                AURA_LOGE(TAG, "chain[%u] %s: more than one changes_frame_rate node", i,
                          link->algo);
                rc = AURA_ERR_INVALID_ARG;
                goto fail;
            }
        }

        aura_err_t    create_err = AURA_OK;
        aura_node_t  *node       = desc->create(&params, &create_err);
        if (node == NULL) {
            AURA_LOGE(TAG, "chain[%u] %s: create failed (%s)", i, link->algo,
                      aura_strerror(create_err));
            rc = (create_err != AURA_OK) ? create_err : AURA_ERR_FAIL;
            goto fail;
        }
        /* 节点名由工厂从 params.instance_name 自取（见 aura_algo_params_t 注释）：
         * 这里不给 node->name 赋值，那会存下指向链描述的悬垂指针。 */

        rc = aura_pipeline_add(p, node);
        if (rc != AURA_OK) {
            AURA_LOGE(TAG, "chain[%u] %s: pipeline_add failed (%s)", i, link->algo,
                      aura_strerror(rc));
            free(node);
            goto fail;
        }
        local[built++] = node;

        /* 接线：主线顺序连接；纯消费者（观察者/终点）挂到最近的主链生产者上作为扇出。 */
        if (desc->io.produces_audio) {
            if (prev_audio != NULL) {
                rc = aura_pipeline_link(p, prev_audio, node);
                if (rc != AURA_OK) {
                    goto fail;
                }
            }
            prev_audio = node;
            if (desc->io.changes_frame_rate || desc->io.changes_channels) {
                rc = desc->shape_out(&params, &shape_rate, &shape_ch);
                if (rc != AURA_OK) {
                    AURA_LOGE(TAG, "chain[%u] %s: shape_out failed", i, link->algo);
                    goto fail;
                }
                AURA_LOGD(TAG, "chain[%u] %s: shape -> %u Hz / %u ch", i, link->algo, shape_rate,
                          shape_ch);
            }
        } else if (desc->io.consumes_audio) {
            rc = aura_pipeline_link(p, prev_audio, node);
            if (rc != AURA_OK) {
                goto fail;
            }
        }

        if (desc->io.produces_text) {
            if (prev_text != NULL) {
                rc = aura_pipeline_link_text(p, prev_text, node);
                if (rc != AURA_OK) {
                    goto fail;
                }
            }
            prev_text = node;
        } else if (desc->io.consumes_text && prev_text != NULL) {
            rc = aura_pipeline_link_text(p, prev_text, node);
            if (rc != AURA_OK) {
                goto fail;
            }
        }
    }

    if (out_nodes != NULL) {
        for (uint32_t k = 0; k < built; k++) {
            out_nodes[k] = local[k];
        }
    }
    if (out_count != NULL) {
        *out_count = built;
    }
    AURA_LOGI(TAG, "chain built: %u nodes, in=%u Hz/%ums/%uch, out=%u Hz/%uch", built,
              (chain->sample_rate != 0) ? chain->sample_rate : pcfg->sample_rate,
              (chain->frame_ms != 0) ? chain->frame_ms : pcfg->frame_ms,
              (chain->channels != 0) ? chain->channels : pcfg->channels, shape_rate, shape_ch);
    return AURA_OK;

fail:
    /* 失败即回滚：先摘出 pipeline（断开连线、销毁输入队列），再释放节点。
     * 否则失败的 pipeline 里留着一堆半初始化节点，后续 start/destroy 会踩到它们，
     * 而调用方手里没有任何句柄可以回收。 */
    for (uint32_t k = 0; k < built; k++) {
        aura_pipeline_remove(p, local[k]);
        free(local[k]);
    }
    if (out_count != NULL) {
        *out_count = 0;
    }
    return rc;
}

void aura_chain_destroy_nodes(aura_node_t **nodes, uint32_t count)
{
    if (nodes == NULL) {
        return;
    }
    for (uint32_t i = 0; i < count; i++) {
        /* 约定：工厂一次分配、aura_node_t 在首位 → 基址即节点指针。 */
        free(nodes[i]);
        nodes[i] = NULL;
    }
}

/* ========================= 由配置组装链 ========================= */

static bool text_is_true(const char *s)
{
    return strcmp(s, "1") == 0 || strcmp(s, "true") == 0 || strcmp(s, "on") == 0 ||
           strcmp(s, "yes") == 0;
}

/* 按 spec 声明的类型解析一个文本值。整段必须被吃掉（"3abc" 不算 3）——
 * 配置里的错别字应该报错，而不是取个前缀就当读懂了。 */
static aura_err_t param_from_text(aura_algo_params_t *params, const char *name, const char *text,
                                  aura_algo_param_type_t type)
{
    char *end = NULL;
    switch (type) {
    case AURA_ALGO_PARAM_I32: {
        long v = strtol(text, &end, 0);
        if (end == text || *end != '\0') {
            return AURA_ERR_INVALID_ARG;
        }
        return aura_algo_params_set_i32(params, name, (int32_t)v);
    }
    case AURA_ALGO_PARAM_F32: {
        float v = strtof(text, &end);
        if (end == text || *end != '\0') {
            return AURA_ERR_INVALID_ARG;
        }
        return aura_algo_params_set_f32(params, name, v);
    }
    case AURA_ALGO_PARAM_BOOL:
        if (text_is_true(text)) {
            return aura_algo_params_set_bool(params, name, true);
        }
        if (strcmp(text, "0") == 0 || strcmp(text, "false") == 0 || strcmp(text, "off") == 0 ||
            strcmp(text, "no") == 0) {
            return aura_algo_params_set_bool(params, name, false);
        }
        return AURA_ERR_INVALID_ARG;
    case AURA_ALGO_PARAM_STR:
        /* 不做拷贝：指向 cfg 内部缓冲（见头文件的生命周期约定）。 */
        return aura_algo_params_set_str(params, name, text);
    case AURA_ALGO_PARAM_F32_ARR:
    default:
        /* 一行 key=value 表达不了数组。要支持得先定义语法（`k = 1,2,3`？），
         * 在那之前明确报不支持，别让用户以为写进去了。 */
        AURA_LOGE(TAG, "param '%s': array params are not supported by chain_param_*", name);
        return AURA_ERR_UNSUPPORTED;
    }
}

aura_err_t aura_chain_from_config(const struct aura_config *cfg, aura_chain_t *out)
{
    if (cfg == NULL || out == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    aura_chain_init(out);

    if (cfg->chain[0] == '\0') {
        if (cfg->chain_param_count > 0) {
            /* 只给了参数没给链：多半是把 chain 这个键敲错了（chian / nodes ...）。
             * 静默忽略会让"改了配置怎么没生效"再犯一次。 */
            AURA_LOGE(TAG, "%u chain_param_* key(s) but no 'chain' — check the key spelling",
                      cfg->chain_param_count);
            return AURA_ERR_INVALID_ARG;
        }
        return AURA_OK; /* 空链不是错误：就是"这份构建手工装节点" */
    }

    aura_err_t rc = aura_chain_parse(cfg->chain, out);
    if (rc != AURA_OK) {
        AURA_LOGE(TAG, "chain syntax error: '%s'", cfg->chain);
        return rc;
    }
    out->sample_rate = cfg->sample_rate;
    out->frame_ms    = cfg->frame_ms;
    out->channels    = cfg->channels;

    for (uint32_t i = 0; i < cfg->chain_param_count; i++) {
        const char *kv  = cfg->chain_params[i].key; /* "<algo>.<param>" */
        const char *val = cfg->chain_params[i].val;
        const char *dot = strchr(kv, '.');
        if (dot == NULL || dot == kv || dot[1] == '\0') {
            AURA_LOGE(TAG, "chain_param '%s': expected '<algo>.<param>'", kv);
            return AURA_ERR_INVALID_ARG;
        }
        size_t algo_len = (size_t)(dot - kv);
        if (algo_len >= AURA_CHAIN_NAME_MAX) {
            AURA_LOGE(TAG, "chain_param '%s': algorithm name too long", kv);
            return AURA_ERR_INVALID_ARG;
        }
        char algo[AURA_CHAIN_NAME_MAX];
        memcpy(algo, kv, algo_len);
        algo[algo_len] = '\0';
        const char *pname = dot + 1;

        const aura_algo_desc_t *desc = aura_algo_find(algo);
        if (desc == NULL) {
            AURA_LOGE(TAG, "chain_param '%s': algo '%s' not registered", kv, algo);
            return AURA_ERR_NOT_FOUND;
        }
        const aura_algo_param_spec_t *spec = NULL;
        for (uint32_t s = 0; s < desc->param_spec_count; s++) {
            if (desc->param_specs[s].name != NULL && strcmp(desc->param_specs[s].name, pname) == 0) {
                spec = &desc->param_specs[s];
                break;
            }
        }
        if (spec == NULL) {
            /* 拼错键名在这里就死掉 —— 这是本函数存在的主要理由。
             * "一个参数都没声明" 和 "这个名字不在声明里" 分开报：前者是描述表
             * 还没写（Phase 2 里很常见），后者才是用户拼错。 */
            if (desc->param_spec_count == 0) {
                AURA_LOGE(TAG, "chain_param '%s': algo '%s' declares no params", kv, algo);
            } else {
                AURA_LOGE(TAG, "chain_param '%s': algo '%s' has no param '%s'", kv, algo, pname);
            }
            return AURA_ERR_INVALID_ARG;
        }

        /* 同一个算法在链上出现多次时，参数应用到**每一处** —— 链语法还没有
         * 实例命名，此刻也没有别的合理解释。 */
        uint32_t matched = 0;
        for (uint32_t k = 0; k < out->count; k++) {
            if (strcmp(out->links[k].algo, algo) != 0) {
                continue;
            }
            rc = param_from_text(&out->links[k].params, pname, val, spec->type);
            if (rc != AURA_OK) {
                AURA_LOGE(TAG, "chain_param '%s': bad value '%s' for %s param (type %d)", kv, val,
                          pname, (int)spec->type);
                return rc;
            }
            matched++;
        }
        if (matched == 0) {
            AURA_LOGE(TAG, "chain_param '%s': algo '%s' is not in chain '%s'", kv, algo, cfg->chain);
            return AURA_ERR_NOT_FOUND;
        }
        AURA_LOGD(TAG, "chain_param %s = %s -> %u node(s)", kv, val, matched);
    }
    return AURA_OK;
}
