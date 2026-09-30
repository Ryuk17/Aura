/* Aura — 异步事件总线
 *
 * 定位（todo.md 3 / 4.2）：模块之间**不直接互相调用**，一切状态变化、算法结果、
 * 错误都通过事件总线流动；状态机、Agent、以及各 router 都只消费事件。
 *
 * 关键性质：
 *   - `publish` 不阻塞调用者（音频线程安全）：事件拷进队列即返回，队列满则丢弃
 *     并计入 dropped（宁丢事件不卡音频链路）。
 *   - 分发在总线自己的任务里执行；订阅者回调**不得长阻塞**。
 *   - `wait` 供上层/测试做同步等待（不参与生产路径）。
 */
#ifndef AURA_CORE_EVENT_BUS_EVENT_BUS_H
#define AURA_CORE_EVENT_BUS_EVENT_BUS_H

#include <stdbool.h>
#include <stdint.h>

#include "osal/osal.h"
#include "utils/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AURA_EVENT_NONE = 0,
    AURA_EVENT_STATE_CHANGED,     /* code=新状态，payload=状态名 */
    AURA_EVENT_KWS_HIT,           /* u.kws.score */
    AURA_EVENT_VAD_SPEECH_START,
    AURA_EVENT_VAD_SPEECH_END,
    AURA_EVENT_VOICEPRINT_RESULT, /* u.voiceprint.passed / .score */
    AURA_EVENT_ASR_PARTIAL,
    AURA_EVENT_ASR_FINAL,
    AURA_EVENT_LLM_FIRST_TOKEN,
    AURA_EVENT_LLM_TOKEN,
    AURA_EVENT_LLM_DONE,
    AURA_EVENT_TTS_FIRST_CHUNK,
    AURA_EVENT_TTS_DONE,
    AURA_EVENT_PLAYBACK_START,
    AURA_EVENT_PLAYBACK_STOP,
    AURA_EVENT_BARGE_IN,
    AURA_EVENT_SILENCE_TIMEOUT,
    AURA_EVENT_ERROR,             /* code=aura_err_t，payload=描述 */
    AURA_EVENT_USER,              /* 上层自定义，code 由上层约定 */
    AURA_EVENT_TYPE_COUNT
} aura_event_type_t;

#ifndef AURA_EVENT_PAYLOAD_BYTES
#define AURA_EVENT_PAYLOAD_BYTES 256
#endif

typedef struct aura_event {
    aura_event_type_t type;
    uint32_t          seq;     /* 全局递增序号，用于检测丢失 */
    uint64_t          ts_us;   /* 发布时刻 */
    int32_t           code;    /* 错误码 / 结果码 / 状态码 */
    union {
        struct { int32_t  score; } kws;              /* 0–100 */
        struct { int32_t  prob; }  vad;              /* 0–100 */
        struct { bool     passed; int32_t score; } voiceprint; /* score * 100 */
        struct { bool     is_final; uint32_t len; } asr;
        struct { uint32_t tokens; } llm;
        struct { uint64_t pts_us; } audio;
        struct { int32_t  from; int32_t to; } state;
    } u;
    char payload[AURA_EVENT_PAYLOAD_BYTES]; /* 短文本（ASR 结果 / token 片段 / 描述） */
} aura_event_t;

typedef struct aura_event_bus aura_event_bus_t;

/* 订阅回调。在总线分发任务上下文执行，禁止长阻塞，禁止再 publish 造成自环。 */
typedef void (*aura_event_handler_t)(const aura_event_t *event, void *user);

typedef struct aura_event_bus_stats {
    uint64_t published;
    uint64_t dispatched;
    uint64_t dropped;      /* 队列满导致丢弃 */
    uint64_t handler_calls;
} aura_event_bus_stats_t;

aura_event_bus_t *aura_event_bus_create(uint32_t queue_depth);
void              aura_event_bus_destroy(aura_event_bus_t *bus);

/* 订阅 mask 内的事件类型（按位，1u << type）。max_handlers 由实现限制。 */
aura_err_t aura_event_bus_subscribe(aura_event_bus_t *bus, uint32_t type_mask,
                                    aura_event_handler_t handler, void *user);
aura_err_t aura_event_bus_unsubscribe(aura_event_bus_t *bus, aura_event_handler_t handler);

/* 发布，非阻塞；返回 AURA_ERR_FULL 表示队列满已丢弃（调用者通常忽略）。 */
aura_err_t aura_event_bus_publish(aura_event_bus_t *bus, const aura_event_t *event);

/* 发布便捷版：自动填 ts/seq。 */
aura_err_t aura_event_bus_post(aura_event_bus_t *bus, aura_event_type_t type, int32_t code,
                               const char *payload);

/* 同步等待一个匹配事件（测试/上层编排用）。事件同时仍会分发给订阅者。 */
aura_err_t aura_event_bus_wait(aura_event_bus_t *bus, uint32_t type_mask,
                               aura_event_t *out, uint32_t timeout_ms);

/* 丢弃在途事件（打断/复位时用），返回丢弃条数。 */
uint32_t aura_event_bus_flush(aura_event_bus_t *bus);

void aura_event_bus_stats(const aura_event_bus_t *bus, aura_event_bus_stats_t *out);

const char *aura_event_type_name(aura_event_type_t type);

#ifdef __cplusplus
}
#endif

#endif /* AURA_CORE_EVENT_BUS_EVENT_BUS_H */
