/* Aura — MNN 引擎适配（内部头）
 *
 * 本头只在 inference/ 内部可见；对外一律用 inference/inference.h 的 C 接口。
 * 算法模块**不得** include 本文件（分层铁律，todo.md 第 3 节）。
 *
 * 用途：把 inference.h 里 forward 声明的不透明结构（aura_infer_model_t /
 * aura_tensor_t）在 C++ 侧补全定义 —— C 代码只拿指针，C++ 侧持有 MNN 对象。
 */
#ifndef AURA_INFERENCE_ENGINE_MNN_ENGINE_MNN_H
#define AURA_INFERENCE_ENGINE_MNN_ENGINE_MNN_H

#include <memory>
#include <string>
#include <vector>

#include <MNN/Interpreter.hpp>
#include <MNN/expr/Expr.hpp>
#include <MNN/expr/Module.hpp>

#include "inference/inference.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 引擎侧对模型的封装：SESSION 形态持有 Interpreter+Session，
 * MODULE 形态持有 Express::Module；LLM 形态在 Phase 4 接入。 */
struct aura_infer_model {
    char              path[256];
    aura_infer_form_t form;
    aura_model_info_t info;

    /* SESSION 形态 */
    std::shared_ptr<MNN::Interpreter> net;
    MNN::Session                      *session; /* 不拥有，随 net 释放 */

    /* MODULE 形态（Express::Module，含 If/While 子图的模型走这条） */
    std::unique_ptr<MNN::Express::Module> module;

    /* 张量缓存：按输入/输出顺序，与 model 同生命周期。 */
    std::vector<aura_tensor_t> inputs;
    std::vector<aura_tensor_t> outputs;
};

/* 张量封装。两种形态各持一个句柄，另一个为 null：
 *   SESSION —— mt  指向 MNN 内部张量（借用，不拥有）
 *   MODULE  —— var 输入是引擎建的 _Input，输出是 onForward 的返回值 */
struct aura_tensor {
    aura_infer_model_t *model;
    bool                is_input;
    char                name[64];
    int32_t             index;
    MNN::Tensor        *mt;       /* SESSION：MNN 内部张量 */
    MNN::Express::VARP  var;      /* MODULE：Express 张量 */
    aura_tensor_info_t  info;     /* 缓存：形状/类型；MODULE 下也作为权威来源 */
};

#ifdef __cplusplus
}
#endif

#endif /* AURA_INFERENCE_ENGINE_MNN_ENGINE_MNN_H */
