# tests/ — 单元测试（迷你测试框架，零依赖）
add_executable(aura_tests
    test_main.cc
    test_framework.cc
    test_utils.cc
    test_event_bus.cc
    test_orchestrator.cc
    test_aecm.cc
    )
target_link_libraries(aura_tests PRIVATE aura_core aura_utils aura_audio)
# 测试数据目录（golden wav，见 docs/audio_engine_integration.md R3）
target_compile_definitions(aura_tests PRIVATE
    AURA_TEST_DATA_DIR="${PROJECT_SOURCE_DIR}/resources/audio_engine/data")
add_test(NAME aura_tests COMMAND aura_tests)
