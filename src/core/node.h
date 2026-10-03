/* Aura — Pipeline 节点契约（todo.md 4.2 的落地）
 *
 * 三条硬约束，所有节点必须遵守：
 *
 *  1. **推模式**：上游把帧推给下游，节点不主动拉数据。
 *  2. **process 内禁止长阻塞**：大推理（ASR/LLM/TTS）必须在节点内部抛子 task，
 *     process 只做入队 —— 推理不得阻塞音频帧搬运。
 *  3. **错误上抛**：节点不得 exit()/崩溃退出；异常一律发 event_bus 错误事件，
 *     由状态机迁入 Error。pipeline 检测到 process 返回错误时也会代为上抛。
 *
 * 线程模型：pipeline 为每个节点创建一条任务，任务循环从输入队列取帧并调用
 * process_audio/process_text。因此"process 内禁止长阻塞"的代价只落在该节点
 * 自身（表现为上游队列积压 + 丢帧统计），不会拖垮整条音频链路。
 *
 * 数据生命周期：回调收到的 frame/chunk 与其内部指针**仅在本次回调内有效**。
 * 节点若需保留（如攒够一窗再送模型），必须自行深拷贝到自己的缓冲。
 */
#ifndef AURA_CORE_NODE_H
#define AURA_CORE_NODE_H

#include <stdbool.h>
#include <stdint.h>

#include "core/event_bus/event_bus.h"
#include "core/msg/audio_frame.h"
#include "core/msg/text_chunk.h"
#include "osal/osal.h"
#include "utils/error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 一个节点最多向几个下游扇出音频（见 aura_node::next_audio）。
 * 4 = 主线 1 + 旁路观察 3，覆盖 VAD/KWS/audio_debug 同时旁挂的极端情况。 */
#define AURA_NODE_MAX_FANOUT 4

typedef struct aura_node      aura_node_t;
typedef struct aura_pipeline  aura_pipeline_t;
typedef struct aura_event_bus aura_event_bus_t;

/* 节点能力声明：pipeline 据此校验连线合法性，并决定创建哪些队列。 */
typedef struct aura_node_caps {
    bool consumes_audio;  /* 消费音频帧 */
    bool consumes_text;   /* 消费文本 chunk */
    bool produces_audio;  /* 会向下游产出音频帧 */
    bool produces_text;   /* 会向下游产出文本 chunk */
    bool wants_events;    /* 注册时自动订阅 event_bus（消费事件，如 barge_in/VAD） */
    /* 消费 AEC 参考流（播放回采）：帧从 aura_pipeline_feed_ref() 进，走 in_ref 队列，
     * 与主音频流是两个物理时钟源，**不保证帧数与 pts 与主链对齐** —— 节点自己
     * 按 pts 对齐（AEC 适配器内做参考环缓冲），pipeline 只负责投递。 */
    bool consumes_ref_audio;
    /* 本节点会改变音频流形状（采样率/帧长），如 SRC 重采样。
     * 置位后 aura_node_emit_audio **不再改写 pts**（见 struct aura_node 的 pts 契约），
     * 由节点自算输出 pts。全链至多一个此类节点（chain 校验强制）。 */
    bool changes_frame_rate;
} aura_node_caps_t;

/* 控制面指令：上/下游不直接调用对方函数，通过 pipeline 下发。 */
typedef enum {
    AURA_CMD_START = 0,      /* 进入工作状态 */
    AURA_CMD_STOP,           /* 停止工作（保留资源） */
    AURA_CMD_RESET,          /* 清空内部状态（缓存/滑窗/KV） */
    AURA_CMD_FLUSH,          /* 丢弃在途数据（打断时用） */
    AURA_CMD_SET_CONFIG,     /* arg = 配置结构，语义由节点自定 */
} aura_node_cmd_t;

typedef struct aura_node_ops {
    /* 注册后、启动前调用一次；用于分配节点自有资源。可为空。 */
    aura_err_t (*init)(aura_node_t *self);
    /* 启动：节点自己的子 task / 加载模型等。可为空。 */
    aura_err_t (*start)(aura_node_t *self);
    /* 停止：释放子 task；必须可重入（start/stop 可多次）。可为空。 */
    aura_err_t (*stop)(aura_node_t *self);
    /* 处理一帧音频。**禁止长阻塞**。可空（不消费音频的节点）。 */
    aura_err_t (*process_audio)(aura_node_t *self, const aura_audio_frame_t *frame);
    /* 处理一帧 AEC 参考音频（caps.consumes_ref_audio 为 true 时生效）。
     * 与 process_audio 同线程同语义，但**不参与 pts 契约**（参考流是独立时钟源，
     * 不设 last_in_pts）—— 节点自己按 pts 与主链对齐。可空。 */
    aura_err_t (*process_ref_audio)(aura_node_t *self, const aura_audio_frame_t *frame);
    /* 处理一个文本 chunk。**禁止长阻塞**。可空。 */
    aura_err_t (*process_text)(aura_node_t *self, const aura_text_chunk_t *chunk);
    /* 控制面。可空。 */
    aura_err_t (*control)(aura_node_t *self, aura_node_cmd_t cmd, const void *arg);
    /* 事件消费（caps.wants_events 为 true 时生效）。
     * 在 event_bus 分发任务上下文执行 —— **禁止长阻塞**，只做标记/入队。 */
    void (*on_event)(aura_node_t *self, const aura_event_t *event);
    /* 销毁前调用一次。可空。 */
    void (*deinit)(aura_node_t *self);
    /* 自检（单测/上板自检用）。返回 AURA_OK 表示通过。可空。 */
    aura_err_t (*selftest)(aura_node_t *self);
} aura_node_ops_t;

/* 节点运行统计 —— 用于定位"推理拖慢链路"这类问题（配合 profiler）。 */
typedef struct aura_node_stats {
    uint64_t processed;        /* 成功处理的帧/chunk 数 */
    uint64_t dropped;          /* 因下游队列满被丢弃的产出数 */
    uint64_t errors;           /* process 返回错误次数 */
    uint64_t process_us_total; /* 累计处理耗时 */
    uint64_t process_us_max;   /* 单次处理最大耗时（长阻塞的证据） */
} aura_node_stats_t;

/* 节点基类。具体节点把本结构**放在自己结构体首位**，即可被 pipeline 调度：
 *
 *   typedef struct { aura_node_t base; my_state_t s; } my_node_t;
 *   static const aura_node_ops_t my_ops = { ... };
 *   aura_node_t *my_create(void) {
 *       my_node_t *n = calloc(1, sizeof(*n));
 *       aura_node_init(&n->base, "my", &my_ops, &(aura_node_caps_t){...});
 *       return &n->base;
 *   }
 */
struct aura_node {
    const char           *name;
    aura_node_caps_t      caps;
    const aura_node_ops_t *ops;

    /* 下游连线（由 pipeline 在 link/start 时填充）。
     * next_audio 是**扇出数组**：emit_audio 向每一路各拷一份帧。多数节点只有
     * next_audio[0]（线性主线）；旁路观察者（VAD/KWS/audio_debug）挂在 1..n。
     * 这替代了早期"单指针 + 观察者截断下游"的写法（见 pipeline.md 拓扑节）。 */
    aura_node_t *next_audio[AURA_NODE_MAX_FANOUT];
    uint32_t     next_audio_count;
    aura_node_t *next_text; /* 文本仍为单下游：文本控制面没有旁路观察场景 */

    /* 由 pipeline 在注册时填充，节点自身只读。 */
    aura_pipeline_t  *pipeline;
    aura_event_bus_t *bus;
    aura_queue_t     *in_audio;   /* caps.consumes_audio 时有效 */
    aura_queue_t     *in_text;    /* caps.consumes_text 时有效 */
    aura_queue_t     *in_ref;     /* caps.consumes_ref_audio 时有效（AEC 参考流） */
    int               audio_stream; /* 所属音频流 id（多路音频时区分，0 为默认流） */

    /* 运行期 */
    aura_task_t *task;
    volatile bool running;
    void         *priv;  /* 具体节点的私有数据 */

    /* pts 契约：pipeline 在处理输入帧前置为有效，处理后失效。
     * 节点产出音频时不必自己算 pts，产出接口会以该值覆盖（todo.md 4.2）。
     *
     * 例外：caps.changes_frame_rate 的节点（SRC）产出帧的 pts 不被覆盖 ——
     * 输入输出时基不同，覆盖就会把重采样后的时间轴压回输入时基。此类节点
     * 自算：pts_out = 输入 pts + 已消费样本数 × 1e6 / 输入采样率，
     * 并把输出帧头的 sample_rate 改写为输出采样率。
     *
     * 这两个字段只在**节点自己的 task 的 process 回调期间**有效且非线程安全：
     * 节点的子任务/事件回调里不得调用 emit_audio（那属于 process 之外的上下文）。 */
    uint64_t last_in_pts_us;
    bool     last_in_pts_valid;

    aura_node_stats_t stats;
};

/* 初始化节点基类。所有节点创建函数都应调用。 */
void aura_node_init(aura_node_t *node, const char *name, const aura_node_ops_t *ops,
                    const aura_node_caps_t *caps);

/* ------- 供节点回调内部使用的产出接口（等价于"向下游推帧"） ------- */

/* 产出音频帧给下游（**扇出**：每个下游各拿一份独立的池帧拷贝）。
 * 内部会从 pipeline 帧池取缓冲并拷贝 data。
 * 下游队列满时有界等待（10ms = 一帧时长）再放弃 —— 既不让"process 内禁止长阻塞"
 * 被破坏，也避免正常调度抖动造成丢帧；只有下游真卡死才丢，计入 dropped 统计
 * 并返回 AURA_ERR_FULL。
 *
 * 扇出任一路失败不影响其余路：主线（next_audio[0]）丢帧才是真丢帧，旁路观察者
 * 丢帧只降其统计。返回值为"至少一路失败"的信号，调用方通常无需特殊处理。 */
aura_err_t aura_node_emit_audio(aura_node_t *self, const aura_audio_frame_t *frame);

/* 产出文本 chunk 给下游。 */
aura_err_t aura_node_emit_text(aura_node_t *self, const aura_text_chunk_t *chunk);

/* 发布事件到 event_bus（非阻塞；节点不关心谁消费）。 */
aura_err_t aura_node_emit_event(aura_node_t *self, const aura_event_t *event);

/* 便捷：发布一个只带类型/码值的简单事件。 */
aura_err_t aura_node_post_event(aura_node_t *self, int type, int code);

/* 节点发生错误时调用：把错误上抛到 event_bus 并计入统计。
 * 节点**不得**因错误而退出进程或阻塞。 */
aura_err_t aura_node_report_error(aura_node_t *self, aura_err_t err, const char *what);

/* 本节点当前积压帧数（配合 flush / 自检）。 */
uint32_t aura_node_pending(const aura_node_t *self);

/* 该节点是否运行在**当前线程**（用于断言回调不在音频线程执行等约束）。 */
bool aura_node_is_caller_thread(const aura_node_t *self);

#ifdef __cplusplus
}
#endif

#endif /* AURA_CORE_NODE_H */
