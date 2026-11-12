/* Aura — event_bus ↔ flow 桥
 *
 * 实时面（event_bus）与 Agent 数据面（flow）不互相直接 include：
 *   event_bus → flow：ASR 结果、KWS/声纹、状态变化、错误、打断；
 *   flow → event_bus：由 Agent stage 通过 aura_event_bus_post 主动发控制事件。
 */
#ifndef AURA_AGENT_FLOW_FLOW_BRIDGE_H
#define AURA_AGENT_FLOW_FLOW_BRIDGE_H

#include "agent/flow/flow_bus.h"
#include "core/event_bus/event_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aura_flow_bridge aura_flow_bridge_t;

aura_flow_bridge_t *aura_flow_bridge_create(aura_flow_bus_t *flow_bus,
                                            aura_event_bus_t *event_bus);
void                aura_flow_bridge_destroy(aura_flow_bridge_t *bridge);

#ifdef __cplusplus
}
#endif

#endif /* AURA_AGENT_FLOW_FLOW_BRIDGE_H */
