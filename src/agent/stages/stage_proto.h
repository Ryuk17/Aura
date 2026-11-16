/* Aura — Agent 数据流 stage 公共消息协议
 *
 * 约定：
 *   - topic 用宏，禁止在 stage 里手写字符串（写错只会变成"消息没人收"）；
 *   - payload 是 UTF-8 文本，按行组织成 key=value（见 aura_stage_kv_*）；
 *   - 上下文（session/turn/speaker/trace）随消息走，stage 不读全局状态。
 *
 * 这里刻意不引入 JSON：板端解析要小、要能整段读完，且消息 ≤1KB。
 */
#ifndef AURA_AGENT_STAGES_STAGE_PROTO_H
#define AURA_AGENT_STAGES_STAGE_PROTO_H

#include <stdbool.h>
#include <stdint.h>

#include "agent/flow/flow_bus.h"
#include "agent/flow/flow_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- topics */

/* 输入（由 flow_bridge / 上层发布） */
#define AURA_TOPIC_SESSION_WAKE       "session.wake"
#define AURA_TOPIC_SESSION_VOICEPRINT "session.voiceprint"
#define AURA_TOPIC_SESSION_STATE      "session.state"
#define AURA_TOPIC_VAD_START          "vad.start"
#define AURA_TOPIC_VAD_END            "vad.end"
#define AURA_TOPIC_ASR_FINAL          "asr.final"
#define AURA_TOPIC_CONTROL_INTERRUPT  "control.interrupt"

/* 会话输出 */
#define AURA_TOPIC_SESSION_READY      "session.ready"
#define AURA_TOPIC_SESSION_SPEAKER    "session.speaker"
#define AURA_TOPIC_SESSION_CLOSED     "session.closed"
#define AURA_TOPIC_TURN_START         "session.turn.start"
#define AURA_TOPIC_TURN_END           "session.turn.end"

/* 对话 / 记忆 / 工具 / 播报 */
#define AURA_TOPIC_DIALOGUE_INTENT    "dialogue.intent"
#define AURA_TOPIC_MEMORY_WRITE       "memory.write"
#define AURA_TOPIC_MEMORY_STORED      "memory.stored"
#define AURA_TOPIC_TOOL_CALL          "tool.call"
#define AURA_TOPIC_TOOL_RESULT        "tool.result"
#define AURA_TOPIC_LLM_REQUEST        "llm.request"
#define AURA_TOPIC_TTS_SAY            "tts.say"

/* ------------------------------------------------------------- key-value */

#define AURA_STAGE_KV_MAX     8
#define AURA_STAGE_KV_KEY_MAX 32
#define AURA_STAGE_KV_VAL_MAX 256

typedef struct aura_stage_kv {
    char key[AURA_STAGE_KV_KEY_MAX];
    char val[AURA_STAGE_KV_VAL_MAX];
} aura_stage_kv_t;

/* 解析 `<key>=<value>` 行（支持 \r\n、`#` 注释）。返回解析出的条目数。 */
uint32_t aura_stage_kv_parse(const char *payload, aura_stage_kv_t *out, uint32_t max);

/* 取字符串值；不存在返回 NULL。 */
const char *aura_stage_kv_get(const aura_stage_kv_t *kvs, uint32_t n, const char *key);

/* 取整数值；不存在或格式非法返回 false。 */
bool aura_stage_kv_get_int(const aura_stage_kv_t *kvs, uint32_t n, const char *key,
                           int32_t *out);

/* 发布一条消息（自动补 kind/len/topic；ctx 可为 NULL）。 */
aura_err_t aura_stage_publish(aura_flow_bus_t *bus, const char *topic, aura_flow_kind_t kind,
                              const aura_flow_ctx_t *ctx, const char *payload);

#ifdef __cplusplus
}
#endif

#endif /* AURA_AGENT_STAGES_STAGE_PROTO_H */
