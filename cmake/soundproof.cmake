# Aura — soundproof 算法引擎接入
#
# 算法实现、模型、MNN、算法测试全部归 soundproof。Aura 只通过它的 C ABI
# （AudioEngine_*）把算法包装成 pipeline node，见 src/soundproof/。
#
# soundproof 是 CMake super-build：它自己 FetchContent 拉 MNN / abseil /
# neon-fft / pffft / eigen。我们只负责在 MakeAvailable **之前**把裁剪开关
# 写进缓存，避免它把 voice agent 用不到的算法全编一遍。
#
# 离线/本地开发：
#   cmake ... -DFETCHCONTENT_SOURCE_DIR_SOUNDPROOF=F:/share/soundproof
# 这样不会走 GitHub，也不会重复下载（源码目录仍会被单独构建）。

include(FetchContent)

# ---------------------------------------------------------------- 算法裁剪
# 采集/播放链需要的 DSP 目标
set(BUILD_AE_AEC  ON  CACHE BOOL "" FORCE)
set(BUILD_AE_AECM ON  CACHE BOOL "" FORCE)
set(BUILD_AE_NS   ON  CACHE BOOL "" FORCE)
set(BUILD_AE_AGC  ON  CACHE BOOL "" FORCE)
set(BUILD_AE_AGC2 ON  CACHE BOOL "" FORCE)
set(BUILD_AE_BF   ON  CACHE BOOL "" FORCE)
set(BUILD_AE_SRC  ON  CACHE BOOL "" FORCE)
set(BUILD_AE_HS   ON  CACHE BOOL "" FORCE)
set(BUILD_AE_TS   ON  CACHE BOOL "" FORCE)
# IE 需要 NS/AEC 的噪声谱作为第二输入，Aura 适配层暂不接入
set(BUILD_AE_IE   OFF CACHE BOOL "" FORCE)
# DR 是 4 ms 帧（64 样本 @16k），与 Aura 的 10 ms 帧不兼容；
# 需要时单独开，并在 Aura 侧补帧重组器。
set(BUILD_AE_DR   OFF CACHE BOOL "" FORCE)

# MNN 算法目标：取决于 Aura 的 MNN 开关
if(AURA_WITH_SOUNDPROOF_MNN)
    set(WITH_MNN ON CACHE BOOL "" FORCE)
    set(BUILD_AE_VAD  ON CACHE BOOL "" FORCE)
    set(BUILD_AE_KWS  ON CACHE BOOL "" FORCE)
    set(BUILD_AE_ASR  ON CACHE BOOL "" FORCE)
    set(BUILD_AE_SR   ON CACHE BOOL "" FORCE)
    set(BUILD_AE_TTS  ON CACHE BOOL "" FORCE)
    set(BUILD_AE_TURN ON CACHE BOOL "" FORCE)
else()
    set(WITH_MNN OFF CACHE BOOL "" FORCE)
    # 这些目标在 WITH_MNN=OFF 时 soundproof 自己会跳过；显式关掉减少配置噪音
    set(BUILD_AE_KWS  OFF CACHE BOOL "" FORCE)
    set(BUILD_AE_ASR  OFF CACHE BOOL "" FORCE)
    set(BUILD_AE_SR   OFF CACHE BOOL "" FORCE)
    set(BUILD_AE_TTS  OFF CACHE BOOL "" FORCE)
    set(BUILD_AE_TURN OFF CACHE BOOL "" FORCE)
endif()

# LLM / RAG：soundproof 的 AE_LLM 需要 WITH_MNN_VISION（会拉 OpenCV），
# 默认关闭，Phase C 需要时用 -DAURA_WITH_SOUNDPROOF_LLM=ON 打开。
if(AURA_WITH_SOUNDPROOF_LLM AND AURA_WITH_SOUNDPROOF_MNN)
    set(WITH_MNN_LLM    ON CACHE BOOL "" FORCE)
    set(WITH_MNN_VISION ON CACHE BOOL "" FORCE)
    set(BUILD_AE_LLM ON CACHE BOOL "" FORCE)
    set(BUILD_AE_RAG OFF CACHE BOOL "" FORCE)
else()
    set(BUILD_AE_LLM OFF CACHE BOOL "" FORCE)
    set(BUILD_AE_RAG OFF CACHE BOOL "" FORCE)
endif()

# ------------------------------------------------------------ 拉取 / 构建
FetchContent_Declare(soundproof
    GIT_REPOSITORY https://github.com/Ryuk17/soundproof.git
    GIT_TAG        master
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(soundproof)

# soundproof 的头文件用 AUDIO_ENGINE_API 做导出宏；静态链接时必须定义
# AUDIO_ENGINE_STATIC，否则 MinGW/MSVC 下会变成 dllimport。
set(AURA_SOUNDPROOF_TARGETS
    AE_VAD AE_AEC AE_AECM AE_NS AE_AGC AE_AGC2 AE_BF AE_SRC AE_HS AE_TS)
if(AURA_WITH_SOUNDPROOF_MNN)
    list(APPEND AURA_SOUNDPROOF_TARGETS AE_KWS AE_ASR AE_SR AE_TTS AE_TURN)
endif()
if(AURA_WITH_SOUNDPROOF_LLM AND AURA_WITH_SOUNDPROOF_MNN)
    list(APPEND AURA_SOUNDPROOF_TARGETS AE_LLM)
endif()

foreach(_t IN LISTS AURA_SOUNDPROOF_TARGETS)
    if(NOT TARGET ${_t})
        message(FATAL_ERROR
            "Aura: soundproof 未提供 ${_t} 目标。检查 BUILD_AE_* / WITH_MNN 开关，"
            "或确认 FetchContent 拉到的是带该算法的 soundproof 版本。")
    endif()
endforeach()

# soundproof 默认把静态库写到它自己的源码树 lib/（super-build 写法）。Aura 可能
# 同时存在 DSP-only / MNN 两个构建目录，写同一份 lib/ 会互相覆盖（实测：MNN
# 构建产出的 AE_VAD 带 silero，DSP-only 构建随后链接它会缺 MNN 符号）。
# 这里把算法库输出重定向到各构建目录自己的 soundproof-lib/。
set(AURA_SOUNDPROOF_LIB_DIR "${CMAKE_BINARY_DIR}/soundproof-lib")
file(MAKE_DIRECTORY "${AURA_SOUNDPROOF_LIB_DIR}")
foreach(_t IN LISTS AURA_SOUNDPROOF_TARGETS)
    set_target_properties(${_t} PROPERTIES
        ARCHIVE_OUTPUT_DIRECTORY "${AURA_SOUNDPROOF_LIB_DIR}")
endforeach()

# ------------------------------------------------ soundproof 自带测试的裁剪
# Aura 的 ctest 只跑 Aura 的测试；算法测试在 soundproof 仓库里跑。
# 默认把 test_ae_* 从 ALL 目标排除，并在 CTest 里禁用它们。
if(NOT AURA_SOUNDPROOF_TESTS)
    get_property(_sp_targets DIRECTORY ${soundproof_SOURCE_DIR} PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_t IN LISTS _sp_targets)
        if(_t MATCHES "^test_ae_" OR _t STREQUAL "test_mnn_engine")
            set_target_properties(${_t} PROPERTIES EXCLUDE_FROM_ALL TRUE)
        endif()
    endforeach()

    # 说明：CMake 不允许父目录 disable 子目录注册的 CTest 用例，所以这里只把
    # test_ae_* 从 ALL 目标排除（省构建时间）；跑 Aura 测试请用
    #   ctest -L aura
    # 要跑 soundproof 自带测试，配置 -DAURA_SOUNDPROOF_TESTS=ON。
    message(STATUS "Aura: soundproof tests excluded from ALL (AURA_SOUNDPROOF_TESTS=OFF)")
endif()

message(STATUS "Aura: soundproof ready (source: ${soundproof_SOURCE_DIR})")
