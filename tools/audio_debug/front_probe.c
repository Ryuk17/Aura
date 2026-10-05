/* Aura — 语音前端探针（实现，见 front_probe.h） */
#include "front_probe.h"

#include <stdio.h>
#include <string.h>

#include "wavtap.h"

aura_err_t aura_probe_chain(const aura_chain_t *src, aura_chain_t *dst, aura_probe_tap_t *taps,
                            uint32_t *tap_count)
{
    if (src == NULL || dst == NULL || taps == NULL || tap_count == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    aura_chain_init(dst);
    dst->sample_rate = src->sample_rate;
    dst->frame_ms    = src->frame_ms;
    dst->channels    = src->channels;

    uint32_t nt = 0;
    for (uint32_t i = 0; i < src->count; i++) {
        const aura_chain_link_t *l = &src->links[i];
        const aura_algo_desc_t  *d = aura_algo_find(l->algo);
        if (d == NULL) {
            fprintf(stderr, "探针链：算法 %s 未注册\n", l->algo);
            return AURA_ERR_NOT_FOUND;
        }
        if (dst->count >= AURA_CHAIN_MAX_LINKS) {
            fprintf(stderr, "探针链超出 %u 个链元素上限（原链 %u 个）\n", AURA_CHAIN_MAX_LINKS,
                    src->count);
            return AURA_ERR_FULL;
        }
        /* 链元素整份拷过去：params 是值语义（extra 数组；STR 参数指向 cfg 内部缓冲，
         * 调用方保证 cfg 活过 chain_build），拷过来即等价。 */
        dst->links[dst->count++] = *l;

        if (!d->io.produces_audio) {
            continue; /* 观察者/纯消费者节点后面不用再挂探针 */
        }
        char tap_name[AURA_CHAIN_NAME_MAX];
        snprintf(tap_name, sizeof(tap_name), "%s_out", l->name);
        aura_err_t rc = aura_chain_add(dst, "wavtap", tap_name);
        if (rc != AURA_OK) {
            return rc;
        }
        dst->links[dst->count - 1].observer_hint = true;

        if (nt < AURA_PROBE_MAX_TAPS) {
            aura_probe_tap_t *t = &taps[nt++];
            memset(t, 0, sizeof(*t));
            snprintf(t->tap, sizeof(t->tap), "%s", tap_name);
            snprintf(t->node, sizeof(t->node), "%s", l->name);
            snprintf(t->algo, sizeof(t->algo), "%s", l->algo);
            t->declared_out_rate = d->io.changes_frame_rate
                                       ? (uint32_t)aura_algo_params_i32(&l->params,
                                                                        "out_sample_rate", 0)
                                       : 0u;
            t->changes_channels  = d->io.changes_channels;
        }
    }

    /* 链尾终点：名字固定 out，脚本与测试直接找 out.wav / 找名叫 out 的节点。 */
    aura_err_t rc = aura_chain_add(dst, "wavtap", "out");
    if (rc != AURA_OK) {
        return rc;
    }
    dst->links[dst->count - 1].observer_hint = true;

    *tap_count = nt;
    return AURA_OK;
}

aura_node_t *aura_probe_find(aura_node_t **nodes, uint32_t count, const char *name)
{
    if (nodes == NULL || name == NULL) {
        return NULL;
    }
    for (uint32_t i = 0; i < count; i++) {
        if (nodes[i]->name != NULL && strcmp(nodes[i]->name, name) == 0) {
            return nodes[i];
        }
    }
    return NULL;
}

aura_err_t aura_probe_feed(aura_pipeline_t *p, const aura_probe_input_t *in)
{
    if (p == NULL || in == NULL || in->mic == NULL || in->frame_count == 0) {
        return AURA_ERR_INVALID_ARG;
    }

    /* 参考与近端**各喂各的**，不做任何手工偏移：配对由 pipeline 按 pts 完成
     * （pipeline_pair_ref）。两边 pts 都传 0 = 让 pipeline 按帧长自动推进，
     * 各自从 0 起步、同帧长，天然一一对应。
     *
     * 这里曾经手工让参考"领先一帧"，那是在没有 pts 配对的年代，拿调度上的
     * 巧合去补语义上的缺失 —— 补不住，反而让启动错位累积成固定偏移。 */
    for (uint32_t i = 0; i < in->frames; i++) {
        if (in->ref != NULL) {
            aura_err_t rc = aura_pipeline_feed_ref(
                p, in->ref + (size_t)i * in->frame_count * in->ref_channels, in->frame_count,
                in->ref_channels, AURA_SAMPLE_S16, 0, AURA_WAIT_FOREVER);
            if (rc != AURA_OK) {
                fprintf(stderr, "喂参考帧 %u 失败: %s\n", i, aura_strerror(rc));
                return rc;
            }
        }
        aura_err_t rc = aura_pipeline_feed(p, in->mic + (size_t)i * in->frame_count * in->mic_channels,
                                           in->frame_count, in->mic_channels, AURA_SAMPLE_S16, 0,
                                           AURA_WAIT_FOREVER);
        if (rc != AURA_OK) {
            fprintf(stderr, "喂近端帧 %u 失败: %s\n", i, aura_strerror(rc));
            return rc;
        }
    }
    return AURA_OK;
}
