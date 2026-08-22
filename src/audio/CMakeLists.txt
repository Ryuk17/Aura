# audio/ — 音频处理层：AECM 回声消除封装 + 采集→处理→总线 管线
add_library(aura_audio STATIC
    aecm_processor.cc
    audio_pipeline.cc)
target_include_directories(aura_audio PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(aura_audio PUBLIC aura_interfaces aura_core aura_aecm)
