/* Aura — 对话状态机（todo.md 4.3）
 *
 *   Idle ──KWS命中──► Listening ──ASR结束──► Thinking ──首包──► Speaking
 *     ▲                   │                      │                 │
 *     │◄────声纹失败/静音超时┘                      │        ┌────┘
 *     └──────────────────────────────── TTS结束 ────┘        │
 *     ▲                                                      │
 *     └──────────── 有效打断（停播 + 中止 LLM） ◄─────────────┘
 *
 * 设计要点：
 *   - **Interrupted 是迁移事件，不是可停留状态**（避免卡死态）。打断 = Speaking 上
 *     的一个触发器，落到 Listening。
 *   - 迁移表是数据，不是 if-else；非法迁移返回 AURA_ERR_STATE 并计数，
 *     便于用测试穷举校验"哪些事件在哪些状态被拒绝"。
 *   - 状态变化会上报 event_bus（AURA_EVENT_STATE_CHANGED），上层据此驱动 TTS 停播、
 *     LLM 中止等动作 —— 状态机自身不做业务。
 *   - Error 状态可按配置自动复位回 Idle。
 */
#ifndef AURA_CORE_STATE_MACHINE_STATE_MACHINE_H
#define AURA_CORE_STATE_MACHINE_STATE_MACHINE_H

#include <stdbool.h>
#include <stdint.h>

#include "core/event_bus/event_bus.h"
#include "osal/osal.h"
#include "utils/error.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AURA_STATE_IDLE = 0,   /* KWS+VAD 常跑；按需模型卸载 */
    AURA_STATE_LISTENING,  /* 已唤醒（声纹通过）；ASR 就绪，等用户说话 */
    AURA_STATE_THINKING,   /* ASR 结束，等 LLM token 流 */
    AURA_STATE_SPEAKING,   /* TTS 播放中；AEC/VAD/KWS 仍运行 */
    AURA_STATE_ERROR,      /* 模块异常；可配置自动复位 */
    AURA_STATE_COUNT
} aura_state_t;

typedef enum {
    AURA_TRIG_NONE = 0,
    AURA_TRIG_START,            /* 启动 → Idle */
    AURA_TRIG_KWS_HIT,          /* 唤醒词命中 */
    AURA_TRIG_VOICEPRINT_OK,    /* 声纹通过（进入 ASR） */
    AURA_TRIG_VOICEPRINT_FAIL,  /* 声纹失败（退回 Idle，不进 ASR） */
    AURA_TRIG_ASR_FINAL,        /* ASR 出最终结果，送 LLM */
    AURA_TRIG_TTS_START,        /* TTS 首包/开始播放 */
    AURA_TRIG_TTS_DONE,         /* 播放结束 */
    AURA_TRIG_BARGE_IN,         /* 有效打断（多因子裁决通过） */
    AURA_TRIG_SILENCE_TIMEOUT,  /* 静音超时（turn_taking 产生） */
    AURA_TRIG_ERROR,            /* 任一模块异常 */
    AURA_TRIG_RESET,            /* 错误复位 */
    AURA_TRIG_COUNT
} aura_trigger_t;

typedef struct aura_sm aura_sm_t;

/* 迁移回调：在**触发者线程**上执行 —— 必须短小（置标志/发消息），
 * 禁止在其中做推理或等待。 */
typedef void (*aura_sm_change_fn)(aura_sm_t *sm, aura_state_t from, aura_state_t to,
                                  aura_trigger_t trig, void *user);

typedef struct aura_sm_cfg {
    bool     auto_reset;       /* Error 后自动回 Idle */
    uint32_t auto_reset_ms;    /* 自动复位延迟 */
    bool     publish_events;   /* 迁移时向 event_bus 发 STATE_CHANGED */
} aura_sm_cfg_t;

/* bus 可为 NULL（只做本地状态跟踪）。 */
aura_sm_t *aura_sm_create(const aura_sm_cfg_t *cfg, aura_event_bus_t *bus);
void       aura_sm_destroy(aura_sm_t *sm);

void        aura_sm_set_change_cb(aura_sm_t *sm, aura_sm_change_fn cb, void *user);
aura_state_t aura_sm_state(const aura_sm_t *sm);

/* 投递一个触发器。返回 AURA_ERR_STATE 表示当前状态不接受该触发器
 * （已计数，可通过 aura_sm_stats 查询）。 */
aura_err_t aura_sm_handle(aura_sm_t *sm, aura_trigger_t trig);

/* 由 event_bus 事件推导触发器（订阅者常用）。无对应触发器返回 AURA_TRIG_NONE。 */
aura_trigger_t aura_sm_trigger_from_event(const aura_event_t *event);

/* 状态/触发器可读名。 */
const char *aura_state_name(aura_state_t state);
const char *aura_trigger_name(aura_trigger_t trig);

typedef struct aura_sm_stats {
    uint64_t transitions;
    uint64_t rejected;      /* 当前状态不接受的触发器次数 */
    uint64_t errors;        /* 进入 Error 次数 */
    uint64_t auto_resets;
} aura_sm_stats_t;

void aura_sm_stats(const aura_sm_t *sm, aura_sm_stats_t *out);

/* 迁移表只读视图（测试用穷举校验）。 */
typedef struct aura_sm_transition {
    aura_state_t   from;
    aura_trigger_t trig;
    aura_state_t   to;
    const char    *desc;
} aura_sm_transition_t;

uint32_t                     aura_sm_transition_count(void);
const aura_sm_transition_t  *aura_sm_transition_table(void);

#ifdef __cplusplus
}
#endif

#endif /* AURA_CORE_STATE_MACHINE_STATE_MACHINE_H */
