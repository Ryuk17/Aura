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
	# RUN_SERIAL：VAD 边沿断言依赖事件分发的时序，与 MNN 模型测试并行时
	# CPU 被抢占会滞后一帧（实测 frames_in 2 != 3），单跑必过（audio_front 同类）。
	set_tests_properties(test_dsp_adapter PROPERTIES TIMEOUT 60 RUN_SERIAL TRUE)

	# zipformer 辅助件（context_graph/symbol_table）自检：纯 C 零依赖，不拖 MNN。
	add_executable(test_zipformer_util unit/test_zipformer_util.c)
	target_link_libraries(test_zipformer_util PRIVATE
		aura_zipformer_util aura_utils aura_platform)
	target_include_directories(test_zipformer_util PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
	if(NOT MSVC)
		# 与 test_fbank/test_zipformer 同理：aura_test.h 的 constructor 优先级告警
		# 与 sleep_ms 未引用。
		target_compile_options(test_zipformer_util PRIVATE
			-Wno-prio-ctor-dtor -Wno-unused-function)
	endif()
	add_test(NAME test_zipformer_util COMMAND test_zipformer_util)
	set_tests_properties(test_zipformer_util PROPERTIES TIMEOUT 60)

	if(AURA_WITH_NEON_FFT)
		# fbank 前端自检。测试本身是 C++（FeaturePipeline 的 C 封装还没到用武之地，
		# 先直接测 C++ 实现），不能走 aura_add_test 的 .c 假设，单独写。
		add_executable(test_fbank unit/test_fbank.cpp)
		target_link_libraries(test_fbank PRIVATE aura_feature aura_fft aura_utils aura_platform)
		target_include_directories(test_fbank PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
		# aura_test.h 是给 .c 写的（constructor 优先级告警是 C++ 独有的；
		# 本文件不用 aura_test_sleep_ms）。框架本身不动，在这里点掉。
		if(NOT MSVC)
			target_compile_options(test_fbank PRIVATE -Wno-prio-ctor-dtor -Wno-unused-function)
		endif()
		add_test(NAME test_fbank COMMAND test_fbank)
		set_tests_properties(test_fbank PROPERTIES TIMEOUT 60)
	endif()
	if(AURA_WITH_MNN)
		aura_add_test(test_inference)
		# NN 适配模板单测：用**无模型算法**（desc 不声明 model_file）驱动适配层 ——
		# 滑窗 / 透传与旁路 / 标志边沿 / 控制面 / 统计，都不需要真模型即可验证。
		# 真模型（silero VAD）的接入由 host_sim --use-silero 覆盖。
		aura_add_test(test_nn_adapter)
		target_link_libraries(test_nn_adapter PRIVATE aura_algorithm)

		if(AURA_WITH_NEON_FFT)
			# zipformer2 引擎冒烟：模型在 models/（不入库）时真跑，缺失 SKIP。
			add_executable(test_zipformer unit/test_zipformer.c)
			target_link_libraries(test_zipformer PRIVATE
				aura_zipformer aura_inference aura_utils aura_platform)
			target_include_directories(test_zipformer PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
			target_compile_definitions(test_zipformer PRIVATE
				AURA_HOST_SIM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
			# AURA_TEST 的 constructor 优先级取 __LINE__，本文件第一个测试在 100 行
			# 以内 → prio-ctor-dtor 告警；sleep_ms 未被引用。与 test_fbank 同理点掉。
			if(NOT MSVC)
				target_compile_options(test_zipformer PRIVATE
					-Wno-prio-ctor-dtor -Wno-unused-function)
			endif()
			add_test(NAME test_zipformer COMMAND test_zipformer)
			set_tests_properties(test_zipformer PROPERTIES TIMEOUT 120)

			# KWS 唤醒链（真模型 + 模型自带音频）：行为级验收，见 kws_chain/。
			# RUN_SERIAL：真模型推理 + 实时节奏喂帧，避开并行 CPU 争抢（audio_front 同款）。
			add_executable(test_kws_chain kws_chain/test_kws_chain.c)
			target_link_libraries(test_kws_chain PRIVATE
				aura_kws aura_algorithm aura_inference aura_core aura_utils aura_platform)
			target_include_directories(test_kws_chain PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
			target_compile_definitions(test_kws_chain PRIVATE
				AURA_HOST_SIM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
			if(NOT MSVC)
				target_compile_options(test_kws_chain PRIVATE
					-Wno-prio-ctor-dtor -Wno-unused-function)
			endif()
			add_test(NAME test_kws_chain COMMAND test_kws_chain)
			set_tests_properties(test_kws_chain PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE)

			# ASR 流式识别链（真模型 + 语音素材）：partial/final 流 + 文本面断言。
			add_executable(test_asr_chain asr_chain/test_asr_chain.c)
			target_link_libraries(test_asr_chain PRIVATE
				aura_asr aura_algorithm aura_inference aura_core aura_utils aura_platform)
			target_include_directories(test_asr_chain PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
			target_compile_definitions(test_asr_chain PRIVATE
				AURA_HOST_SIM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
			if(NOT MSVC)
				target_compile_options(test_asr_chain PRIVATE
					-Wno-prio-ctor-dtor -Wno-unused-function)
			endif()
			add_test(NAME test_asr_chain COMMAND test_asr_chain)
			set_tests_properties(test_asr_chain PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE)

			# 声纹注册/验证链（真模型 + 语音素材）。
			add_executable(test_voiceprint voiceprint/test_voiceprint.c)
			target_link_libraries(test_voiceprint PRIVATE
				aura_voiceprint aura_algorithm aura_inference aura_core aura_utils
				aura_platform)
			target_include_directories(test_voiceprint PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
			target_compile_definitions(test_voiceprint PRIVATE
				AURA_HOST_SIM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
			if(NOT MSVC)
				target_compile_options(test_voiceprint PRIVATE
					-Wno-prio-ctor-dtor -Wno-unused-function)
			endif()
			add_test(NAME test_voiceprint COMMAND test_voiceprint)
			set_tests_properties(test_voiceprint PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE)

			# Phase 3 全链路串通（真模型 ×4 + 状态机）。
			add_executable(test_voice_chain voice_chain/test_voice_chain.c)
			target_link_libraries(test_voice_chain PRIVATE
				aura_agent aura_silero aura_kws aura_voiceprint aura_asr
				aura_algorithm aura_inference aura_core aura_utils aura_platform)
			target_include_directories(test_voice_chain PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
			target_compile_definitions(test_voice_chain PRIVATE
				AURA_HOST_SIM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
			if(NOT MSVC)
				target_compile_options(test_voice_chain PRIVATE
					-Wno-prio-ctor-dtor -Wno-unused-function)
			endif()
			add_test(NAME test_voice_chain COMMAND test_voice_chain)
			set_tests_properties(test_voice_chain PROPERTIES TIMEOUT 600 RUN_SERIAL TRUE)
		endif()
	endif()
endif()

# ------------------------------------------------- 语音前端（Phase 2，需真算法）
# 这两条测的是**真算法链**（aec3,bf,ns,agc,src），链装配/喂帧/落盘全部来自
# tools/audio_debug 的公共库 —— 工具里听得出的毛病，这里必须也能断言。
# 没有 TrickRoom 就一个真算法都造不出来，故整体门控在 TR 上。
if(AURA_WITH_TRICKROOM)
	# out_dir 每条测试各给一个：两条测试的链同名，落盘文件名也一样，
	# 共用目录会在并行跑时互相覆盖。
	function(aura_add_front_test name out_dir)
		add_executable(${name} audio_front/${name}.c)
		target_link_libraries(${name} PRIVATE
			aura_audio_debug_common aura_dsp_trickroom
			aura_core aura_utils aura_platform)
		# tests/ 在 include 路径上：框架在 tests/unit/ 下，两个测试按
		# "unit/aura_test.h" 引它。
		target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
		target_compile_definitions(${name} PRIVATE
			AURA_TEST_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
			AURA_TEST_OUT_DIR="${CMAKE_CURRENT_BINARY_DIR}/${out_dir}"
		)
		add_test(NAME ${name} COMMAND ${name})
		# AEC3 对 6 秒音频要跑几秒（Release）；给足余量，别让它偶发超时。
		# RUN_SERIAL：这是**实时节奏**的 DSP 链（喂帧按 10ms 节拍 + 收尾按队列
		# 排空判定），与 MNN 模型测试并行时 CPU 被抢占，出现过两类假失败：
		# ① AGC 尾部 2 帧未在 stop 前 flush（641 != 643）；② 喂参考帧踩到池背压
		# 上限报 OOM。单跑全过 —— 是负载敏感，不是链坏了。排到并行组之后跑。
		set_tests_properties(${name} PROPERTIES TIMEOUT 300 RUN_SERIAL TRUE)
	endfunction()

	aura_add_front_test(test_audio_debug audio_front)
	# 真实阵列录音（素材未入库）：缺素材时自己打印 SKIP 并返回成功。
	aura_add_front_test(test_audio_debug_real audio_front_real)

	# 工具冒烟：造场景 → 喂回去跑真链 → --check 的形状类断言。
	# 它验的是"人手工用工具时那条路"（命令行解析、落盘、--check），深度指标由上面
	# 两条测试断言。
	#
	# 两步拆成两条测试而不是一条：add_test 只收一个 COMMAND。用 fixture 串起来，
	# 而不是靠命名顺序 —— ctest -j 下没有 fixtures 的依赖关系不成立，跑的会是
	# "读上一个 cwd 里恰好残留的文件"，那种绿比红更坏。
	if(AURA_BUILD_TOOLS)
		add_test(NAME audio_debug_gen_scene
			COMMAND $<TARGET_FILE:audio_debug>
				--gen-scene ${CMAKE_CURRENT_BINARY_DIR}/audio_front/synth)
		set_tests_properties(audio_debug_gen_scene PROPERTIES
			TIMEOUT 60 FIXTURES_SETUP aura_synth_scene)

		add_test(NAME audio_debug_synth
			COMMAND $<TARGET_FILE:audio_debug>
				--in ${CMAKE_CURRENT_BINARY_DIR}/audio_front/synth_mic.wav
				--ref ${CMAKE_CURRENT_BINARY_DIR}/audio_front/synth_ref.wav
				--config ${CMAKE_CURRENT_SOURCE_DIR}/audio_front/configs/frontend_synth.conf
				--out-dir ${CMAKE_CURRENT_BINARY_DIR}/audio_front/synth_out
				--check)
		set_tests_properties(audio_debug_synth PROPERTIES
			TIMEOUT 300 FIXTURES_REQUIRED aura_synth_scene
			RUN_SERIAL TRUE) # 同上：实时节奏链，避开并行负载
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
		target_link_libraries(host_sim PRIVATE aura_inference aura_silero)
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

# ------------------------------------------------------------------- 测试串行
# 全套串行执行（用户拍板 2026-11-01）：产品运行时真正需要的并行只有 NN_ASYNC
# 的 worker（ASR/声纹的大推理），与测试无关；而 ctest -j 的并行 CPU 抢占制造
# 三类假 flake —— ① audio_front 帧池耗尽；② dsp_adapter VAD 边沿滞后一帧；
# ③ kws_chain 喂帧/解码超时。串行总时长 ~4 分钟，红了就是真红。
get_property(_aura_all_tests DIRECTORY PROPERTY TESTS)
set_tests_properties(${_aura_all_tests} PROPERTIES RUN_SERIAL TRUE)
