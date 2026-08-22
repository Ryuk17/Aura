# examples/ — 示例程序
add_executable(aura_demo_loopback m1_loopback.cc)
target_link_libraries(aura_demo_loopback PRIVATE aura_core aura_audio aura_platform aura_utils)
