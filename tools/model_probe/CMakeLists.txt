# model_probe — 模型探针（换模型 / 接入新算法的第一步）
#
# 纯诊断工具：加载 .mnn，打印张量清单与全量 metadata，可选空跑一次前向。
# 只依赖 aura_inference（不碰 pipeline / 算法层）—— 它回答的是"模型文件本身
# 长什么样"，在算法还没写的时候就得能跑。
#
# Phase 3 起每个新模型接入前跑一遍：ONNX→MNN 转换丢元数据、改 dtype、固化动态维
# 都是静默的，照参考实现直接写引擎只会得到"结果不对"而不是报错。

if(AURA_WITH_MNN AND AURA_BUILD_TOOLS)
	add_executable(model_probe model_probe.c)
	target_link_libraries(model_probe PRIVATE aura_inference)
else()
	message(STATUS "Aura: AURA_WITH_MNN=OFF —— 不构建 model_probe")
endif()
