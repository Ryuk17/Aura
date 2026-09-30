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
	core/event_bus/event_bus.c
	core/state_machine/state_machine.c
	core/config/config.c
)
target_link_libraries(aura_core PUBLIC aura_utils)

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

# ------------------------------------------------------ agent（闭环组装层）
# Phase 1 只做骨架组装：config → pipeline/event_bus/状态机 → 对外纯 C API。
# Phase 4 在此填充 orchestrator / dialogue / memory / tools。
add_library(aura_agent STATIC
	agent/agent_core.c
)
target_link_libraries(aura_agent PUBLIC aura_core)
if(AURA_WITH_MNN)
	target_link_libraries(aura_agent PUBLIC aura_inference)
endif()
