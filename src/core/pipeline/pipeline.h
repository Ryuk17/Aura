/* Aura — Pipeline 引擎
 *
 * 职责：节点注册与连线、每节点一条任务、帧池管理、pts 维护、错误上抛、统计。
 *
 * 拓扑：**树形受限 DAG** —— 一条线性主链 + 扇出旁路观察（tee）+ AEC 参考第二输入。
 * 不是一个通用的有向图，理由见 docs/algorithm_unified_api.md：
 *
 *   - 控制/结果面本来就是广播（event_bus），不需要图来承载多消费者；
 *   - 音频面唯一的多消费者场景是"旁路观察"（VAD/KWS/audio_debug 同时看同一路
 *     PCM），用扇出即可覆盖；
 *   - 真正的多源汇聚（两个上游的时间轴合成一路）在语音拓扑里没有对应场景，
 *     为它引入多源 pts 仲裁与引用计数是无谓的复杂度。
 *
 * 三个平面：
 *   音频面 —— `next_audio[]` 扇出（主线 + 观察者），`feed_ref` 供 AEC 参考流；
 *   文本面 —— `next_text` 单下游（文本没有旁路观察场景）；
 *   事件面 —— event_bus 天然广播，不受拓扑约束。
 *
 * pts 维护：`aura_pipeline_feed` 由调用方给出首帧 pts；此后 pipeline 按
 * `帧长 / 采样率` 自动推进下游 pts，保证各通路严格对齐（todo.md 4.2）。
 * 节点**不得**自行改写 pts，唯一例外是 `caps.changes_frame_rate` 的节点（SRC）。
 *
 * 帧内存：全部来自 pipeline 自有的 mem_pool，运行期无 malloc/free。
 */
#ifndef AURA_CORE_PIPELINE_PIPELINE_H
#define AURA_CORE_PIPELINE_PIPELINE_H

#include <stdbool.h>
#include <stdint.h>

#include "core/event_bus/event_bus.h"
#include "core/node.h"
#include "osal/osal.h"
#include "utils/error.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef AURA_PIPELINE_MAX_NODES
#define AURA_PIPELINE_MAX_NODES 32
#endif

typedef struct aura_pipeline_cfg {
    uint32_t sample_rate;        /* 链路主采样率，默认 16000 */
    uint32_t frame_ms;           /* 帧长（毫秒），默认 10 */
    uint32_t channels;           /* 采集通道数（含参考通道），默认 1 */
    uint32_t audio_queue_depth;  /* 每节点音频队列深度，默认 8 */
    uint32_t text_queue_depth;   /* 每节点文本队列深度，默认 16 */
    /* AEC 参考流队列深度，默认 16（比主音频深：参考流由播放回调喂入，
     * 突发性更强，且 AEC 缺参考只能降级处理）。 */
    uint32_t ref_queue_depth;
    /* 帧池块数，默认 16（每块含 MAX_BYTES 样本区）。
     * 扇出会**成倍消耗**帧池：N 路扇出时同一帧同时在途 N 份，
     * 按"最长路径节点数 × 最大扇出宽度"估算，默认 16 够 AEC→VAD/KWS 这类拓扑。 */
    uint32_t frame_pool_blocks;
    uint32_t event_queue_depth;  /* 内建 event_bus 队列深度，默认 64 */
} aura_pipeline_cfg_t;

typedef struct aura_pipeline_stats {
    uint64_t frames_in;       /* 外部喂入帧数 */
    uint64_t frames_out;      /* 终止于 sink 的帧数 */
    uint64_t frames_dropped;  /* 队列满丢弃帧数 */
    uint64_t frames_in_flight;/* 当前在途（含正在处理）帧数 */
    uint64_t text_in;
    uint64_t text_out;
    uint64_t text_dropped;
    uint64_t ref_in;      /* 外部喂入的 AEC 参考帧数 */
    uint64_t ref_dropped; /* 参考帧丢弃数（队列满/帧池耗尽：AEC 降级，不算致命） */
    /* 参考帧因"对应近端帧已经处理过去"被丢弃的帧数。参考按 pts 配对，正常
     * 不该有；持续非 0 说明参考喂得比近端快（时间轴标错或播放回调在空转）。
     * 早先没有 pts 配对时，这类错位会静默累积成几百毫秒的固定偏移，
     * 表现是 AEC 完全不收敛却一切统计正常 —— 所以这个数必须看得见。 */
    uint64_t ref_stale;
    uint64_t node_errors;
} aura_pipeline_stats_t;

typedef struct aura_pipeline aura_pipeline_t;

void aura_pipeline_config_default(aura_pipeline_cfg_t *cfg);

aura_pipeline_t *aura_pipeline_create(const aura_pipeline_cfg_t *cfg);
void             aura_pipeline_destroy(aura_pipeline_t *p);

/* 绑定外部 event_bus；不调用则由 pipeline 自建一个（destroy 时一并释放）。 */
aura_err_t aura_pipeline_attach_bus(aura_pipeline_t *p, aura_event_bus_t *bus);
aura_event_bus_t *aura_pipeline_bus(aura_pipeline_t *p);

/* 注册节点。节点结构体在 pipeline 生命周期内必须保持有效。
 * pipeline 在注册时为其建输入队列，并在 remove/destroy 时代为销毁。 */
aura_err_t aura_pipeline_add(aura_pipeline_t *p, aura_node_t *node);

/* 摘除节点：断开所有指向它的连线、销毁其输入队列。**不释放节点结构体**
 * （那是调用方/工厂的事）。运行中调用返回 AURA_ERR_STATE。
 * 用途：链装配失败回滚、运行期重建链。 */
aura_err_t aura_pipeline_remove(aura_pipeline_t *p, aura_node_t *node);

/* 连线：up 的音频产出送往 down。up->caps.produces_audio 必须为 true。
 * 可对同一 up 多次调用形成**扇出**（上限 AURA_NODE_MAX_FANOUT）：
 * 第一次连的是主线 next_audio[0]，后续为旁路观察者。
 * 未连线时，节点的音频产出按注册顺序自动接到下一个消费音频的节点（隐式线性）。 */
aura_err_t aura_pipeline_link(aura_pipeline_t *p, aura_node_t *up, aura_node_t *down);

/* 同上，用于文本流（单下游，重复连线会覆盖）。 */
aura_err_t aura_pipeline_link_text(aura_pipeline_t *p, aura_node_t *up, aura_node_t *down);

/* 节点遍历：装配完成后可由上层取出节点句柄（chain_build / 分组控制）。
 * idx 越界返回 NULL。顺序 = 注册顺序。 */
uint32_t     aura_pipeline_node_count(const aura_pipeline_t *p);
aura_node_t *aura_pipeline_node_at(const aura_pipeline_t *p, uint32_t idx);

/* 取音频入口节点（未 start 时会先解析隐式连线）。无入口返回 NULL。 */
aura_node_t *aura_pipeline_source(aura_pipeline_t *p);

/* 取生效配置（链组装需要知道链路主采样率/帧长）。生命周期同 pipeline。 */
const aura_pipeline_cfg_t *aura_pipeline_config(const aura_pipeline_t *p);

/* 启动：创建各节点任务、调用 ops->init/start。 */
aura_err_t aura_pipeline_start(aura_pipeline_t *p);

/* 停止：通知各节点退出、join 任务、调用 ops->stop。
 * 幂等，可在任何状态下调用。 */
aura_err_t aura_pipeline_stop(aura_pipeline_t *p);

/* 外部喂音频（采集线程 / 文件回放 / host_sim 入口），推给第一个音频节点。
 * timeout_ms 为入队等待上限（文件喂入可用 AURA_WAIT_FOREVER 做背压）。 */
aura_err_t aura_pipeline_feed(aura_pipeline_t *p, const void *pcm, uint32_t frame_count,
                              uint32_t channels, aura_sample_fmt_t fmt, uint64_t pts_us,
                              uint32_t timeout_ms);

/* 喂 AEC 参考音频（播放回采），推给链上**唯一**的 caps.consumes_ref_audio 节点。
 *
 * 与 feed 的关键差别：参考流与主采集流是两个物理时钟源（播放回调 vs mic 采集），
 * 帧数/时刻都不对齐，所以这里**不推进 pts、不与主链做任何配对** —— pts 由调用方
 * 按播放时间轴标注，节点自己按 pts 对齐（AEC 适配器内维护参考环缓冲）。
 * 缺失的参考帧不会由 pipeline 补静音：节点发现缓冲里没有对应 pts 的参考时，
 * 自行喂静音并计入 ref_missed 统计（比 pipeline 猜要准确）。 */
aura_err_t aura_pipeline_feed_ref(aura_pipeline_t *p, const void *pcm, uint32_t frame_count,
                                  uint32_t channels, aura_sample_fmt_t fmt, uint64_t pts_us,
                                  uint32_t timeout_ms);

/* 向所有节点广播控制指令。 */
aura_err_t aura_pipeline_control(aura_pipeline_t *p, aura_node_cmd_t cmd);

/* 向单个节点下发控制指令。 */
aura_err_t aura_pipeline_control_node(aura_pipeline_t *p, aura_node_t *node, aura_node_cmd_t cmd,
                                      const void *arg);

/* 清空所有队列 + 丢弃在途帧（打断 / 复位用）。 */
aura_err_t aura_pipeline_flush(aura_pipeline_t *p);

/* 等待在途帧处理完毕（测试与优雅停止用）。
 *
 * 注意：参考帧也占帧池。喂了参考却没有喂对应的近端时，该参考会停在节点的
 * 配对槽里等伙伴（见 feed_ref 的 pts 约定），此时本函数**不会**归零，会一直
 * 等到超时 —— 参考不是流水线终点，测试里必须"近端/参考成对喂完再等"。 */
aura_err_t aura_pipeline_wait_drained(aura_pipeline_t *p, uint32_t timeout_ms);

void aura_pipeline_stats(const aura_pipeline_t *p, aura_pipeline_stats_t *out);

/* 打印每个节点的队列深度与耗时统计（长阻塞排查）。 */
void aura_pipeline_dump(const aura_pipeline_t *p);

/* 调用 ops->selftest（无 selftest 的节点视为通过）。 */
aura_err_t aura_pipeline_selftest(aura_pipeline_t *p);

/* ------------------- 供 core 内部与测试使用的帧池接口 ------------------- */

aura_audio_frame_t *aura_pipeline_frame_alloc(aura_pipeline_t *p);
void                aura_pipeline_frame_free(aura_pipeline_t *p, aura_audio_frame_t *frame);

#ifdef __cplusplus
}
#endif

#endif /* AURA_CORE_PIPELINE_PIPELINE_H */
