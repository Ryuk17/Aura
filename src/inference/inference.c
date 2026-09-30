/* Aura — 推理层 C 侧实现
 *
 * 本文件只放**与引擎无关**的纯 C 逻辑（帮助函数与全局状态登记），
 * 真正与 MNN 打交道的是 engine_mnn/engine_mnn.cpp。
 */
#include "inference/inference.h"

#include <string.h>
