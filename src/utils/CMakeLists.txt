# utils/ — 通用工具库（日志、配置、wav），零第三方依赖
add_library(aura_utils STATIC
    log.cc
    json.cc
    wav.cc)
target_include_directories(aura_utils PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(aura_utils PUBLIC aura_interfaces)
