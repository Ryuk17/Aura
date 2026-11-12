# Aura 测试 —— unit（核心/数据流）+ host_sim（状态机闭环）+ soundproof 适配链
#
# 算法本身的正确性由 soundproof 仓库的 test_ae_* 覆盖；这里只测：
#   1. Aura 核心骨架（pipeline/event_bus/状态机/注册表）；
#   2. Agent 数据流中间件（路由/背压/中间件/取消）；
#   3. 真实 soundproof 适配层（VAD，model-free）+ host_sim 状态机闭环。

function(aura_add_test name)
	add_executable(${name} unit/${name}.c)
	target_link_libraries(${name} PRIVATE
		aura_agent aura_flow aura_core aura_utils aura_platform)
	target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
	target_compile_definitions(${name} PRIVATE
		AURA_HOST_SIM_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
	)
	add_test(NAME ${name} COMMAND ${name})
	set_tests_properties(${name} PROPERTIES TIMEOUT 60 LABELS aura)
endfunction()

if(AURA_BUILD_TESTS)
	aura_add_test(test_osal)
	aura_add_test(test_ringbuf)
	aura_add_test(test_mem_pool)
	aura_add_test(test_event_bus)
	aura_add_test(test_state_machine)
	aura_add_test(test_pipeline)
	aura_add_test(test_algorithm)
	aura_add_test(test_flow)
endif()

# host_sim：文件喂音频驱动全链路（Phase 1 出口验收，mock 节点，无模型依赖）
if(AURA_BUILD_HOST_SIM)
	add_executable(host_sim
		host_sim/host_sim.c
		host_sim/wav_io.c
	)
	target_link_libraries(host_sim PRIVATE
		aura_agent aura_flow aura_core aura_utils aura_platform)
	target_include_directories(host_sim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
	target_compile_definitions(host_sim PRIVATE
		AURA_HOST_SIM_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
	)
	add_test(NAME host_sim COMMAND host_sim)
	set_tests_properties(host_sim PROPERTIES TIMEOUT 180 LABELS aura)
endif()

# ------------------------------------------------- soundproof 适配层（真实算法）
# VAD WebRTC 模式不需要模型：用 soundproof 自带语音素材驱动 chain=vad，
# 断言 SPEECH_START/END 事件。这是“Aura 适配层 + 注册表 + pipeline”的闭环证据。
if(AURA_WITH_SOUNDPROOF)
	add_executable(aura_test_ae_vad
		soundproof_chain/test_ae_vad.c
		host_sim/wav_io.c
	)
	target_link_libraries(aura_test_ae_vad PRIVATE
		aura_agent aura_soundproof aura_flow aura_core aura_utils aura_platform)
	target_include_directories(aura_test_ae_vad PRIVATE
		${CMAKE_CURRENT_SOURCE_DIR}
		${CMAKE_CURRENT_SOURCE_DIR}/host_sim)
	target_compile_definitions(aura_test_ae_vad PRIVATE
		AURA_SOUNDPROOF_SOURCE_DIR="${soundproof_SOURCE_DIR}"
	)
	add_test(NAME aura_test_ae_vad COMMAND aura_test_ae_vad)
	set_tests_properties(aura_test_ae_vad PROPERTIES TIMEOUT 120 RUN_SERIAL TRUE LABELS aura)
endif()

# 全套串行：实时节奏的音频链与事件边沿断言对 CPU 争抢敏感。
get_property(_aura_all_tests DIRECTORY PROPERTY TESTS)
if(_aura_all_tests)
	set_tests_properties(${_aura_all_tests} PROPERTIES RUN_SERIAL TRUE)
endif()
