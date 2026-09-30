/* Aura — MNN 引擎适配实现
 *
 * 职责（todo.md 3 / model_requirement.md 1.2）：
 *   - 把 inference.h 的 C 接口接到 MNN C++ API；
 *   - 全局统一 MNN 配置（线程数、后端、precision/memory 档位）；
 *   - MemoryPool 全局复用：同类模型挂在同一个 Runtime 下，
 *     MNN 在该 Runtime 内统一复用中间张量内存（todo.md 4.4）。
 *
 * 两种运行形态，**按模型能力选，不是偏好问题**：
 *   - SESSION：Interpreter + Session。算子表里没有控制流子图的模型（声纹等）。
 *   - MODULE ：Express::Module。**带 If/While 子图的模型必须走这条** ——
 *     Session 的 resizeSession 要对整图做形状推导，子图分支在推导期没有取值，
 *     会以 COMPUTE_SIZE_ERROR 失败（输出张量恒为 0 形状，runSession 报
 *     "Can't run session because not resized"）。3.3.0 / 3.6.1 上表现一致。
 *     silero VAD 就是这种模型（输入含动态维 + If 子图），官方 sherpa-mnn 的
 *     SileroVadModel 同样只用 Module，见 MNN 源码树
 *     apps/frameworks/sherpa-mnn/sherpa-mnn/csrc/silero-vad-model.cc。
 *
 * 输入张量约定（MODULE）：模型声明的形状可能含 -1（动态维），_Input 需要具体形状，
 * 故建 VARP 时用 1 占位，而 info 里保留声明值（含 -1）—— 调用方据此得知
 * "这个输入必须先 aura_tensor_resize"。动态形状是流式模型（VAD 窗口、ASR T 维）
 * 的常态，不是异常。
 *
 * MNN 3.6.1 注意：张量类型是 halide_type_t（code/bits），不是 DataType 枚举；
 * Session 路径的 RuntimeInfo 是 pair<map<ForwardType, Runtime>, Runtime>，
 * Express 路径用 Executor::RuntimeManager，两者各自持有。
 */
#include "inference/engine_mnn/engine_mnn.h"

#include <algorithm>
#include <stdio.h>
#include <string.h>

#include <MNN/HalideRuntime.h>
#include <MNN/MNNForwardType.h>
#include <MNN/expr/Executor.hpp>
#include <MNN/expr/ExprCreator.hpp>
#include <MNN/expr/NeuralNetWorkOp.hpp>

#include "osal/osal.h"
#include "utils/logger/logger.h"

#define TAG "mnn"

using namespace MNN;

/* ------------------------------------------------------------ 全局引擎状态 */

namespace {

struct EngineState {
    bool             inited = false;
    aura_infer_cfg_t cfg;
    ScheduleConfig   sched;
    RuntimeInfo      runtime; /* SESSION 全局共享：MemoryPool 复用 */
    /* MODULE 全局共享：Express 走 RuntimeManager，所有 Module 挂同一份，
     * 中间张量内存在 RuntimeManager 内复用（todo.md 4.4）。 */
    std::shared_ptr<Express::Executor::RuntimeManager> rtmgr;
};

EngineState g_state;

/* 引擎持有所有已加载模型（内存统计用）。声明在前，aura_infer_load 用。 */
std::vector<aura_infer_model_t *> &engine_models()
{
    static std::vector<aura_infer_model_t *> models;
    return models;
}

ScheduleConfig make_sched(const aura_infer_cfg_t *cfg)
{
    static BackendConfig bc; /* 生命周期必须覆盖所有 session：静态持有 */
    ScheduleConfig sc;
    switch (cfg->backend) {
    case AURA_INFER_BACKEND_CPU:    sc.type = MNN_FORWARD_CPU; break;
    case AURA_INFER_BACKEND_OPENCL: sc.type = MNN_FORWARD_OPENCL; break;
    case AURA_INFER_BACKEND_VULKAN: sc.type = MNN_FORWARD_VULKAN; break;
    default:                        sc.type = MNN_FORWARD_AUTO; break;
    }
    sc.numThread = (cfg->threads > 0) ? cfg->threads : 2;
    bc.power     = BackendConfig::Power_Normal;
    bc.memory    = cfg->memory_low ? BackendConfig::Memory_Low : BackendConfig::Memory_Normal;
    bc.precision = cfg->precision_low ? BackendConfig::Precision_Low
                                      : BackendConfig::Precision_Normal;
    sc.backendConfig = &bc;
    return sc;
}

/* halide_type_t → aura dtype。 */
aura_dtype_t from_halide(halide_type_t t)
{
    switch (t.code) {
    case halide_type_float:
        return (t.bits == 16) ? AURA_DTYPE_F16 : AURA_DTYPE_F32;
    case halide_type_int:
        if (t.bits == 8) {
            return AURA_DTYPE_S8;
        }
        if (t.bits == 64) {
            return AURA_DTYPE_S64;
        }
        return AURA_DTYPE_S32;
    case halide_type_uint:
        return (t.bits == 8) ? AURA_DTYPE_U8 : AURA_DTYPE_UNKNOWN;
    default:
        return AURA_DTYPE_UNKNOWN;
    }
}

/* aura dtype → halide_type_t（from_halide 的逆，建 _Input 时用）。 */
halide_type_t to_halide(aura_dtype_t dt)
{
    switch (dt) {
    case AURA_DTYPE_F32: return halide_type_of<float>();
    case AURA_DTYPE_F16: return halide_type_t(halide_type_float, 16);
    case AURA_DTYPE_S8:  return halide_type_of<int8_t>();
    case AURA_DTYPE_U8:  return halide_type_of<uint8_t>();
    case AURA_DTYPE_S32: return halide_type_of<int32_t>();
    case AURA_DTYPE_S64: return halide_type_of<int64_t>();
    default:             return halide_type_of<float>();
    }
}

/* 各维乘积；含动态维（<0）返回 0。 */
uint64_t dims_elements(const int32_t *dims, int32_t dim_count)
{
    uint64_t elems = 1;
    for (int32_t i = 0; i < dim_count; i++) {
        if (dims[i] < 0) {
            return 0;
        }
        elems *= (uint64_t)dims[i];
    }
    return elems;
}

/* ------------------------------------------------------ MODULE 张量辅助 */

/* 按 shape 建一个可写的输入 VARP。shape 里若含动态维用 1 占位 ——
 * 真实形状由调用方 aura_tensor_resize 决定（见文件头说明）。 */
Express::VARP make_input_var(const int32_t *dims, int32_t dim_count, aura_dtype_t dtype)
{
    std::vector<int> shape;
    for (int32_t i = 0; i < dim_count; i++) {
        shape.push_back(dims[i] < 0 ? 1 : dims[i]);
    }
    if (shape.empty()) {
        shape.push_back(1); /* 标量输入（silero 的 sr）也按 {1} 建，MNN 不接受空 shape */
    }
    return Express::_Input(shape, Express::NCHW, to_halide(dtype));
}

/* MODULE 形态下张量的权威形状在 VARP 上（resize / onForward 之后会变），
 * 回填缓存供 aura_tensor_bytes / aura_tensor_get_info 使用。 */
void refresh_info_from_var(aura_tensor_t *t)
{
    if (t->var == nullptr) {
        return;
    }
    const Express::Variable::Info *vi = t->var->getInfo();
    if (vi == nullptr || vi->dim.empty()) {
        return;
    }
    t->info.dim_count = (int32_t)vi->dim.size();
    for (int32_t i = 0; i < t->info.dim_count && i < AURA_TENSOR_MAX_DIMS; i++) {
        t->info.dims[i] = vi->dim[i];
    }
    t->info.dtype = from_halide(vi->type);
    t->info.bytes = dims_elements(t->info.dims, t->info.dim_count) *
                    aura_dtype_size(t->info.dtype);
}

/* VARP 的主机指针（写路径）。dtype 决定模板实例化 —— MNN 的 writeMap<T>
 * 返回的是按 T 解释的指针，类型不对会踩内存。 */
void *var_host_ptr(const Express::VARP &v, bool for_write)
{
    const Express::Variable::Info *vi = v->getInfo();
    if (vi == nullptr) {
        return NULL;
    }
    switch (vi->type.code) {
    case halide_type_float:
        return (vi->type.bits == 16)
                   ? (void *)(for_write ? v->writeMap<uint16_t>() : v->readMap<uint16_t>())
                   : (void *)(for_write ? v->writeMap<float>() : v->readMap<float>());
    case halide_type_int:
        if (vi->type.bits == 8) {
            return (void *)(for_write ? v->writeMap<int8_t>() : v->readMap<int8_t>());
        }
        if (vi->type.bits == 64) {
            return (void *)(for_write ? v->writeMap<int64_t>() : v->readMap<int64_t>());
        }
        return (void *)(for_write ? v->writeMap<int32_t>() : v->readMap<int32_t>());
    case halide_type_uint:
        return (void *)(for_write ? v->writeMap<uint8_t>() : v->readMap<uint8_t>());
    default:
        return NULL;
    }
}

} /* namespace */

/* ------------------------------------------------------------ 生命周期 */

aura_err_t aura_infer_init(const aura_infer_cfg_t *cfg)
{
    if (cfg == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    if (g_state.inited) {
        return AURA_ERR_EXIST;
    }
    g_state.cfg   = *cfg;
    g_state.sched = make_sched(cfg);

    /* SESSION 路径的全局共享 Runtime：所有 session 的内存都在这一个池里调度。 */
    g_state.runtime = Interpreter::createRuntime({g_state.sched});
    if (g_state.runtime.first.empty() && g_state.runtime.second == nullptr) {
        AURA_LOGE(TAG, "createRuntime failed");
        return AURA_ERR_FAIL;
    }
    /* MODULE 路径的全局共享 RuntimeManager，配置同上（同一个 sched）。 */
    g_state.rtmgr.reset(
        Express::Executor::RuntimeManager::createRuntimeManager(g_state.sched));
    if (g_state.rtmgr == nullptr) {
        AURA_LOGE(TAG, "createRuntimeManager failed");
        return AURA_ERR_FAIL;
    }
    g_state.inited = true;
    AURA_LOGI(TAG, "MNN %s ready (threads=%d, precision=%s, memory=%s)", MNN::getVersion(),
              g_state.sched.numThread, cfg->precision_low ? "low" : "normal",
              cfg->memory_low ? "low" : "normal");
    return AURA_OK;
}

void aura_infer_deinit(void)
{
    g_state.runtime.first.clear();
    g_state.runtime.second.reset();
    g_state.rtmgr.reset();
    g_state.inited = false;
}

bool aura_infer_ready(void)
{
    return g_state.inited;
}

const char *aura_infer_version(void)
{
    return MNN::getVersion();
}

/* ------------------------------------------------------------ 张量信息 */

static void fill_tensor_info(const MNN::Tensor *t, aura_tensor_info_t *out, bool is_input,
                             int32_t index, const char *name)
{

    memset(out, 0, sizeof(*out));
    snprintf(out->name, sizeof(out->name), "%s", (name != NULL) ? name : "");
    out->is_input  = is_input;
    out->index     = index;
    out->dtype     = from_halide(t->getType());
    out->dim_count = t->dimensions();
    uint64_t elems = 1;
    for (int i = 0; i < out->dim_count && i < AURA_TENSOR_MAX_DIMS; i++) {
        out->dims[i] = t->length(i);
        if (out->dims[i] < 0) {
            elems = 0; /* 动态维 */
            break;
        }
        elems *= (uint64_t)out->dims[i];
    }
    uint32_t esz = aura_dtype_size(out->dtype);
    out->bytes = (elems != 0) ? elems * esz : 0;
}

/* ------------------------------------------------------------ 模型加载 */

/* SESSION：Interpreter + Session。算子表里没有控制流的模型走这条。 */
static bool load_session(aura_infer_model_t *m, const char *path)
{
    m->net.reset(Interpreter::createFromFile(path));
    if (m->net == nullptr) {
        AURA_LOGE(TAG, "createFromFile failed: %s", path);
        return false;
    }
    m->session = m->net->createSession(g_state.sched, g_state.runtime);
    if (m->session == NULL) {
        AURA_LOGE(TAG, "createSession failed: %s", path);
        return false;
    }
    m->net->resizeSession(m->session);

    int32_t idx = 0;
    for (const auto &kv : m->net->getSessionInputAll(m->session)) {
        /* 值初始化而不是 memset：aura_tensor 含 VARP(shared_ptr)，memset 会
         * 留下非空垃圾指针，析构时崩。 */
        aura_tensor_t t{};
        t.model    = m;
        t.is_input = true;
        t.index    = idx++;
        t.mt       = kv.second;
        snprintf(t.name, sizeof(t.name), "%s", kv.first.c_str());
        fill_tensor_info(t.mt, &t.info, true, t.index, t.name);
        m->inputs.push_back(t);
    }
    idx = 0;
    for (const auto &kv : m->net->getSessionOutputAll(m->session)) {
        /* 值初始化而不是 memset：aura_tensor 含 VARP(shared_ptr)，memset 会
         * 留下非空垃圾指针，析构时崩。 */
        aura_tensor_t t{};
        t.model    = m;
        t.is_input = false;
        t.index    = idx++;
        t.mt       = kv.second;
        snprintf(t.name, sizeof(t.name), "%s", kv.first.c_str());
        fill_tensor_info(t.mt, &t.info, false, t.index, t.name);
        m->outputs.push_back(t);
    }
    return true;
}

/* MODULE：Express::Module。带 If/While 子图的模型只能走这条，见文件头说明。
 * 输入张量的形状以模型声明为准（可能含 -1），先建占位 VARP；输出张量要到
 * aura_model_run 之后才有形状，加载期只登记名字。 */
static bool load_module(aura_infer_model_t *m, const char *path)
{
    Express::Module::Config cfg;
    cfg.rearrange = true; /* 与 sherpa-mnn GetSessionOptionsImpl 一致 */

    m->module.reset(Express::Module::load({}, {}, path, g_state.rtmgr, &cfg));
    if (m->module == nullptr) {
        AURA_LOGE(TAG, "Module::load failed: %s", path);
        return false;
    }
    const Express::Module::Info *mi = m->module->getInfo();
    if (mi == nullptr) {
        AURA_LOGE(TAG, "module getInfo failed: %s", path);
        return false;
    }

    for (size_t i = 0; i < mi->inputNames.size(); i++) {
        /* 值初始化而不是 memset：aura_tensor 含 VARP(shared_ptr)，memset 会
         * 留下非空垃圾指针，析构时崩。 */
        aura_tensor_t t{};
        t.model    = m;
        t.is_input = true;
        t.index    = (int32_t)i;
        snprintf(t.name, sizeof(t.name), "%s", mi->inputNames[i].c_str());
        snprintf(t.info.name, sizeof(t.info.name), "%s", t.name);
        t.info.is_input = true;
        t.info.index    = t.index;
        if (i < mi->inputs.size()) {
            const Express::Variable::Info &vi = mi->inputs[i];
            t.info.dim_count = (int32_t)vi.dim.size();
            for (int32_t d = 0; d < t.info.dim_count && d < AURA_TENSOR_MAX_DIMS; d++) {
                t.info.dims[d] = vi.dim[d];
            }
            t.info.dtype = from_halide(vi.type);
            /* 含动态维时为 0：告诉调用方必须先 aura_tensor_resize */
            t.info.bytes = dims_elements(t.info.dims, t.info.dim_count) *
                           aura_dtype_size(t.info.dtype);
        }
        t.var = make_input_var(t.info.dims, t.info.dim_count, t.info.dtype);
        m->inputs.push_back(t);
    }

    for (size_t i = 0; i < mi->outputNames.size(); i++) {
        /* 值初始化而不是 memset：aura_tensor 含 VARP(shared_ptr)，memset 会
         * 留下非空垃圾指针，析构时崩。 */
        aura_tensor_t t{};
        t.model    = m;
        t.is_input = false;
        t.index    = (int32_t)i;
        snprintf(t.name, sizeof(t.name), "%s", mi->outputNames[i].c_str());
        snprintf(t.info.name, sizeof(t.info.name), "%s", t.name);
        t.info.is_input = false;
        t.info.index    = t.index;
        t.info.dtype    = AURA_DTYPE_UNKNOWN; /* run 之后由 refresh_info_from_var 填 */
        m->outputs.push_back(t);
    }
    return true;
}

aura_infer_model_t *aura_infer_load(const char *path, aura_infer_form_t form)
{
    if (path == NULL || path[0] == '\0') {
        return NULL;
    }
    if (!g_state.inited) {
        AURA_LOGE(TAG, "engine not initialized");
        return NULL;
    }
    if (form != AURA_INFER_FORM_SESSION && form != AURA_INFER_FORM_MODULE) {
        AURA_LOGE(TAG, "form '%s' not supported in Phase 1 (path=%s)",
                  aura_infer_form_name(form), path);
        return NULL;
    }

    uint64_t t0 = aura_osal_time_us();

    std::unique_ptr<aura_infer_model_t> m(new aura_infer_model_t());
    snprintf(m->path, sizeof(m->path), "%s", path);
    m->form    = form;
    m->session = NULL;

    bool ok = (form == AURA_INFER_FORM_SESSION) ? load_session(m.get(), path)
                                                : load_module(m.get(), path);
    if (!ok) {
        return NULL;
    }

    /* 运行时内存（MB → bytes） */
    float mem_mb = 0.f;
    if (form == AURA_INFER_FORM_SESSION
            ? (m->net->getSessionInfo(m->session, MNN::Interpreter::MEMORY, &mem_mb) &&
               mem_mb > 0)
            : (g_state.rtmgr->getInfo(MNN::Interpreter::MEMORY, &mem_mb) && mem_mb > 0)) {
        m->info.runtime_bytes = (uint64_t)(mem_mb * 1024.f * 1024.f);
    }
    m->info.input_count  = (int32_t)m->inputs.size();
    m->info.output_count = (int32_t)m->outputs.size();
    m->info.form         = form;
    m->info.load_us      = aura_osal_time_us() - t0;
    snprintf(m->info.path, sizeof(m->info.path), "%s", path);

    /* 文件大小（权重账） */
    FILE *fp = fopen(path, "rb");
    if (fp != NULL) {
        fseek(fp, 0, SEEK_END);
        m->info.weight_bytes = (uint64_t)ftell(fp);
        fclose(fp);
    }

    AURA_LOGI(TAG, "loaded %s: in=%d out=%d runtime=%llu MB load=%llu ms",
              path, m->info.input_count, m->info.output_count,
              (unsigned long long)(m->info.runtime_bytes >> 20),
              (unsigned long long)(m->info.load_us / 1000));

    engine_models().push_back(m.get());
    return m.release();
}

aura_err_t aura_infer_unload(aura_infer_model_t *model)
{
    if (model == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    engine_models().erase(std::remove(engine_models().begin(), engine_models().end(), model),
                          engine_models().end());
    delete model;
    return AURA_OK;
}

aura_err_t aura_model_get_info(const aura_infer_model_t *model, aura_model_info_t *out)
{
    if (model == NULL || out == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    *out = model->info;
    return AURA_OK;
}

int32_t aura_model_input_count(const aura_infer_model_t *model)
{
    return (model == NULL) ? 0 : (int32_t)model->inputs.size();
}

int32_t aura_model_output_count(const aura_infer_model_t *model)
{
    return (model == NULL) ? 0 : (int32_t)model->outputs.size();
}

/* 形状的权威来源按形态不同：SESSION 直接问 MNN 张量 —— resizeSession 会连带
 * 改掉别的张量的形状（例如 input 定形后 state 的 batch 维才落地），缓存会过期；
 * MODULE 用缓存 info（VARP 的 info 在 resize / run 之后回填）。 */
static void tensor_info_now(const aura_tensor_t *t, aura_tensor_info_t *out)
{
    if (t->model->form == AURA_INFER_FORM_MODULE) {
        *out = t->info;
    } else {
        fill_tensor_info(t->mt, out, t->is_input, t->index, t->name);
    }
}

aura_err_t aura_model_input_info(const aura_infer_model_t *model, int32_t idx,
                                 aura_tensor_info_t *out)
{
    if (model == NULL || out == NULL || idx < 0 || (size_t)idx >= model->inputs.size()) {
        return AURA_ERR_INVALID_ARG;
    }
    tensor_info_now(&model->inputs[idx], out);
    return AURA_OK;
}

aura_err_t aura_model_output_info(const aura_infer_model_t *model, int32_t idx,
                                  aura_tensor_info_t *out)
{
    if (model == NULL || out == NULL || idx < 0 || (size_t)idx >= model->outputs.size()) {
        return AURA_ERR_INVALID_ARG;
    }
    tensor_info_now(&model->outputs[idx], out);
    return AURA_OK;
}

aura_tensor_t *aura_model_input(aura_infer_model_t *model, const char *name)
{
    if (model == NULL) {
        return NULL;
    }
    for (auto &t : model->inputs) {
        if (name == NULL || strcmp(t.name, name) == 0) {
            return &t;
        }
    }
    return NULL;
}

aura_tensor_t *aura_model_output(aura_infer_model_t *model, const char *name)
{
    if (model == NULL) {
        return NULL;
    }
    for (auto &t : model->outputs) {
        if (name == NULL || strcmp(t.name, name) == 0) {
            return &t;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------ 张量 I/O */

aura_err_t aura_tensor_resize(aura_tensor_t *t, const int32_t *dims, int32_t dim_count)
{
    if (t == NULL || t->model == NULL || dims == NULL || dim_count <= 0 ||
        dim_count > AURA_TENSOR_MAX_DIMS) {
        return AURA_ERR_INVALID_ARG;
    }
    for (int32_t i = 0; i < dim_count; i++) {
        if (dims[i] <= 0) {
            AURA_LOGE(TAG, "resize '%s': dim[%d]=%d must be > 0", t->name, (int)i, (int)dims[i]);
            return AURA_ERR_INVALID_ARG;
        }
    }

    if (t->model->form == AURA_INFER_FORM_MODULE) {
        /* Express 的张量形状在 VARP 上，改形状 = 换一个 VARP。只有输入会 resize：
         * 输出的形状由 onForward 决定，改它没有意义。 */
        if (!t->is_input) {
            return AURA_ERR_INVALID_ARG;
        }
        t->var = make_input_var(dims, dim_count, t->info.dtype);
        if (t->var == nullptr) {
            return AURA_ERR_FAIL;
        }
        /* 声明形状里的动态维在 VARP 上是占位值，resize 之后以新形状为准。 */
        t->info.dim_count = dim_count;
        for (int32_t i = 0; i < dim_count && i < AURA_TENSOR_MAX_DIMS; i++) {
            t->info.dims[i] = dims[i];
        }
        t->info.bytes = dims_elements(t->info.dims, t->info.dim_count) *
                        aura_dtype_size(t->info.dtype);
        return AURA_OK;
    }

    if (t->mt == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    std::vector<int> shape;
    for (int32_t i = 0; i < dim_count; i++) {
        shape.push_back(dims[i]);
    }
    t->model->net->resizeTensor(t->mt, shape);
    t->model->net->resizeSession(t->model->session, 1); /* 1 = 强制重新分配 */
    fill_tensor_info(t->mt, &t->info, t->is_input, t->index, t->name);
    return AURA_OK;
}

/* halide 类型 → host 指针（写路径）。 */
static void *tensor_host_ptr(MNN::Tensor *mt)
{
    halide_type_t ht = mt->getType();
    switch (ht.code) {
    case halide_type_float:
        return (ht.bits == 16) ? (void *)mt->host<uint16_t>() : (void *)mt->host<float>();
    case halide_type_int:
        if (ht.bits == 8) {
            return mt->host<int8_t>();
        }
        if (ht.bits == 64) {
            return mt->host<int64_t>();
        }
        return mt->host<int32_t>();
    case halide_type_uint:
        return mt->host<uint8_t>();
    default:
        return NULL;
    }
}

/* 主机指针按形态取；for_write 只影响 MODULE（readMap/writeMap 是两个入口）。 */
static void *tensor_host_ptr_now(const aura_tensor_t *t, bool for_write)
{
    if (t->model->form == AURA_INFER_FORM_MODULE) {
        if (t->var == nullptr) {
            return NULL;
        }
        return var_host_ptr(t->var, for_write);
    }
    return tensor_host_ptr(t->mt);
}

aura_err_t aura_tensor_write(aura_tensor_t *t, const void *data, uint64_t bytes)
{
    if (t == NULL || t->model == NULL || data == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    uint64_t want = aura_tensor_bytes(t);
    if (want == 0) {
        /* 含动态维（-1）：必须先 aura_tensor_resize 再写。 */
        return AURA_ERR_INVALID_ARG;
    }
    if (bytes != want) {
        AURA_LOGE(TAG, "write '%s': size mismatch want=%llu got=%llu", t->name,
                  (unsigned long long)want, (unsigned long long)bytes);
        return AURA_ERR_INVALID_ARG;
    }
    void *dst = tensor_host_ptr_now(t, true);
    if (dst == NULL) {
        AURA_LOGE(TAG, "write: unsupported tensor dtype on %s", t->name);
        return AURA_ERR_UNSUPPORTED;
    }
    memcpy(dst, data, (size_t)bytes);
    return AURA_OK;
}

aura_err_t aura_tensor_read(const aura_tensor_t *t, void *out, uint64_t bytes)
{
    if (t == NULL || t->model == NULL || out == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    uint64_t want = aura_tensor_bytes(t);
    if (want == 0 || bytes != want) {
        return AURA_ERR_INVALID_ARG;
    }
    const void *src = tensor_host_ptr_now(t, false);
    if (src == NULL) {
        return AURA_ERR_UNSUPPORTED;
    }
    memcpy(out, src, (size_t)bytes);
    return AURA_OK;
}

uint64_t aura_tensor_bytes(const aura_tensor_t *t)
{
    if (t == NULL || t->model == NULL) {
        return 0;
    }
    if (t->model->form == AURA_INFER_FORM_MODULE) {
        return t->info.bytes;
    }
    if (t->mt == NULL) {
        return 0;
    }
    uint64_t elems = 1;
    for (int i = 0; i < t->mt->dimensions(); i++) {
        int len = t->mt->length(i);
        if (len < 0) {
            return 0; /* 动态维未定型 */
        }
        elems *= (uint64_t)len;
    }
    return elems * aura_dtype_size(from_halide(t->mt->getType()));
}

aura_err_t aura_tensor_get_info(const aura_tensor_t *t, aura_tensor_info_t *out)
{
    if (t == NULL || t->model == NULL || out == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    tensor_info_now(t, out);
    return AURA_OK;
}

aura_dtype_t aura_tensor_dtype(const aura_tensor_t *t)
{
    if (t == NULL || t->model == NULL) {
        return AURA_DTYPE_UNKNOWN;
    }
    if (t->model->form == AURA_INFER_FORM_MODULE) {
        return t->info.dtype;
    }
    if (t->mt == NULL) {
        return AURA_DTYPE_UNKNOWN;
    }
    return from_halide(t->mt->getType());
}

/* ------------------------------------------------------------ 前向执行 */

aura_err_t aura_model_run(aura_infer_model_t *model)
{
    if (model == NULL) {
        return AURA_ERR_INVALID_ARG;
    }

    if (model->form == AURA_INFER_FORM_MODULE) {
        if (model->module == nullptr) {
            return AURA_ERR_INVALID_ARG;
        }
        /* 输入必须按模型声明的名字顺序给（Express 的 onForward 是位置参数，
         * 顺序取自 getInfo()->inputNames，加载时就按这个顺序建的 inputs）。 */
        std::vector<Express::VARP> ins;
        ins.reserve(model->inputs.size());
        for (auto &t : model->inputs) {
            if (t.var == nullptr) {
                AURA_LOGE(TAG, "input '%s' has no tensor (%s)", t.name, model->path);
                return AURA_ERR_MODEL;
            }
            ins.push_back(t.var);
        }
        std::vector<Express::VARP> outs = model->module->onForward(ins);
        if (outs.size() != model->outputs.size()) {
            AURA_LOGE(TAG, "onForward returned %d outputs, expected %d (%s)", (int)outs.size(),
                      (int)model->outputs.size(), model->path);
            return AURA_ERR_MODEL;
        }
        for (size_t i = 0; i < outs.size(); i++) {
            /* 输出 VARP 由 Module 持有，我们只保留引用；形状在 run 之后才定型。 */
            model->outputs[i].var = outs[i];
            refresh_info_from_var(&model->outputs[i]);
        }
        return AURA_OK;
    }

    if (model->session == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    MNN::ErrorCode ec = model->net->runSession(model->session);
    if (ec != MNN::NO_ERROR) {
        AURA_LOGE(TAG, "runSession failed: code=%d (%s)", (int)ec, model->path);
        return AURA_ERR_MODEL;
    }
    return AURA_OK;
}

/* ------------------------------------------------------------ 辅助函数 */

aura_err_t aura_infer_memory_stats(uint64_t *current_bytes, uint64_t *peak_bytes)
{
    if (current_bytes == NULL || peak_bytes == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    uint64_t total = 0;
    bool have_module = false;
    for (auto *m : engine_models()) {
        if (m->form == AURA_INFER_FORM_MODULE) {
            have_module = true; /* 所有 Module 共用一份 RuntimeManager，最后只加一次 */
            continue;
        }
        if (m->session == NULL) {
            continue;
        }
        float mem_mb = 0.f;
        if (m->net->getSessionInfo(m->session, MNN::Interpreter::MEMORY, &mem_mb) &&
            mem_mb > 0) {
            total += (uint64_t)(mem_mb * 1024.f * 1024.f);
        }
    }
    if (have_module && g_state.rtmgr != nullptr) {
        float mem_mb = 0.f;
        if (g_state.rtmgr->getInfo(MNN::Interpreter::MEMORY, &mem_mb) && mem_mb > 0) {
            total += (uint64_t)(mem_mb * 1024.f * 1024.f);
        }
    }
    *current_bytes = total;
    *peak_bytes    = total; /* MNN 不暴露峰值；用当前值近似，峰值账由 model_manager 维护 */
    return AURA_OK;
}

const char *aura_infer_form_name(aura_infer_form_t form)
{
    switch (form) {
    case AURA_INFER_FORM_SESSION: return "session";
    case AURA_INFER_FORM_MODULE:  return "module";
    case AURA_INFER_FORM_LLM:     return "llm";
    default:                      return "?";
    }
}

const char *aura_dtype_name(aura_dtype_t dtype)
{
    switch (dtype) {
    case AURA_DTYPE_F32:     return "f32";
    case AURA_DTYPE_F16:     return "f16";
    case AURA_DTYPE_S8:      return "s8";
    case AURA_DTYPE_U8:      return "u8";
    case AURA_DTYPE_S32:     return "s32";
    case AURA_DTYPE_S64:     return "s64";
    case AURA_DTYPE_UNKNOWN: return "?";
    default:                 return "?";
    }
}

uint32_t aura_dtype_size(aura_dtype_t dtype)
{
    switch (dtype) {
    case AURA_DTYPE_F32: return 4;
    case AURA_DTYPE_F16: return 2;
    case AURA_DTYPE_S8:  return 1;
    case AURA_DTYPE_U8:  return 1;
    case AURA_DTYPE_S32: return 4;
    case AURA_DTYPE_S64: return 8;
    default:             return 0;
    }
}

uint64_t aura_tensor_info_elements(const aura_tensor_info_t *info)
{
    if (info == NULL) {
        return 0;
    }
    uint64_t elems = 1;
    for (int32_t i = 0; i < info->dim_count; i++) {
        if (info->dims[i] < 0) {
            return 0;
        }
        elems *= (uint64_t)info->dims[i];
    }
    return elems;
}
