/* Aura — Agent 数据流中间件：消息模型
 *
 * 三平面设计（见 docs/agent_dataflow.md）：
 *   音频实时面（core/pipeline）—— PCM、pts、可丢帧；
 *   Agent 数据面（aura_flow）—— 会话/轮次/文本/工具/记忆，有序、可背压、可取消；
 *   控制/观察面（event_bus）—— 状态、错误、底层算法事件。
 *
 * 本层**不搬 PCM**。消息按值穿过队列，payload 内联，运行期不 malloc。
 */
#ifndef AURA_AGENT_FLOW_FLOW_TYPES_H
#define AURA_AGENT_FLOW_FLOW_TYPES_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AURA_FLOW_TOPIC_MAX 64
#define AURA_FLOW_PAYLOAD_MAX 1024
#define AURA_FLOW_NAME_MAX 32

typedef enum {
    AURA_FLOW_ASR_PARTIAL = 0,
    AURA_FLOW_ASR_FINAL,
    AURA_FLOW_TEXT,
    AURA_FLOW_TURN,
    AURA_FLOW_TOOL_CALL,
    AURA_FLOW_TOOL_RESULT,
    AURA_FLOW_MEMORY_OP,
    AURA_FLOW_CONTROL,
    AURA_FLOW_EVENT,
    AURA_FLOW_ERROR,
    AURA_FLOW_METRIC,
} aura_flow_kind_t;

typedef enum {
    AURA_FLOW_FINAL = 1u << 0,
    AURA_FLOW_EOS   = 1u << 1,
    AURA_FLOW_ABORT = 1u << 2,
    AURA_FLOW_RETRY = 1u << 3,
} aura_flow_flag_t;

typedef enum {
    AURA_FLOW_DROP_OLDEST = 0, /* 音频/中间结果：保最新 */
    AURA_FLOW_DROP_NEWEST,     /* 指标/日志：保稳定 */
    AURA_FLOW_BLOCK_TIMEOUT,   /* 控制/工具结果：有限等待 */
    AURA_FLOW_SPILL,           /* 记忆/审计：落盘后继续（当前等价 DROP_OLDEST） */
} aura_flow_policy_t;

typedef struct aura_flow_ctx {
    uint32_t session_id;
    uint32_t turn_id;
    uint32_t speaker_id;
    uint32_t trace_id;
    uint64_t start_pts_us;
    uint64_t deadline_us;
} aura_flow_ctx_t;

typedef struct aura_flow_msg {
    uint32_t        kind;  /* aura_flow_kind_t */
    uint32_t        flags; /* aura_flow_flag_t 位或 */
    uint64_t        seq;   /* 每 topic 单调递增 */
    uint64_t        ts_us; /* 产生时刻 */
    aura_flow_ctx_t ctx;
    char            topic[AURA_FLOW_TOPIC_MAX];
    uint32_t        len;
    char            payload[AURA_FLOW_PAYLOAD_MAX];
} aura_flow_msg_t;

typedef struct aura_flow_stats {
    uint64_t published;
    uint64_t dispatched;
    uint64_t dropped;
    uint64_t cancelled;
    uint64_t errors;
    uint64_t queue_depth; /* 当前所有 stage 队列的积压和 */
} aura_flow_stats_t;

const char *aura_flow_kind_name(aura_flow_kind_t kind);
const char *aura_flow_policy_name(aura_flow_policy_t policy);

#ifdef __cplusplus
}
#endif

#endif /* AURA_AGENT_FLOW_FLOW_TYPES_H */
