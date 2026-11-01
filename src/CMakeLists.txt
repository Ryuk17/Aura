# Aura 源码构建 —— 按分层产出静态库目标
#
# 每个库只允许依赖它下层的库（PUBLIC 依赖传播 = 编译期强制分层）：
#   aura_agent → aura_inference → aura_core → aura_utils → aura_platform
# 想反向依赖会直接链接失败，而不是靠 code review 抓。

find_package(Threads REQUIRED)

# 公共编译设置：所有目标共享的 include 目录与告警级别。
add_library(aura_common INTERFACE)
target_include_directories(aura_common INTERFACE
	${CMAKE_SOURCE_DIR}/include                        # agent_export.h（对外 API）
	${CMAKE_SOURCE_DIR}/platform                       # osal/...
	${CMAKE_SOURCE_DIR}/src                            # core/ utils/ inference/ agent/
	${CMAKE_BINARY_DIR}/generated                      # aura_config.h（CMake 生成）
)
target_link_libraries(aura_common INTERFACE Threads::Threads)
if(WIN32 AND CMAKE_C_COMPILER_ID STREQUAL "GNU")
	# MinGW-w64 需要显式链接 winpthreads
	target_link_libraries(aura_common INTERFACE pthread)
endif()

if(MSVC)
	target_compile_options(aura_common INTERFACE /W3)
else()
	target_compile_options(aura_common INTERFACE -Wall -Wextra)
endif()

# ------------------------------------------------------------ platform（HAL）
# 唯一允许 include 板级 SDK 头的层；移植时只改这里。
# 注意 platform/ 在仓库根目录（不在 src/ 下），路径要用绝对前缀。
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
)
target_link_libraries(aura_core PUBLIC aura_utils)

# ------------------------------------------------------------------ dsp（音频算法）
# 3A/BSS 等信号处理算法的接入层。aura_dsp 是**引擎无关**的适配模板（只用函数
# 指针，不 include 任何引擎头），因此全平台、任何裁剪组合下都能编、都能测。
# 具体引擎的描述表单独成目标（trickroom/），换引擎只换那一张表。
add_library(aura_dsp STATIC
	dsp/aura_dsp_adapter.c
)
target_link_libraries(aura_dsp PUBLIC aura_core)

# FFT 与 fbank：KWS/ASR/声纹共用的前端特征。放在 dsp 层，但**不**依赖 core ——
# 它们是纯计算，只有 utils 的错误码和日志，离线工具（tools/、tests/）也能直接
# 拿去用，不必为了算一帧特征先把 pipeline 拉起来。
#
# aura_fft 是唯一能看见 NE10 头的地方（链接 NE10 用的是 PRIVATE —— 把那堆
# NE10_*.h 泄漏给上层，就等于允许别人绕过这里直接用 FFT）。
if(AURA_WITH_NEON_FFT)
	add_library(aura_fft STATIC dsp/aura_fft.c)
	target_link_libraries(aura_fft PUBLIC aura_utils PRIVATE aura_neon_fft)

	# 特征流水线（WeNet 的 Fbank + FeaturePipeline，见 dsp/aura_feature_pipeline.h）：
	# 攒帧、切帧、开头的镜像前缀、以及一个给下游读特征的非阻塞队列都在里面。
	# KWS / ASR / 声纹三条链共用它一个实例（各自一份），不各算一遍。
	# aura_feature.{h,cpp} 是它的纯 C 封装（算法节点用）。
	add_library(aura_feature STATIC
		dsp/aura_feature_pipeline.cc
		dsp/aura_feature.cpp
	)
	target_link_libraries(aura_feature PUBLIC aura_fft)
endif()

if(AURA_WITH_TRICKROOM)
	# 六张描述表对应的六个引擎库：aec3 / bf / ns / agc / src（+ vad，Phase 1 已在用）
	foreach(ae_target AE_AEC AE_BF AE_NS AE_AGC AE_SRC AE_VAD)
		if(NOT TARGET ${ae_target})
			message(FATAL_ERROR
				"Aura: TrickRoom 未提供 ${ae_target} 目标（BUILD_AE_* 被关掉了？），"
				"而 aura_dsp_trickroom 需要它（见 dsp/trickroom/aura_dsp_trickroom.c）")
		endif()
	endforeach()

	add_library(aura_dsp_trickroom STATIC
		dsp/trickroom/aura_dsp_trickroom.c
	)
	# 描述表是唯一能看见 TrickRoom 头的地方（.c 内 include 引擎头 + 链接引擎库）。
	# AUDIO_ENGINE_STATIC：静态链接时必须定义，否则 _WIN32 下 API 宏会变成
	# __declspec(dllimport)，链接期才报"找不到符号"。
	# AURA_DSP_TRICKROOM_ENABLED：文件内的编译期自检（见该 .c 首部）—— 有它，
	# 源文件被误加进别的目标时会立刻报错，而不是缺符号。
	target_compile_definitions(aura_dsp_trickroom PRIVATE
		AUDIO_ENGINE_STATIC=1
		AURA_DSP_TRICKROOM_ENABLED=1
	)
	target_link_libraries(aura_dsp_trickroom PUBLIC aura_dsp
		PRIVATE AE_AEC AE_BF AE_NS AE_AGC AE_SRC AE_VAD)
endif()

# ---------------------------------------------------------- inference（推理）
# 对外只暴露 C 接口（inference/inference.h），MNN 的 C++ 头只在 engine_mnn 内部出现。
# 算法模块因此永远看不到 MNN/NPU SDK 头，替换推理引擎不需要改动算法代码。
if(AURA_WITH_MNN)
	add_library(aura_inference STATIC
		inference/inference.c
		inference/engine_mnn/engine_mnn.cpp
		inference/model_manager/model_manager.c
	)
	target_link_libraries(aura_inference PUBLIC aura_core PRIVATE aura_mnn)
	target_compile_definitions(aura_inference PUBLIC AURA_WITH_MNN=1)
else()
	message(WARNING "Aura: AURA_WITH_MNN=OFF —— inference 层不编译，模型相关功能不可用")
endif()

# ------------------------------------------------------ algorithm（NN 接入）
# NN 算法适配模板，与 dsp/aura_dsp_adapter 同层（模板里没有任何引擎代码，
# 张量读写走 inference 的 C 接口）。它**必须**有推理层才成立 —— 装不了模型的
# NN 节点一个也造不出来，所以 AURA_WITH_MNN=OFF 时干脆不建这个目标，
# 而不是产出一个永远链不上的库。
if(AURA_WITH_MNN)
	add_library(aura_algorithm STATIC
		algorithm/aura_nn_adapter.c
	)
	target_link_libraries(aura_algorithm PUBLIC aura_core PUBLIC aura_inference)
endif()

# ------------------------------------------------------ kws / asr（感知算法族）
# 算法族与模板分库：一张描述表 + 算法逻辑，模板是骨架。上层（应用/测试）
# 在 agent init 前调 aura_kws_register() / aura_asr_register() 注册。
if(AURA_WITH_MNN AND AURA_WITH_NEON_FFT)
	add_library(aura_kws STATIC
		kws/aura_kws.c
	)
	target_link_libraries(aura_kws PUBLIC aura_algorithm aura_zipformer)

	add_library(aura_asr STATIC
		asr/aura_asr.c
	)
	target_link_libraries(aura_asr PUBLIC aura_algorithm aura_zipformer)

	add_library(aura_voiceprint STATIC
		voiceprint/aura_voiceprint.c
		voiceprint/vp_lib.c
	)
	target_link_libraries(aura_voiceprint PUBLIC aura_algorithm aura_feature
		aura_inference)

	# silero VAD 描述表（从 tests/host_sim 搬进正式模块）
	add_library(aura_silero STATIC
		audio/vad/aura_silero.c
	)
	target_link_libraries(aura_silero PUBLIC aura_algorithm)
endif()

# ------------------------------------------------- zipformer 族（KWS/ASR 共用）
# 辅助件（纯 C、零 MNN 依赖）：symbol_table（tokens.txt 解析）与 context_graph
# （关键词 AC 自动机）—— 离线工具与测试不用拉起推理就能用。
add_library(aura_zipformer_util STATIC
	dsp/zipformer/symbol_table.c
	dsp/zipformer/context_graph.c
)
target_link_libraries(aura_zipformer_util PUBLIC aura_utils)

# 流式引擎：纯 C、只依赖 inference 的 C 接口，不碰 core/pipeline。
if(AURA_WITH_MNN AND AURA_WITH_NEON_FFT)
	add_library(aura_zipformer STATIC
		dsp/zipformer/aura_zipformer.c
	)
	target_link_libraries(aura_zipformer PUBLIC aura_inference aura_feature
		aura_zipformer_util)
endif()

# ------------------------------------------------------ agent（闭环组装层）
# Phase 1 只做骨架组装：config → pipeline/event_bus/状态机 → 对外纯 C API。
# Phase 4 在此填充 orchestrator / dialogue / memory / tools。
add_library(aura_agent STATIC
	agent/agent_core.c
)
target_link_libraries(aura_agent PUBLIC aura_core)
if(AURA_WITH_MNN)
	# aura_algorithm：配置链上的 NN 节点由它的工厂 create（chain_build 只按
	# 描述表调函数指针），少了它链上出现 NN 算法时链接期就缺符号。
	target_link_libraries(aura_agent PUBLIC aura_inference aura_algorithm)
endif()
if(AURA_WITH_TRICKROOM)
	# aura_dsp_trickroom：同上，链上写 aec3/bf/ns/agc/src 时由它的工厂造节点。
	# agent 是装配层，在这一层链住 DSP 族不违反分层（core 仍然不认识 TrickRoom）。
	target_link_libraries(aura_agent PUBLIC aura_dsp_trickroom)
endif()
