# tests/ — 单元测试（迷你测试框架，零依赖）
add_executable(aura_tests
    test_main.cc
    test_framework.cc
    test_utils.cc
    test_event_bus.cc
    test_orchestrator.cc
    test_aecm.cc)
target_link_libraries(aura_tests PRIVATE aura_core aura_audio aura_utils)
add_test(NAME aura_tests COMMAND aura_tests)
