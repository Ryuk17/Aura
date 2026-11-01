# voiceprint_enroll — 声纹离线注册工具
# 注册与运行时零共享：音频 → fbank → embedding（独立模型）→ 库文件，
# 声纹节点初始化时读同一文件。
if(AURA_WITH_MNN AND AURA_WITH_NEON_FFT)
	add_executable(voiceprint_enroll voiceprint_enroll.c)
	target_link_libraries(voiceprint_enroll PRIVATE
		aura_voiceprint aura_inference aura_utils aura_platform)
endif()
