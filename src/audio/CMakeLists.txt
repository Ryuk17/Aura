# src/audio — 音频链路（AudioChain 节点 + 管线 + 焦点管理）
# 依赖：aura_core（事件总线）、aura_utils（log）、AE_AECM（TrickRoom）
add_library(aura_audio STATIC
    audio_chain.cc
    aecm_node.cc
    audio_pipeline.cc
    audio_focus.cc
    )
target_include_directories(aura_audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(aura_audio PUBLIC aura_interfaces aura_core aura_utils AE_AECM)
