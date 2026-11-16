# Aura 源码构建 —— 按分层产出静态库目标
#
# 每个库只允许依赖它下层的库（PUBLIC 依赖传播 = 编译期强制分层）：
#   aura_agent → aura_flow → aura_core → aura_utils → aura_platform
#   aura_agent → aura_soundproof → (soundproof libAE_*)
#
# 算法实现不在本仓库：aura_soundproof 只做 C ABI 适配。

find_package(Threads REQUIRED)

# 公共编译设置：所有目标共享的 include 目录与告警级别。
add_library(aura_common INTERFACE)
target_include_directories(aura_common INTERFACE
	${CMAKE_SOURCE_DIR}/include                        # agent_export.h（对外 API）
	${CMAKE_SOURCE_DIR}/platform                       # osal/...
	${CMAKE_SOURCE_DIR}/src                            # core/ utils/ agent/ soundproof/
	${CMAKE_BINARY_DIR}/generated                      # aura_config.h（CMake 生成）
)
target_link_libraries(aura_common INTERFACE Threads::Threads)
if(WIN32 AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
	target_link_libraries(aura_common INTERFACE pthread)
endif()

if(MSVC)
	target_compile_options(aura_common INTERFACE /W3)
else()
	target_compile_options(aura_common INTERFACE -Wall -Wextra)
endif()

# ------------------------------------------------------------ platform（HAL）
# 唯一允许 include 板级 SDK 头的层；移植时只改这里。
set(AURA_OSAL_SOURCES ${CMAKE_SOURCE_DIR}/platform/osal/posix/osal_posix.c)

add_library(aura_platform STATIC ${AURA_OSAL_SOURCES})
target_link_libraries(aura_platform PUBLIC aura_common)

# -------------------------------------------------------------- utils（基础）
add_library(aura_utils STATIC
	utils/error.c
	utils/ringbuf/ringbuf.c
	utils/mem_pool/mem_pool.c
	utils/logger/logger.c
	utils/profiler/profiler.c
)
target_link_libraries(aura_utils PUBLIC aura_platform)

# --------------------------------------------------------------- core（骨架）
add_library(aura_core STATIC
	core/node.c
	core/pipeline/pipeline.c
	core/algorithm.c
	core/event_bus/event_bus.c
	core/state_machine/state_machine.c
	core/config/config.c
	core/playback/playback.c
)
target_link_libraries(aura_core PUBLIC aura_utils)

# ------------------------------------------------------ agent/flow（数据流中间件）
# 会话/轮次/文本/工具/记忆/取消/追踪。音频帧不走这里，见 docs/agent_dataflow.md。
add_library(aura_flow STATIC
	agent/flow/flow_bus.c
	agent/flow/flow_stage.c
	agent/flow/flow_middleware.c
	agent/flow/flow_session.c
	agent/flow/flow_bridge.c
	agent/flow/flow_store.c
	agent/flow/flow_metrics.c
)
target_link_libraries(aura_flow PUBLIC aura_core)

# ------------------------------------------------------ soundproof（算法适配层）
# 只包含 AudioEngine_* → aura_node_t 的胶水；没有任何算法实现。
if(AURA_WITH_SOUNDPROOF)
	set(AURA_SOUNDPROOF_SOURCES
		soundproof/ae_common.c
		soundproof/ae_audio_nodes.c
		soundproof/ae_vad_node.c
		soundproof/ae_register.c
	)
	if(AURA_WITH_SOUNDPROOF_MNN)
		list(APPEND AURA_SOUNDPROOF_SOURCES
			soundproof/ae_kws_node.c
			soundproof/ae_asr_node.c
		)
	endif()
	if(AURA_WITH_SOUNDPROOF_MNN AND AURA_BUILD_VOICEPRINT)
		list(APPEND AURA_SOUNDPROOF_SOURCES soundproof/ae_sr_node.c)
	endif()
	if(AURA_WITH_SOUNDPROOF_MNN AND AURA_BUILD_TTS)
		list(APPEND AURA_SOUNDPROOF_SOURCES soundproof/ae_tts_node.c)
	endif()
	if(AURA_WITH_SOUNDPROOF_LLM AND AURA_BUILD_LLM)
		list(APPEND AURA_SOUNDPROOF_SOURCES soundproof/ae_llm_node.c)
	endif()
else()
	# 无算法引擎：只保留空注册函数，框架单测仍可链接。
	set(AURA_SOUNDPROOF_SOURCES soundproof/ae_register.c)
endif()

add_library(aura_soundproof STATIC ${AURA_SOUNDPROOF_SOURCES})
target_compile_definitions(aura_soundproof PRIVATE AUDIO_ENGINE_STATIC=1)
target_link_libraries(aura_soundproof PUBLIC aura_core)
if(AURA_WITH_SOUNDPROOF)
	target_link_libraries(aura_soundproof PRIVATE ${AURA_SOUNDPROOF_TARGETS})
	if(WIN32)
		# soundproof 的部分 WAV/工具代码在 MinGW 下需要 winmm；静态库传递到最终链接。
		target_link_libraries(aura_soundproof PUBLIC winmm)
	endif()
	# AE_KWS 与 AE_ASR 都把共享的 transducer/hypothesis 源编进了自己的静态库，
	# 同时链接两者时会出现重复符号。两份实现来自同一份源码，取任意一份语义等价；
	# 在 GNU/Clang 链接器上允许重复定义。长期应由 soundproof 把共享源拆成
	# OBJECT/独立库（backlog）。
	if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
		target_link_options(aura_soundproof INTERFACE "LINKER:--allow-multiple-definition")
	endif()
endif()

# ------------------------------------------------------ agent（闭环组装层）
# 组装 config → pipeline/event_bus/状态机/flow → 对外纯 C API。
# Phase C 在此填充 orchestrator / dialogue / memory / tools。
add_library(aura_agent STATIC
	agent/agent_core.c
	agent/agent_stages.c
	agent/llm/llm.c
	agent/memory/memory.c
	agent/stages/dialogue_stage.c
	agent/stages/memory_stage.c
	agent/stages/orchestrator_stage.c
	agent/stages/session_runtime.c
	agent/stages/session_stage.c
	agent/stages/stage_proto.c
	agent/stages/tool_runner_stage.c
	agent/stages/tts_egress_stage.c
	agent/tools/tools.c
)
target_link_libraries(aura_agent PUBLIC aura_core aura_flow aura_soundproof)
