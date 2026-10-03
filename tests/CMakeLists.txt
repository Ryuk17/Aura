# Aura 测试 —— unit（PC 上跑）+ host_sim（PC 全链路仿真）
#
# 每个单测一个可执行文件：失败定位直接，也让 CTest 可以并行跑。

# 通用：测试都链接核心库
function(aura_add_test name)
	add_executable(${name} unit/${name}.c)
	target_link_libraries(${name} PRIVATE aura_agent aura_core aura_utils aura_platform)
	target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
	# 注入源码根目录：测试从任意 cwd 运行都能定位 models/（模型不入 git）
	target_compile_definitions(${name} PRIVATE
		AURA_HOST_SIM_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
	)
	add_test(NAME ${name} COMMAND ${name})
	set_tests_properties(${name} PROPERTIES TIMEOUT 60)
endfunction()

if(AURA_BUILD_TESTS)
	aura_add_test(test_osal)
	aura_add_test(test_ringbuf)
	aura_add_test(test_mem_pool)
	aura_add_test(test_event_bus)
	aura_add_test(test_state_machine)
	aura_add_test(test_pipeline)
	aura_add_test(test_algorithm)
	# DSP 适配模板单测：只依赖 aura_dsp（引擎无关），不需要 TrickRoom
	add_executable(test_dsp_adapter unit/test_dsp_adapter.c)
	target_link_libraries(test_dsp_adapter PRIVATE aura_dsp aura_core aura_utils aura_platform)
	target_include_directories(test_dsp_adapter PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
	add_test(NAME test_dsp_adapter COMMAND test_dsp_adapter)
	set_tests_properties(test_dsp_adapter PROPERTIES TIMEOUT 60)
	if(AURA_WITH_MNN)
		aura_add_test(test_inference)
		# NN 适配模板单测：用**无模型算法**（desc 不声明 model_file）驱动适配层 ——
		# 滑窗 / 透传与旁路 / 标志边沿 / 控制面 / 统计，都不需要真模型即可验证。
		# 真模型（silero VAD）的接入由 host_sim --use-silero 覆盖。
		aura_add_test(test_nn_adapter)
		target_link_libraries(test_nn_adapter PRIVATE aura_algorithm)
	endif()
endif()

# host_sim：文件喂音频驱动全链路（Phase 1 出口验收）
if(AURA_BUILD_HOST_SIM)
	add_executable(host_sim
		host_sim/host_sim.c
		host_sim/wav_io.c
	)
	target_link_libraries(host_sim PRIVATE aura_agent aura_core aura_utils aura_platform)
	if(AURA_WITH_MNN)
		target_link_libraries(host_sim PRIVATE aura_inference)
	endif()
	target_include_directories(host_sim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
	target_compile_definitions(host_sim PRIVATE
		AURA_HOST_SIM_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
	)
	add_test(NAME host_sim COMMAND host_sim)
	set_tests_properties(host_sim PROPERTIES TIMEOUT 180)

	# silero 变体：真实 NN 模型经**算法链**装配（--use-silero 注册算法描述表，
	# .conf 里的 chain = silero_vad 决定拓扑）—— 同一套接口对 NN 族成立的证据。
	# 与上一条共用 cwd 里的合成输入文件，故声明 RUN_SERIAL（-j 时也不并跑）。
	if(AURA_WITH_MNN)
		add_test(NAME host_sim_silero COMMAND host_sim --use-silero
			--config ${CMAKE_CURRENT_SOURCE_DIR}/host_sim/configs/silero.conf
			--out ${CMAKE_CURRENT_BINARY_DIR}/host_sim_silero_out.wav)
		set_tests_properties(host_sim_silero PROPERTIES TIMEOUT 180 RUN_SERIAL TRUE)
	endif()
endif()
