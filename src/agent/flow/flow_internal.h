/* Aura — flow 内部结构（bus / stage / middleware 共享，不对外） */
#ifndef AURA_AGENT_FLOW_FLOW_INTERNAL_H
#define AURA_AGENT_FLOW_FLOW_INTERNAL_H

#include "agent/flow/flow_bus.h"
#include "agent/flow/flow_middleware.h"
#include "agent/flow/flow_stage.h"
#include <stdatomic.h>

#include "osal/osal.h"

#ifdef __cplusplus
extern "C" {
#endif

#define AURA_FLOW_MAX_SUBS   64
#define AURA_FLOW_MAX_TOPICS 32

typedef struct aura_flow_sub {
    bool                used;
    char                topic[AURA_FLOW_TOPIC_MAX];
    aura_flow_stage_t  *stage;
} aura_flow_sub_t;

typedef struct aura_flow_topic_seq {
    bool     used;
    char     topic[AURA_FLOW_TOPIC_MAX];
    uint64_t seq;
} aura_flow_topic_seq_t;

struct aura_flow_bus {
    aura_mutex_t *lock;

    uint32_t            default_depth;
    aura_flow_policy_t  default_policy;

    aura_flow_sub_t       subs[AURA_FLOW_MAX_SUBS];
    aura_flow_topic_seq_t topics[AURA_FLOW_MAX_TOPICS];
    aura_flow_mw_entry_t  mw[AURA_FLOW_MW_MAX];
    uint32_t              mw_count;

    uint32_t cancelled_session;
    uint32_t cancelled_turn;

    aura_flow_stats_t stats;
};

struct aura_flow_stage {
    aura_flow_bus_t   *bus;
    char               name[AURA_FLOW_NAME_MAX];
    aura_flow_handler_t handler;
    void              *user;

    aura_queue_t      *q;
    aura_task_t       *task;
    volatile bool      running;

    uint32_t           depth;
    aura_flow_policy_t policy;

    _Atomic uint64_t received;
    _Atomic uint64_t dropped;
    _Atomic uint64_t delivered;
    _Atomic uint64_t errors;
};

#ifdef __cplusplus
}
#endif

#endif /* AURA_AGENT_FLOW_FLOW_INTERNAL_H */
