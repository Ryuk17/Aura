/* Aura — Pipeline 引擎
 *
 * 职责：节点注册与连线、每节点一条任务、帧池管理、pts 维护、错误上抛、统计。
 *
 * 拓扑（Phase 1）：**线性链**，一个节点最多一个下游。多路音频（多麦拆分 /
 * 参考通道）通过 `audio_stream` 字段区分，暂不做分支 —— 需要分支时扩展 link 接口。
 *
 * pts 维护：`aura_pipeline_feed` 由调用方给出首帧 pts；此后 pipeline 按
 * `帧长 / 采样率` 自动推进下游 pts，保证 AEC 参考与多麦通路严格对齐（todo.md 4.2）。
 * 节点**不得**自行改写 pts —— 需要改变时间轴（如重采样）时用 audio_stream 标记。
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
    uint32_t frame_pool_blocks;  /* 帧池块数，默认 16（每块含 MAX_BYTES 样本区） */
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
    uint64_t node_errors;
} aura_pipeline_stats_t;

typedef struct aura_pipeline aura_pipeline_t;

void aura_pipeline_config_default(aura_pipeline_cfg_t *cfg);

aura_pipeline_t *aura_pipeline_create(const aura_pipeline_cfg_t *cfg);
void             aura_pipeline_destroy(aura_pipeline_t *p);

/* 绑定外部 event_bus；不调用则由 pipeline 自建一个（destroy 时一并释放）。 */
aura_err_t aura_pipeline_attach_bus(aura_pipeline_t *p, aura_event_bus_t *bus);
aura_event_bus_t *aura_pipeline_bus(aura_pipeline_t *p);

/* 注册节点。节点结构体在 pipeline 生命周期内必须保持有效。 */
aura_err_t aura_pipeline_add(aura_pipeline_t *p, aura_node_t *node);

/* 连线：up 的音频产出送往 down。up->caps.produces_audio 必须为 true。
 * 未连线时，节点的音频产出按注册顺序自动接到下一个消费音频的节点（隐式线性）。 */
aura_err_t aura_pipeline_link(aura_pipeline_t *p, aura_node_t *up, aura_node_t *down);

/* 同上，用于文本流。 */
aura_err_t aura_pipeline_link_text(aura_pipeline_t *p, aura_node_t *up, aura_node_t *down);

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

/* 向所有节点广播控制指令。 */
aura_err_t aura_pipeline_control(aura_pipeline_t *p, aura_node_cmd_t cmd);

/* 向单个节点下发控制指令。 */
aura_err_t aura_pipeline_control_node(aura_pipeline_t *p, aura_node_t *node, aura_node_cmd_t cmd,
                                      const void *arg);

/* 清空所有队列 + 丢弃在途帧（打断 / 复位用）。 */
aura_err_t aura_pipeline_flush(aura_pipeline_t *p);

/* 等待在途帧处理完毕（测试与优雅停止用）。 */
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
