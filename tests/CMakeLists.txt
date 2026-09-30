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
	if(AURA_WITH_MNN)
		aura_add_test(test_inference)
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
endif()
