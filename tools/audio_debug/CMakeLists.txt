# audio_debug — Phase 2 语音前端调试工具（读 WAV → 真链 → 各节点 PCM 落盘）
#
# 有两类使用者，都以本目录的实现为准：
#   人   —— 命令行跑一遍，把 mic/ref/aec3_out/bf_out/ns_out/agc_out/src_out 全落成
#           WAV，用耳朵判断"回声消干净没有、有没有爆音"（todo.md Phase 2 的 ★项）；
#   机器 —— tests/audio_front 下的 CTest 直接链接下面的公共库，用同一套落盘与统计
#           做客观断言。
# 共用实现是刻意的：工具里听得出的毛病，测试里必须也能断言。分成两套实现的话，
# "测试全绿但一听就废"就没有人能解释得清。

add_library(aura_audio_debug_common STATIC
	wav_io.c      # 多通道 WAV 读 + 流式写（与 host_sim 那份单声道实现无关）
	metrics.c     # 能量/分贝/比值/削波
	wavtap.c      # 观察者节点：统计 + 逐节点落盘
	synth_scene.c # 合成回声场景（确定性，供无麦克风时验证）
	front_probe.c # 探针链重写 + 喂帧节奏（工具与 CTest 共用，只有这一份）
)
target_include_directories(aura_audio_debug_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
# wavtap 是货真价实的 pipeline 节点（注册进算法注册表），所以这层依赖 core。
# 链上不出现 TrickRoom 时它照样能用（单测里的 mock 链）。
target_link_libraries(aura_audio_debug_common PUBLIC aura_core)

# 上面的公共库无条件建（tests/audio_front 要用）；可执行文件才跟着开关走。
if(AURA_WITH_TRICKROOM AND AURA_BUILD_TOOLS)
	add_executable(audio_debug audio_debug.c)
	target_link_libraries(audio_debug PRIVATE aura_audio_debug_common aura_dsp_trickroom)
elseif(NOT AURA_WITH_TRICKROOM)
	# 没有 TrickRoom 时链上一个真算法都造不出来，这个可执行文件没有存在意义。
	message(STATUS "Aura: AURA_WITH_TRICKROOM=OFF —— 不构建 audio_debug 可执行文件")
endif()
