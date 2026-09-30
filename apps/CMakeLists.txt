# Aura 示例应用
#
# 规划（见 docs/todo.md 第 2 节）：demo_kws / demo_asr / demo_full_offline /
# demo_cloud_enhanced —— 每个都是完整可烧录 demo。
#
# Phase 1 尚无应用：本阶段的验收载体是 tests/host_sim（PC 仿真跑通全链路），
# 上板 demo 随 Phase 3（KWS/ASR）与 Phase 4（对话闭环）逐条补齐。
#
# 新增应用时的写法：
#   add_executable(demo_xxx demo_xxx/main.c)
#   target_link_libraries(demo_xxx PRIVATE aura_agent)
