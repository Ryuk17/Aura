# platform/ — HAL 平台抽象层
#   Windows: WASAPI 音频采集/播放
#   Linux:   ALSA 音频采集/播放
add_library(aura_platform STATIC
    audio_device_factory.cc)
target_include_directories(aura_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(aura_platform PUBLIC aura_interfaces aura_utils)

if(WIN32)
    target_sources(aura_platform PRIVATE wasapi_device.cc)
    target_link_libraries(aura_platform PRIVATE ole32 winmm avrt)
elseif(UNIX AND NOT APPLE)
    target_sources(aura_platform PRIVATE alsa_device.cc)
    find_package(ALSA REQUIRED)
    target_link_libraries(aura_platform PRIVATE ${ALSA_LIBRARIES})
    # 环境无 pkg-config 且 ALSA 裁剪安装于 /usr/local：补充库搜索路径
    target_link_directories(aura_platform PRIVATE /usr/local/lib)
endif()
