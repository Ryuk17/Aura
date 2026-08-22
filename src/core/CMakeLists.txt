# core/ — 编排引擎：事件定义、事件总线、状态机（Orchestrator）
add_library(aura_core STATIC
    events.cc
    event_bus.cc
    orchestrator.cc)
target_include_directories(aura_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(aura_core PUBLIC aura_interfaces aura_utils)
