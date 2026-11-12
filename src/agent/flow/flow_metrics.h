/* Aura — Agent 数据流指标/诊断 */
#ifndef AURA_AGENT_FLOW_FLOW_METRICS_H
#define AURA_AGENT_FLOW_FLOW_METRICS_H

#include "agent/flow/flow_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 打印总线统计（publish/drop/cancel/error/dispatched）。 */
void aura_flow_report(const aura_flow_bus_t *bus);

#ifdef __cplusplus
}
#endif

#endif /* AURA_AGENT_FLOW_FLOW_METRICS_H */
