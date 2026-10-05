#include "core/pipeline/pipeline.h"

#include <stdlib.h>
#include <string.h>

#include "utils/logger/logger.h"
#include "utils/mem_pool/mem_pool.h"
#include "utils/profiler/profiler.h"

#define TAG "pipeline"

/* 帧池的块大小 = 帧头 + 最大样本区，帧与样本区同块，一次分配零碎片。 */
#define FRAME_HDR_BYTES ((uint32_t)((sizeof(aura_audio_frame_t) + 7u) & ~7u))
#define FRAME_BLOCK_BYTES (FRAME_HDR_BYTES + AURA_AUDIO_FRAME_MAX_BYTES)

/* 节点任务空闲时的轮询间隔：stop 的最坏响应延迟即此值。 */
#define NODE_IDLE_POLL_MS 20

/* 帧池取块的等待上限（见 aura_pipeline_frame_alloc 注释）。 */
#define AURA_FRAME_ALLOC_TIMEOUT_MS 2000u

struct aura_pipeline {
    aura_pipeline_cfg_t cfg;

    aura_node_t *nodes[AURA_PIPELINE_MAX_NODES];
    uint32_t     node_count;
    aura_node_t *source;     /* 音频入口节点 */
    aura_node_t *ref_sink;   /* 唯一的 AEC 参考流消费者（无则 NULL） */
    /* 参考流的自动 pts 推进（feed_ref 传 0 时用）。与近端各自独立推进：
     * 两条流可以各按帧长走，也可以由调用方按自己的时钟标注。 */
    uint64_t next_ref_pts_us;
    bool     next_ref_pts_valid;

    aura_event_bus_t *bus;
    bool              owns_bus;

    aura_mem_pool_t  *pool;
    aura_mutex_t     *lock; /* 保护统计字段 */
    aura_pipeline_stats_t stats;

    uint64_t next_pts_us; /* 外部喂帧时的自动 pts 推进 */
    bool     running;
    bool     inited; /* ops->init 是否已成功执行（stop 时据此决定是否调 ops->stop） */
};

static void node_release_queues(aura_node_t *node);

void aura_pipeline_config_default(aura_pipeline_cfg_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->sample_rate       = 16000;
    cfg->frame_ms          = 10;
    cfg->channels          = 1;
    cfg->audio_queue_depth = 8;
    cfg->text_queue_depth  = 16;
    cfg->ref_queue_depth   = 16;
    cfg->frame_pool_blocks = 16;
    cfg->event_queue_depth = 64;
}

aura_pipeline_t *aura_pipeline_create(const aura_pipeline_cfg_t *cfg)
{
    aura_pipeline_t *p = (aura_pipeline_t *)calloc(1, sizeof(*p));
    if (p == NULL) {
        return NULL;
    }
    aura_pipeline_config_default(&p->cfg);
    if (cfg != NULL) {
        p->cfg = *cfg;
        if (p->cfg.sample_rate == 0) {
            p->cfg.sample_rate = 16000;
        }
        if (p->cfg.audio_queue_depth == 0) {
            p->cfg.audio_queue_depth = 8;
        }
        if (p->cfg.text_queue_depth == 0) {
            p->cfg.text_queue_depth = 16;
        }
        if (p->cfg.ref_queue_depth == 0) {
            p->cfg.ref_queue_depth = 16;
        }
        if (p->cfg.frame_pool_blocks == 0) {
            p->cfg.frame_pool_blocks = 16;
        }
    }

    p->lock = aura_osal_mutex_create();
    p->pool = aura_mem_pool_create(FRAME_BLOCK_BYTES, p->cfg.frame_pool_blocks);
    if (p->lock == NULL || p->pool == NULL) {
        aura_pipeline_destroy(p);
        return NULL;
    }
    return p;
}

void aura_pipeline_destroy(aura_pipeline_t *p)
{
    if (p == NULL) {
        return;
    }
    aura_pipeline_stop(p);
    /* 节点结构体归调用方所有，但输入队列是 pipeline 在 add 时建的：
     * 不在这里销毁就是每次 init/deinit 泄漏一批队列（含互斥量）。 */
    for (uint32_t i = 0; i < p->node_count; i++) {
        node_release_queues(p->nodes[i]);
    }
    if (p->owns_bus && p->bus != NULL) {
        aura_event_bus_destroy(p->bus);
        p->bus = NULL;
    }
    if (p->pool != NULL) {
        aura_mem_pool_destroy(p->pool);
    }
    if (p->lock != NULL) {
        aura_osal_mutex_destroy(p->lock);
    }
    free(p);
}

aura_err_t aura_pipeline_attach_bus(aura_pipeline_t *p, aura_event_bus_t *bus)
{
    if (p == NULL || bus == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    if (p->bus != NULL && p->owns_bus) {
        aura_event_bus_destroy(p->bus);
    }
    p->bus      = bus;
    p->owns_bus = false;
    return AURA_OK;
}

aura_event_bus_t *aura_pipeline_bus(aura_pipeline_t *p)
{
    return (p == NULL) ? NULL : p->bus;
}

aura_err_t aura_pipeline_add(aura_pipeline_t *p, aura_node_t *node)
{
    if (p == NULL || node == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    if (p->running) {
        return AURA_ERR_STATE; /* 运行中不允许改拓扑 */
    }
    if (p->node_count >= AURA_PIPELINE_MAX_NODES) {
        return AURA_ERR_FULL;
    }
    for (uint32_t i = 0; i < p->node_count; i++) {
        if (p->nodes[i] == node) {
            return AURA_ERR_EXIST;
        }
    }
    if (p->bus == NULL) {
        p->bus = aura_event_bus_create(p->cfg.event_queue_depth);
        if (p->bus == NULL) {
            return AURA_ERR_NOMEM;
        }
        p->owns_bus = true;
    }

    /* AEC 参考流只支持单一消费者：两路参考意味着两套回采时钟，"哪路对哪路"
     * 无法由框架推断，属于需要显式建模的场景（多播放设备）。现在拒绝，比将来
     * 交出语义模糊的图要好。 */
    if (node->caps.consumes_ref_audio) {
        if (p->ref_sink != NULL) {
            AURA_LOGE(TAG, "node %s: ref audio consumer already registered (%s)", node->name,
                      p->ref_sink->name);
            return AURA_ERR_EXIST;
        }
        p->ref_sink = node;
    }

    node->pipeline = p;
    node->bus      = p->bus;
    /* 节点结构体归调用方所有，可能没被清零过；配对槽必须先置空，
     * 否则 stop/remove 会去释放一块从没分配过的"帧"。 */
    node->has_pending_ref = false;
    if (node->caps.consumes_audio) {
        node->in_audio = aura_osal_queue_create(p->cfg.audio_queue_depth,
                                                (uint32_t)sizeof(aura_audio_frame_t));
        if (node->in_audio == NULL) {
            return AURA_ERR_NOMEM;
        }
    }
    if (node->caps.consumes_text) {
        node->in_text = aura_osal_queue_create(p->cfg.text_queue_depth,
                                               (uint32_t)sizeof(aura_text_chunk_t));
        if (node->in_text == NULL) {
            return AURA_ERR_NOMEM;
        }
    }
    if (node->caps.consumes_ref_audio) {
        node->in_ref = aura_osal_queue_create(p->cfg.ref_queue_depth,
                                              (uint32_t)sizeof(aura_audio_frame_t));
        if (node->in_ref == NULL) {
            return AURA_ERR_NOMEM;
        }
    }
    p->nodes[p->node_count++] = node;
    return AURA_OK;
}

/* 释放节点持有的输入队列（pipeline 代建，故由 pipeline 代销毁）。 */
static void node_release_queues(aura_node_t *node)
{
    /* 配对槽里那帧也算在 frames_in_flight 里，不还回去 wait_drained 会永远
     * 等不到归零。销毁队列之前先还，池此时还活着。 */
    if (node->has_pending_ref && node->pipeline != NULL) {
        aura_pipeline_frame_free(node->pipeline, &node->pending_ref);
        node->has_pending_ref = false;
    }
    if (node->in_audio != NULL) {
        aura_osal_queue_destroy(node->in_audio);
        node->in_audio = NULL;
    }
    if (node->in_text != NULL) {
        aura_osal_queue_destroy(node->in_text);
        node->in_text = NULL;
    }
    if (node->in_ref != NULL) {
        aura_osal_queue_destroy(node->in_ref);
        node->in_ref = NULL;
    }
}

/* 从 arrays 里摘掉一个指针（保持顺序，数组很小）。 */
static void forget_downstream(aura_node_t **arr, uint32_t *count, const aura_node_t *victim)
{
    uint32_t w = 0;
    for (uint32_t r = 0; r < *count; r++) {
        if (arr[r] != victim) {
            arr[w++] = arr[r];
        }
    }
    *count = w;
}

aura_err_t aura_pipeline_remove(aura_pipeline_t *p, aura_node_t *node)
{
    if (p == NULL || node == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    if (p->running) {
        return AURA_ERR_STATE; /* 运行中不允许改拓扑 */
    }
    uint32_t idx = p->node_count;
    for (uint32_t i = 0; i < p->node_count; i++) {
        if (p->nodes[i] == node) {
            idx = i;
            break;
        }
    }
    if (idx == p->node_count) {
        return AURA_ERR_NOT_FOUND;
    }
    /* 先断开所有指向它的连线，否则其它节点的扇出数组里会留下悬垂指针。 */
    for (uint32_t i = 0; i < p->node_count; i++) {
        aura_node_t *n = p->nodes[i];
        forget_downstream(n->next_audio, &n->next_audio_count, node);
        if (n->next_text == node) {
            n->next_text = NULL;
        }
    }
    for (uint32_t i = idx + 1; i < p->node_count; i++) {
        p->nodes[i - 1] = p->nodes[i];
    }
    p->node_count--;
    if (p->source == node) {
        p->source = NULL; /* 下次 start 时重解析 */
    }
    if (p->ref_sink == node) {
        p->ref_sink = NULL;
    }
    node_release_queues(node);
    node->pipeline = NULL;
    node->bus      = NULL;
    return AURA_OK;
}

aura_err_t aura_pipeline_link(aura_pipeline_t *p, aura_node_t *up, aura_node_t *down)
{
    if (p == NULL || up == NULL || down == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    if (up == down || !down->caps.consumes_audio) {
        return AURA_ERR_INVALID_ARG;
    }
    if (!up->caps.produces_audio) {
        return AURA_ERR_INVALID_ARG;
    }
    for (uint32_t i = 0; i < up->next_audio_count; i++) {
        if (up->next_audio[i] == down) {
            return AURA_ERR_EXIST; /* 重复连线：幂等而非静默叠加，便于发现装配错误 */
        }
    }
    if (up->next_audio_count >= AURA_NODE_MAX_FANOUT) {
        AURA_LOGE(TAG, "node %s: fanout limit %d reached, cannot link %s", up->name,
                  AURA_NODE_MAX_FANOUT, down->name);
        return AURA_ERR_FULL;
    }
    up->next_audio[up->next_audio_count++] = down;
    return AURA_OK;
}

aura_err_t aura_pipeline_link_text(aura_pipeline_t *p, aura_node_t *up, aura_node_t *down)
{
    if (p == NULL || up == NULL || down == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    if (up == down || !down->caps.consumes_text) {
        return AURA_ERR_INVALID_ARG;
    }
    if (!up->caps.produces_text) {
        return AURA_ERR_INVALID_ARG;
    }
    up->next_text = down;
    return AURA_OK;
}

/* 未显式连线的节点，按注册顺序自动接线。
 *
 * 规则（比 Phase 1 的"接到后面第一个消费者"多了一条，为的是不饿死主线）：
 * 从 i 向后扫描消费音频的节点 ——
 *   - 第一个**自己也产出音频**的节点是主线，接为 next_audio[0] 并停止扫描；
 *   - 扫描途中遇到的**纯消费者**（观察者 VAD/KWS、或终点 sink）接到扇出旁路。
 *
 * 这样 `aec, vad, kws, sink` 会接成 aec→{kws, vad}、kws→sink，而不是
 * aec→vad 就把链断在 VAD 上（vad 不产出，kws 永远收不到帧）。
 * 显式 chain_build 的链路不走这条路径（链装配显式 link），此处只为手工
 * 注册的场景兜底。 */
static void resolve_links(aura_pipeline_t *p)
{
    for (uint32_t i = 0; i < p->node_count; i++) {
        aura_node_t *n = p->nodes[i];
        if (n->caps.produces_audio && n->next_audio_count == 0) {
            for (uint32_t j = i + 1; j < p->node_count; j++) {
                aura_node_t *cand = p->nodes[j];
                if (!cand->caps.consumes_audio) {
                    continue;
                }
                if (n->next_audio_count >= AURA_NODE_MAX_FANOUT) {
                    break;
                }
                n->next_audio[n->next_audio_count++] = cand;
                if (cand->caps.produces_audio) {
                    break; /* 主线接上，其后节点由 cand 自己接续 */
                }
                /* 纯消费者：旁路观察者或终点，继续向后找主线。 */
            }
        }
        if (n->caps.produces_text && n->next_text == NULL) {
            for (uint32_t j = i + 1; j < p->node_count; j++) {
                if (p->nodes[j]->caps.consumes_text) {
                    n->next_text = p->nodes[j];
                    break;
                }
            }
        }
    }
    if (p->source == NULL) {
        for (uint32_t i = 0; i < p->node_count; i++) {
            if (p->nodes[i]->caps.consumes_audio) {
                p->source = p->nodes[i];
                break;
            }
        }
    }
}

static void pipeline_process_audio(aura_node_t *n, const aura_audio_frame_t *frame)
{
    aura_pipeline_t *p = n->pipeline;
    bool terminal = !(n->caps.produces_audio || n->caps.produces_text);

    n->last_in_pts_us    = frame->pts_us;
    n->last_in_pts_valid = true;

    uint64_t   t0 = aura_osal_time_us();
    aura_err_t rc = (n->ops->process_audio != NULL) ? n->ops->process_audio(n, frame) : AURA_OK;
    uint64_t   dt = aura_osal_time_us() - t0;

    n->last_in_pts_valid = false;

    n->stats.processed++;
    n->stats.process_us_total += dt;
    if (dt > n->stats.process_us_max) {
        n->stats.process_us_max = dt;
    }
    if (rc != AURA_OK) {
        n->stats.errors++;
        aura_osal_mutex_lock(p->lock);
        p->stats.node_errors++;
        aura_osal_mutex_unlock(p->lock);
        /* 节点自己没上抛时，pipeline 代为上抛（契约：错误不得静默）。 */
        aura_node_report_error(n, rc, "process_audio failed");
    }
    if (terminal) {
        aura_osal_mutex_lock(p->lock);
        p->stats.frames_out++;
        aura_osal_mutex_unlock(p->lock);
    }
    aura_pipeline_frame_free(p, (aura_audio_frame_t *)frame);
}

/* 参考帧：不进主链的最后输入 pts 契约，也不参与 frames_out 统计 —— 它是旁路，
 * 不是主音频流的一部分。 */
static void pipeline_process_ref(aura_node_t *n, const aura_audio_frame_t *frame)
{
    aura_pipeline_t *p = n->pipeline;

    uint64_t   t0 = aura_osal_time_us();
    aura_err_t rc =
        (n->ops->process_ref_audio != NULL) ? n->ops->process_ref_audio(n, frame) : AURA_OK;
    uint64_t dt = aura_osal_time_us() - t0;

    n->stats.processed++;
    n->stats.process_us_total += dt;
    if (dt > n->stats.process_us_max) {
        n->stats.process_us_max = dt;
    }
    if (rc != AURA_OK) {
        n->stats.errors++;
        aura_osal_mutex_lock(p->lock);
        p->stats.node_errors++;
        aura_osal_mutex_unlock(p->lock);
        aura_node_report_error(n, rc, "process_ref_audio failed");
    }
    aura_pipeline_frame_free(p, (aura_audio_frame_t *)frame);
}

static void pipeline_process_text(aura_node_t *n, const aura_text_chunk_t *chunk)
{
    aura_pipeline_t *p = n->pipeline;
    bool terminal = !(n->caps.produces_audio || n->caps.produces_text);

    uint64_t   t0 = aura_osal_time_us();
    aura_err_t rc = (n->ops->process_text != NULL) ? n->ops->process_text(n, chunk) : AURA_OK;
    uint64_t   dt = aura_osal_time_us() - t0;

    n->stats.processed++;
    n->stats.process_us_total += dt;
    if (dt > n->stats.process_us_max) {
        n->stats.process_us_max = dt;
    }
    if (rc != AURA_OK) {
        n->stats.errors++;
        aura_osal_mutex_lock(p->lock);
        p->stats.node_errors++;
        aura_osal_mutex_unlock(p->lock);
        aura_node_report_error(n, rc, "process_text failed");
    }
    if (terminal) {
        aura_osal_mutex_lock(p->lock);
        p->stats.text_out++;
        aura_osal_mutex_unlock(p->lock);
    }
}

/* 参考配对：把"属于 pts = T 这一近端帧"的参考投给节点。
 *
 * 为什么要配对而不是按到达顺序投：早先参考只有在音频队列空的那一瞬才会被取走，
 * 于是启动阶段前十几帧近端拿不到参考、参考又在缓冲里越积越多，两者就此错开
 * 一个固定偏移并随调度抖动漂移。AEC 拿到的参考落在近端"未来"，回声对不上，
 * 表现是 ERLE 只剩十几 dB 而所有统计一切正常（ref_missed 甚至可能是 0）。
 *
 * 规则（两条流同一时基；参考未标注 pts 时由 feed_ref 自动推进，天然与近端对齐）：
 *   ref.pts + 帧长 <= T     → 它对应的近端帧已经过去，丢弃并计 ref_stale
 *   ref.pts <= T < +帧长    → 就是这一帧，投下去
 *   ref.pts >  T            → 还没轮到，留在 pending 槽等下一帧
 *
 * 一帧近端最多配一帧参考：AEC 是 1:1 消费的，多投只会堆在适配层 FIFO 里，
 * 把后面的参考挤成过期 —— 那正是要消灭的东西。
 *
 * 配不上（槽空）时本帧不投参考，适配器按缺参考降级（静音顶替 + ref_missed）。
 * 关键是**只降级这一帧**：下一帧按自己的 pts 照常配对，不会像顺序投那样
 * 让偏移一直累积下去。 */
static void pipeline_pair_ref(aura_node_t *n, uint64_t pts_us)
{
    if (n->in_ref == NULL) {
        return;
    }
    aura_pipeline_t *p = n->pipeline;

    /* 槽里那帧是最早到达的候选；槽空才再取一帧。 */
    if (!n->has_pending_ref &&
        aura_osal_queue_pop(n->in_ref, &n->pending_ref, AURA_NO_WAIT) == AURA_OK) {
        n->has_pending_ref = true;
    }

    /* 丢弃已经过期的（含连锁丢弃：一次音频可能跨过好几帧参考）。 */
    while (n->has_pending_ref) {
        const aura_audio_frame_t *r = &n->pending_ref;
        uint32_t rate = r->sample_rate ? r->sample_rate : p->cfg.sample_rate;
        uint64_t dur  = (uint64_t)r->frame_count * 1000000ull / rate;
        if (r->pts_us + dur > pts_us) {
            break; /* 还没过期 */
        }
        aura_pipeline_frame_free(p, &n->pending_ref);
        n->has_pending_ref = false;
        aura_osal_mutex_lock(p->lock);
        p->stats.ref_stale++;
        aura_osal_mutex_unlock(p->lock);
        if (aura_osal_queue_pop(n->in_ref, &n->pending_ref, AURA_NO_WAIT) != AURA_OK) {
            break;
        }
        n->has_pending_ref = true;
    }

    if (!n->has_pending_ref) {
        return; /* 缺参考：本帧降级 */
    }
    if (n->pending_ref.pts_us > pts_us) {
        return; /* 领先：留在槽里等它对应的那一帧近端 */
    }
    pipeline_process_ref(n, &n->pending_ref); /* 内部会 frame_free */
    n->has_pending_ref = false;
}

/* 投一帧近端：先把它该配的参考配好。参考必须先于近端进入节点，
 * 因为适配器是"处理近端时从参考 FIFO 取头"的。 */
static void node_handle_audio(aura_node_t *n, const aura_audio_frame_t *frame)
{
    pipeline_pair_ref(n, frame->pts_us);
    pipeline_process_audio(n, frame);
}

static void node_task(void *arg)
{
    aura_node_t *n = (aura_node_t *)arg;
    aura_audio_frame_t frame;
    aura_text_chunk_t  chunk;

    while (n->running) {
        bool did = false;

        /* 快路径：三条队列都非阻塞试一次，避免同节点内音频/参考/文本互相饿死。
         * 音频优先于参考：主链缺帧会掉帧，参考缺帧只是 AEC 降级。 */
        if (n->in_audio != NULL &&
            aura_osal_queue_pop(n->in_audio, &frame, AURA_NO_WAIT) == AURA_OK) {
            node_handle_audio(n, &frame);
            did = true;
        }
        /* 参考独立取帧只留给**不消费音频**的纯参考节点。AEC 这类"音频+参考"
         * 节点的参考一律由 node_handle_audio 里的 pts 配对投递，不能再走这条
         * 路 —— 否则同一帧参考会被投两次（配一次、这里按顺序再投一次），
         * 正是要修掉的错位来源。 */
        if (!did && n->in_ref != NULL && n->in_audio == NULL &&
            aura_osal_queue_pop(n->in_ref, &frame, AURA_NO_WAIT) == AURA_OK) {
            pipeline_process_ref(n, &frame);
            did = true;
        }
        if (!did && n->in_text != NULL &&
            aura_osal_queue_pop(n->in_text, &chunk, AURA_NO_WAIT) == AURA_OK) {
            pipeline_process_text(n, &chunk);
            did = true;
        }
        if (did) {
            continue;
        }

        /* 慢路径：在主队列上阻塞等待（超时用于周期性检查 running）。 */
        if (n->in_audio != NULL) {
            if (aura_osal_queue_pop(n->in_audio, &frame, NODE_IDLE_POLL_MS) == AURA_OK) {
                node_handle_audio(n, &frame);
                continue;
            }
        }
        if (n->in_ref != NULL && n->in_audio == NULL) {
            if (aura_osal_queue_pop(n->in_ref, &frame, NODE_IDLE_POLL_MS) == AURA_OK) {
                pipeline_process_ref(n, &frame);
                continue;
            }
        }
        if (n->in_text != NULL) {
            uint32_t timeout = (n->in_audio != NULL || n->in_ref != NULL) ? AURA_NO_WAIT
                                                                          : NODE_IDLE_POLL_MS;
            if (aura_osal_queue_pop(n->in_text, &chunk, timeout) == AURA_OK) {
                pipeline_process_text(n, &chunk);
            }
        }
        if (n->in_audio == NULL && n->in_text == NULL && n->in_ref == NULL) {
            /* 无输入队列的节点（事件驱动型）：只做周期轮询，不忙转。 */
            aura_osal_sleep_ms(NODE_IDLE_POLL_MS);
        }
    }
}

/* 节点订阅总线事件时的转发（caps.wants_events）。 */
static void node_event_forwarder(const aura_event_t *event, void *user)
{
    aura_node_t *n = (aura_node_t *)user;
    if (n != NULL && n->ops != NULL && n->ops->on_event != NULL) {
        n->ops->on_event(n, event);
    }
}

static uint32_t all_events_mask(void)
{
    uint32_t mask = 0;
    for (int i = 1; i < AURA_EVENT_TYPE_COUNT; i++) {
        mask |= (1u << i);
    }
    return mask;
}

aura_err_t aura_pipeline_start(aura_pipeline_t *p)
{
    if (p == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    if (p->running) {
        return AURA_ERR_STATE;
    }
    if (p->source == NULL) {
        resolve_links(p);
    }
    if (p->source == NULL) {
        AURA_LOGE(TAG, "no audio-consuming node registered");
        return AURA_ERR_STATE;
    }

    for (uint32_t i = 0; i < p->node_count; i++) {
        aura_node_t *n = p->nodes[i];
        if (n->ops->init != NULL) {
            aura_err_t rc = n->ops->init(n);
            if (rc != AURA_OK) {
                AURA_LOGE(TAG, "node %s init failed: %s", n->name, aura_strerror(rc));
                return rc;
            }
        }
    }
    for (uint32_t i = 0; i < p->node_count; i++) {
        aura_node_t *n = p->nodes[i];
        if (n->ops->start != NULL) {
            aura_err_t rc = n->ops->start(n);
            if (rc != AURA_OK) {
                AURA_LOGE(TAG, "node %s start failed: %s", n->name, aura_strerror(rc));
                return rc;
            }
        }
        if (n->caps.wants_events && p->bus != NULL) {
            aura_event_bus_subscribe(p->bus, all_events_mask(), node_event_forwarder, n);
        }
    }

    p->next_pts_us = 0;
    p->running     = true;
    p->inited      = true;
    for (uint32_t i = 0; i < p->node_count; i++) {
        aura_node_t *n = p->nodes[i];
        n->running     = true;
        n->task = aura_osal_task_create(n->name, 8192, 3, node_task, n);
        if (n->task == NULL) {
            AURA_LOGE(TAG, "node %s task create failed", n->name);
            aura_pipeline_stop(p);
            return AURA_ERR_NOMEM;
        }
    }
    AURA_LOGI(TAG, "pipeline started: %u nodes, source=%s", p->node_count, p->source->name);
    return AURA_OK;
}

aura_err_t aura_pipeline_stop(aura_pipeline_t *p)
{
    if (p == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    p->running = false;
    for (uint32_t i = 0; i < p->node_count; i++) {
        aura_node_t *n = p->nodes[i];
        n->running     = false;
        if (n->task != NULL) {
            aura_osal_task_destroy(n->task);
            n->task = NULL;
        }
        /* 任务已停，槽不会再被改：在销毁队列之前把这帧还回帧池。 */
        if (n->has_pending_ref) {
            aura_pipeline_frame_free(p, &n->pending_ref);
            n->has_pending_ref = false;
        }
    }
    for (uint32_t i = p->node_count; i > 0; i--) {
        aura_node_t *n = p->nodes[i - 1];
        if (n->caps.wants_events && p->bus != NULL) {
            aura_event_bus_unsubscribe(p->bus, node_event_forwarder);
        }
        /* 只有 init 过的节点才允许 stop，避免对未初始化资源做释放。 */
        if (p->inited && n->ops->stop != NULL) {
            n->ops->stop(n);
        }
    }
    if (p->inited) {
        for (uint32_t i = p->node_count; i > 0; i--) {
            aura_node_t *n = p->nodes[i - 1];
            if (n->ops->deinit != NULL) {
                n->ops->deinit(n);
            }
        }
    }
    p->inited = false;
    return AURA_OK;
}

aura_err_t aura_pipeline_feed(aura_pipeline_t *p, const void *pcm, uint32_t frame_count,
                              uint32_t channels, aura_sample_fmt_t fmt, uint64_t pts_us,
                              uint32_t timeout_ms)
{
    if (p == NULL || pcm == NULL || frame_count == 0 || channels == 0) {
        return AURA_ERR_INVALID_ARG;
    }
    if (!p->running || p->source == NULL || p->source->in_audio == NULL) {
        return AURA_ERR_STATE;
    }
    uint32_t bytes = aura_audio_frame_bytes((uint16_t)channels, (uint16_t)frame_count, fmt);
    if (bytes > AURA_AUDIO_FRAME_MAX_BYTES) {
        return AURA_ERR_INVALID_ARG;
    }

    aura_audio_frame_t *f = aura_pipeline_frame_alloc(p);
    if (f == NULL) {
        aura_osal_mutex_lock(p->lock);
        p->stats.frames_dropped++;
        aura_osal_mutex_unlock(p->lock);
        return AURA_ERR_NOMEM;
    }
    f->sample_rate = p->cfg.sample_rate;
    f->channels    = (uint16_t)channels;
    f->frame_count = (uint16_t)frame_count;
    f->fmt         = fmt;
    f->data_bytes  = bytes;

    /* pts 统一由 pipeline 维护：未指定（0）时按帧长自动推进。
     * 注意自动路径的时基是**相对 pipeline 起点的单调时间**（首帧 = 0），
     * 不用挂钟时间 —— 挂钟值巨大且跨重启不一致，会破坏下游的 pts 对齐。 */
    if (pts_us != 0) {
        f->pts_us = pts_us;
    } else {
        f->pts_us = p->next_pts_us;
    }
    p->next_pts_us = f->pts_us + (uint64_t)frame_count * 1000000ull / p->cfg.sample_rate;

    memcpy(f->data, pcm, bytes);

    if (aura_osal_queue_push(p->source->in_audio, f, timeout_ms) != AURA_OK) {
        aura_pipeline_frame_free(p, f);
        aura_osal_mutex_lock(p->lock);
        p->stats.frames_dropped++;
        aura_osal_mutex_unlock(p->lock);
        return AURA_ERR_FULL;
    }
    aura_osal_mutex_lock(p->lock);
    p->stats.frames_in++;
    aura_osal_mutex_unlock(p->lock);
    return AURA_OK;
}

aura_err_t aura_pipeline_feed_ref(aura_pipeline_t *p, const void *pcm, uint32_t frame_count,
                                  uint32_t channels, aura_sample_fmt_t fmt, uint64_t pts_us,
                                  uint32_t timeout_ms)
{
    if (p == NULL || pcm == NULL || frame_count == 0 || channels == 0) {
        return AURA_ERR_INVALID_ARG;
    }
    if (!p->running || p->ref_sink == NULL || p->ref_sink->in_ref == NULL) {
        return AURA_ERR_STATE;
    }
    uint32_t bytes = aura_audio_frame_bytes((uint16_t)channels, (uint16_t)frame_count, fmt);
    if (bytes > AURA_AUDIO_FRAME_MAX_BYTES) {
        return AURA_ERR_INVALID_ARG;
    }

    aura_audio_frame_t *f = aura_pipeline_frame_alloc(p);
    if (f == NULL) {
        aura_osal_mutex_lock(p->lock);
        p->stats.ref_dropped++;
        aura_osal_mutex_unlock(p->lock);
        return AURA_ERR_NOMEM;
    }
    f->sample_rate = p->cfg.sample_rate;
    f->channels    = (uint16_t)channels;
    f->frame_count = (uint16_t)frame_count;
    f->fmt         = fmt;
    f->data_bytes  = bytes;
    /* pts 规则与 feed 一致：传 0 = 让 pipeline 按帧长自动推进（默认与近端
     * 同节奏，两边各自从 0 起步，正好一一对应）；传非 0 = 调用方按播放时间轴
     * 标注，用于回采时钟与采集时钟不同源时显式对齐。
     *
     * 参考**必须**有时间轴：pipeline 靠 pts 把它和近端帧配对（pipeline_pair_ref）。
     * 早先这里写"传 0 就是不关心"，结果参考只能按到达顺序投，启动错位会静默
     * 累积成固定偏移，AEC 完全不收敛而统计一切正常 —— 时间轴不是可选项。 */
    uint64_t ref_dur = (uint64_t)frame_count * 1000000ull / p->cfg.sample_rate;
    if (pts_us != 0) {
        f->pts_us           = pts_us;
        p->next_ref_pts_us  = pts_us + ref_dur;
        p->next_ref_pts_valid = true;
    } else {
        f->pts_us = p->next_ref_pts_valid ? p->next_ref_pts_us : 0;
        p->next_ref_pts_us += ref_dur;
        p->next_ref_pts_valid = true;
    }

    memcpy(f->data, pcm, bytes);

    if (aura_osal_queue_push(p->ref_sink->in_ref, f, timeout_ms) != AURA_OK) {
        aura_pipeline_frame_free(p, f);
        aura_osal_mutex_lock(p->lock);
        p->stats.ref_dropped++;
        aura_osal_mutex_unlock(p->lock);
        return AURA_ERR_FULL;
    }
    aura_osal_mutex_lock(p->lock);
    p->stats.ref_in++;
    aura_osal_mutex_unlock(p->lock);
    return AURA_OK;
}

aura_err_t aura_pipeline_control(aura_pipeline_t *p, aura_node_cmd_t cmd)
{
    if (p == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    aura_err_t last = AURA_OK;
    for (uint32_t i = 0; i < p->node_count; i++) {
        aura_node_t *n = p->nodes[i];
        if (n->ops->control != NULL) {
            aura_err_t rc = n->ops->control(n, cmd, NULL);
            if (rc != AURA_OK) {
                last = rc;
            }
        }
    }
    return last;
}

aura_err_t aura_pipeline_control_node(aura_pipeline_t *p, aura_node_t *node, aura_node_cmd_t cmd,
                                      const void *arg)
{
    if (p == NULL || node == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    if (node->ops->control == NULL) {
        return AURA_ERR_UNSUPPORTED;
    }
    return node->ops->control(node, cmd, arg);
}

aura_err_t aura_pipeline_flush(aura_pipeline_t *p)
{
    if (p == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    /* 先清空队列（丢弃在途帧），再通知节点丢弃自有缓存。 */
    for (uint32_t i = 0; i < p->node_count; i++) {
        aura_node_t *n = p->nodes[i];
        aura_audio_frame_t f;
        while (n->in_audio != NULL &&
               aura_osal_queue_pop(n->in_audio, &f, AURA_NO_WAIT) == AURA_OK) {
            aura_pipeline_frame_free(p, &f);
        }
        /* 参考队列同样清空：打断时在途的播放参考已经无意义，留着只会让 AEC
         * 拿旧参考去抵消新的一段近端语音。配对槽里的那帧同理。 */
        if (n->has_pending_ref) {
            aura_pipeline_frame_free(p, &n->pending_ref);
            n->has_pending_ref = false;
        }
        while (n->in_ref != NULL &&
               aura_osal_queue_pop(n->in_ref, &f, AURA_NO_WAIT) == AURA_OK) {
            aura_pipeline_frame_free(p, &f);
        }
        if (n->in_text != NULL) {
            aura_osal_queue_reset(n->in_text);
        }
    }
    for (uint32_t i = 0; i < p->node_count; i++) {
        aura_node_t *n = p->nodes[i];
        if (n->ops->control != NULL) {
            n->ops->control(n, AURA_CMD_FLUSH, NULL);
        }
    }
    if (p->bus != NULL) {
        aura_event_bus_flush(p->bus);
    }
    return AURA_OK;
}

aura_err_t aura_pipeline_wait_drained(aura_pipeline_t *p, uint32_t timeout_ms)
{
    if (p == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    uint64_t deadline = aura_osal_time_ms() + timeout_ms;
    for (;;) {
        bool busy = false;
        for (uint32_t i = 0; i < p->node_count; i++) {
            if (aura_node_pending(p->nodes[i]) > 0) {
                busy = true;
                break;
            }
        }
        if (!busy) {
            aura_osal_mutex_lock(p->lock);
            uint64_t in_flight = p->stats.frames_in_flight;
            aura_osal_mutex_unlock(p->lock);
            if (in_flight == 0) {
                return AURA_OK;
            }
        }
        if (aura_osal_time_ms() >= deadline) {
            return AURA_ERR_TIMEOUT;
        }
        aura_osal_sleep_ms(2);
    }
}

void aura_pipeline_stats(const aura_pipeline_t *p, aura_pipeline_stats_t *out)
{
    if (p == NULL || out == NULL) {
        return;
    }
    aura_pipeline_t *m = (aura_pipeline_t *)p;
    aura_osal_mutex_lock(m->lock);
    *out = m->stats;
    aura_osal_mutex_unlock(m->lock);
}

void aura_pipeline_dump(const aura_pipeline_t *p)
{
    if (p == NULL) {
        return;
    }
    aura_pipeline_t *m = (aura_pipeline_t *)p;
    AURA_LOGI(TAG, "%-14s %6s %8s %8s %10s %10s", "node", "pend", "frames", "drops",
              "max_us", "avg_us");
    for (uint32_t i = 0; i < m->node_count; i++) {
        const aura_node_t *n = m->nodes[i];
        uint64_t avg = (n->stats.processed > 0)
                           ? (n->stats.process_us_total / n->stats.processed)
                           : 0;
        AURA_LOGI(TAG, "%-14s %6u %8llu %8llu %10llu %10llu", n->name,
                  aura_node_pending(n), (unsigned long long)n->stats.processed,
                  (unsigned long long)n->stats.dropped,
                  (unsigned long long)n->stats.process_us_max, (unsigned long long)avg);
    }
    aura_pipeline_stats_t s;
    aura_pipeline_stats(m, &s);
    AURA_LOGI(TAG, "in=%llu out=%llu dropped=%llu in_flight=%llu errors=%llu",
              (unsigned long long)s.frames_in, (unsigned long long)s.frames_out,
              (unsigned long long)s.frames_dropped, (unsigned long long)s.frames_in_flight,
              (unsigned long long)s.node_errors);
    if (s.ref_in > 0 || s.ref_dropped > 0) {
        AURA_LOGI(TAG, "ref: in=%llu dropped=%llu", (unsigned long long)s.ref_in,
                  (unsigned long long)s.ref_dropped);
    }
    if (m->bus != NULL) {
        aura_event_bus_stats_t es;
        aura_event_bus_stats(m->bus, &es);
        AURA_LOGI(TAG, "events published=%llu dispatched=%llu dropped=%llu",
                  (unsigned long long)es.published, (unsigned long long)es.dispatched,
                  (unsigned long long)es.dropped);
    }
}

uint32_t aura_pipeline_node_count(const aura_pipeline_t *p)
{
    return (p == NULL) ? 0 : p->node_count;
}

aura_node_t *aura_pipeline_node_at(const aura_pipeline_t *p, uint32_t idx)
{
    if (p == NULL || idx >= p->node_count) {
        return NULL;
    }
    return p->nodes[idx];
}

aura_node_t *aura_pipeline_source(aura_pipeline_t *p)
{
    if (p == NULL) {
        return NULL;
    }
    if (p->source == NULL) {
        resolve_links(p);
    }
    return p->source;
}

const aura_pipeline_cfg_t *aura_pipeline_config(const aura_pipeline_t *p)
{
    return (p == NULL) ? NULL : &p->cfg;
}

aura_err_t aura_pipeline_selftest(aura_pipeline_t *p)
{
    if (p == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    for (uint32_t i = 0; i < p->node_count; i++) {
        aura_node_t *n = p->nodes[i];
        if (n->ops->selftest != NULL) {
            aura_err_t rc = n->ops->selftest(n);
            if (rc != AURA_OK) {
                AURA_LOGE(TAG, "selftest failed on node %s: %s", n->name, aura_strerror(rc));
                return rc;
            }
        }
    }
    return AURA_OK;
}

aura_audio_frame_t *aura_pipeline_frame_alloc(aura_pipeline_t *p)
{
    if (p == NULL) {
        return NULL;
    }
    /* 线性链上帧一定会被释放，正常情况下不会等满；超时用于把"帧泄漏"这类
     * 隐性 bug 变成可见的失败，而不是永久挂死。 */
    uint8_t *blk = (uint8_t *)aura_mem_pool_alloc(p->pool, AURA_FRAME_ALLOC_TIMEOUT_MS);
    if (blk == NULL) {
        AURA_LOGE(TAG, "frame pool exhausted (%u blocks, in_use=%u)", p->cfg.frame_pool_blocks,
                  aura_mem_pool_in_use(p->pool));
        return NULL;
    }
    aura_audio_frame_t *f = (aura_audio_frame_t *)blk;
    memset(f, 0, FRAME_HDR_BYTES);
    f->data          = blk + FRAME_HDR_BYTES;
    f->sample_rate   = p->cfg.sample_rate;

    aura_osal_mutex_lock(p->lock);
    p->stats.frames_in_flight++;
    aura_osal_mutex_unlock(p->lock);
    return f;
}

void aura_pipeline_frame_free(aura_pipeline_t *p, aura_audio_frame_t *frame)
{
    if (p == NULL || frame == NULL || frame->data == NULL) {
        return;
    }
    /* 帧结构按值穿过队列（局部拷贝），真正的池块首地址只能从 data 反推：
     * 池块布局 = [帧头][样本区]，data = 块首 + FRAME_HDR_BYTES。 */
    uint8_t *blk = frame->data - FRAME_HDR_BYTES;
    aura_osal_mutex_lock(p->lock);
    if (p->stats.frames_in_flight > 0) {
        p->stats.frames_in_flight--;
    }
    aura_osal_mutex_unlock(p->lock);
    aura_mem_pool_free(p->pool, blk);
}
