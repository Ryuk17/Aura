/* Aura — Agent 数据流指标/诊断（实现） */
#include "agent/flow/flow_metrics.h"

#include "utils/logger/logger.h"

void aura_flow_report(const aura_flow_bus_t *bus)
{
    aura_flow_stats_t st;
    aura_flow_stats(bus, &st);
    AURA_LOGI("flow", "published=%llu dispatched=%llu dropped=%llu cancelled=%llu errors=%llu",
              (unsigned long long)st.published, (unsigned long long)st.dispatched,
              (unsigned long long)st.dropped, (unsigned long long)st.cancelled,
              (unsigned long long)st.errors);
}
