# tools — 板端调试工具（Phase 2 ★项：各节点 PCM 落盘回放）
#
# audio_debug 与 tests/audio_front 共用同一份实现（wav_io / wavtap / synth_scene /
# metrics）：工具用来人工听评，测试用来断言指标。两者必须跑在**同一条链**、
# **同一套落盘逻辑**上 —— 否则"测试通过但工具里听出毛病"这类偏差没人解释得清。
#
# 公共库放在 tools/ 下而不是 tests/ 下：工具是给人用的，测试是它的自动化影子；
# tests/ 依赖 tools/ 的方向比反过来自然。

add_subdirectory(audio_debug)
