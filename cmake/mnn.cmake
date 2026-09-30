# MNN — 端侧统一推理引擎（Phase 0 选型，见 docs/model_requirement.md）
#
# 作为 in-tree 子工程构建（FetchContent add_subdirectory），产物是 MNN 目标本身，
# 直接 target_link_libraries(<target> MNN) 使用。
#
# 注意：
#  - 不要再用 include_directories/link_directories 指向源码树 —— MNN 是 CMake 目标，
#    传播 include 目录由 target 完成。
#  - 3.6.1 与模型产物版本对齐（LLM 3.4.0 / TTS 3.6，见 model_requirement.md 第 1 节）。
#  - 升级点集中在 MNN_VERSION / MNN_URL / MNN_URL_HASH 三处（hash 用实测 sha256sum）。

set(MNN_VERSION "3.6.1")
set(MNN_URL "https://github.com/alibaba/MNN/archive/refs/tags/3.6.1.tar.gz")
set(MNN_URL_HASH "SHA256=4b6065c4e2674318f5bf1dc75836ce4d30c17bfe598c4a1b11b7d0b2092b06e6")

# ---------------------------------------------------------------- 裁剪开关
# 默认只构建"推理运行时"，不构建转换器/工具/训练 —— 那些会把构建时间拉长数倍，
# 且 PC 与板端都不需要（模型转换是 tools/model_convert 的活，离线执行）。
set(MNN_BUILD_SHARED_LIBS OFF CACHE BOOL "MNN static lib" FORCE)
set(MNN_SEP_BUILD OFF CACHE BOOL "must be OFF for static build" FORCE)
set(MNN_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(MNN_BUILD_CONVERTER OFF CACHE BOOL "" FORCE)
set(MNN_BUILD_DEMO OFF CACHE BOOL "" FORCE)
set(MNN_BUILD_QUANTOOLS OFF CACHE BOOL "" FORCE)
set(MNN_BUILD_TRAIN OFF CACHE BOOL "" FORCE)
set(MNN_EVALUATION OFF CACHE BOOL "" FORCE)
set(MNN_BUILD_OPENCV OFF CACHE BOOL "" FORCE)
set(MNN_BUILD_TEST OFF CACHE BOOL "" FORCE)
# 转换器专用，运行期不需要（.mnn 是 flatbuffer，不走 protobuf）
set(MNN_BUILD_PROTOBUFFER OFF CACHE BOOL "" FORCE)

# ---------------------------------------------------------- LLM 管线开关
# model_requirement.md 第 1 节：MNN-LLM 模型（LLM / Embedding）的运行前提。
# Phase 1 不构建（体积/耗时大），Phase 4 接入 LLM 时打开 AURA_WITH_MNN_LLM。
if(AURA_WITH_MNN_LLM)
	set(MNN_BUILD_LLM ON CACHE BOOL "" FORCE)
	set(MNN_LOW_MEMORY ON CACHE BOOL "" FORCE)
	set(MNN_CPU_WEIGHT_DEQUANT_GEMM ON CACHE BOOL "" FORCE)
	set(MNN_SUPPORT_TRANSFORMER_FUSE ON CACHE BOOL "" FORCE)
endif()

FetchContent_Declare(MNN
	URL ${MNN_URL}
	URL_HASH ${MNN_URL_HASH}
	DOWNLOAD_EXTRACT_TIMESTAMP ON
)
FetchContent_MakeAvailable(MNN)

# MNN 的 include 目录是目录级（include_directories）而非 target 级传播 ——
# 建一个 INTERFACE 目标承接，依赖方只 link aura_mnn 即可，无需关心 MNN 内部结构。
# 目录清单与 MNN CMakeLists 的 MNN_INCLUDES 保持一致（含 flatbuffers/half 等
# 传递依赖头，Interpreter.hpp → MNN_generated.h 需要它们）。
add_library(aura_mnn INTERFACE)
target_link_libraries(aura_mnn INTERFACE MNN)
target_include_directories(aura_mnn INTERFACE
	${MNN_SOURCE_DIR}/include
	${MNN_SOURCE_DIR}/source
	${MNN_SOURCE_DIR}/express
	${MNN_SOURCE_DIR}/tools
	${MNN_SOURCE_DIR}/codegen
	${MNN_SOURCE_DIR}/schema/current
	${MNN_SOURCE_DIR}/3rd_party
	${MNN_SOURCE_DIR}/3rd_party/flatbuffers/include
	${MNN_SOURCE_DIR}/3rd_party/half
	${MNN_SOURCE_DIR}/3rd_party/imageHelper
	${MNN_SOURCE_DIR}/3rd_party/OpenCLHeaders
)

message(STATUS "MNN ${MNN_VERSION} ready (source: ${mnn_SOURCE_DIR})")
