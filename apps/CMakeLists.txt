# Aura 示例应用
#
# 规划（见 docs/todo.md 第 2 节）：demo_kws / demo_asr / demo_full_offline /
# demo_cloud_enhanced —— 每个都是完整可烧录 demo。
#
# Phase A/B 尚无应用：当前验收载体是 tests/host_sim 与 tests/soundproof_chain；
# 上板 demo 随 Phase B（Agent 数据流）/ Phase C（全双工播放）逐条补齐。
#
# 新增应用时的写法：
#   add_executable(demo_xxx demo_xxx/main.c)
#   target_link_libraries(demo_xxx PRIVATE aura_agent)
