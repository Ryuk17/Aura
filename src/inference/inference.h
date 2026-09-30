/* Aura — 推理引擎适配层（对外纯 C 接口）
 *
 * 分层铁律（todo.md 第 3 节）：**算法模块只依赖本文件**，不直接 include MNN/NPU SDK
 * 头文件。因此这里用 C 接口把张量读写抽象出来，MNN 的 C++ API 被关在
 * engine_mnn/ 内部 —— 将来换 runtime（engine_ext/ 厂商 NPU）时算法代码零改动。
 *
 * 三种运行形态（model_requirement.md 第 1 节实测结论，缺一不可）：
 *   - SESSION ：普通 Session     → ASR / VAD / turn / 声纹
 *   - MODULE  ：Express::Module  → TTS（含 LSTM→While 子图，Session 建不出来）
 *   - LLM     ：MNN-LLM 管线     → LLM / Embedding（权重外置、按块加载）
 *
 * 内存策略（todo.md 4.4）：
 *   - 引擎级统一配置（线程数、后端、precision/memory 档位），全局一份；
 *   - 权重只读走 mmap 由引擎负责，运行期不额外拷贝；
 *   - 模型的常驻/按需由 model_manager 决定，不在本层做策略。
 */
#ifndef AURA_INFERENCE_INFERENCE_H
#define AURA_INFERENCE_INFERENCE_H

#include <stdbool.h>
#include <stdint.h>

#include "utils/error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ 引擎 */

typedef enum {
    AURA_INFER_BACKEND_CPU = 0, /* 默认：树莓派/PC 通用 */
    AURA_INFER_BACKEND_OPENCL,  /* 算力卡（后置） */
    AURA_INFER_BACKEND_VULKAN,  /* 算力卡（后置） */
    AURA_INFER_BACKEND_AUTO,    /* 由引擎自行选择 */
} aura_infer_backend_t;

typedef struct aura_infer_cfg {
    aura_infer_backend_t backend;
    int32_t  threads;        /* 计算线程数；<=0 表示用 CPU 核数 */
    bool     precision_low;  /* 低精度计算（fp16），端侧省内存/提速 */
    bool     memory_low;     /* 低内存模式（权重分块加载，LLM 必须） */
    uint64_t mem_pool_bytes; /* 预留：全局 MemoryPool 上限，0 = 引擎默认 */
} aura_infer_cfg_t;

/* 全局初始化。必须在使用任何模型接口前调用；重复调用返回 AURA_ERR_EXIST。
 * 不依赖 MNN 的构建（AURA_WITH_MNN=OFF）下返回 AURA_ERR_UNSUPPORTED。 */
aura_err_t aura_infer_init(const aura_infer_cfg_t *cfg);
void       aura_infer_deinit(void);
bool       aura_infer_ready(void);

/* 引擎版本字符串（MNN 版本），未初始化返回 "none"。 */
const char *aura_infer_version(void);

/* 全局内存统计：MNN MemoryPool 的当前/峰值占用（字节）。引擎不支持时返回 0。 */
aura_err_t aura_infer_memory_stats(uint64_t *current_bytes, uint64_t *peak_bytes);

/* ------------------------------------------------------------------ 模型 */

typedef enum {
    AURA_INFER_FORM_SESSION = 0, /* 普通 Session */
    AURA_INFER_FORM_MODULE,      /* Express::Module（含子图，如 TTS） */
    AURA_INFER_FORM_LLM,         /* MNN-LLM 管线（Phase 4） */
} aura_infer_form_t;

typedef struct aura_infer_model aura_infer_model_t;
typedef struct aura_tensor      aura_tensor_t;

typedef enum {
    AURA_DTYPE_F32 = 0,
    AURA_DTYPE_F16,
    AURA_DTYPE_S8,
    AURA_DTYPE_U8,
    AURA_DTYPE_S32,
    AURA_DTYPE_S64, /* int64：silero VAD 的 sr 标量等 */
    AURA_DTYPE_UNKNOWN,
} aura_dtype_t;

#ifndef AURA_TENSOR_MAX_DIMS
#define AURA_TENSOR_MAX_DIMS 8
#endif

typedef struct aura_tensor_info {
    char         name[64];
    aura_dtype_t dtype;
    int32_t      dims[AURA_TENSOR_MAX_DIMS];
    int32_t      dim_count;
    uint64_t     bytes; /* 当前形状下的字节数 */
    bool         is_input;
    int32_t      index;
} aura_tensor_info_t;

typedef struct aura_model_info {
    char     path[256];
    aura_infer_form_t form;
    int32_t  input_count;
    int32_t  output_count;
    uint64_t weight_bytes;  /* 权重文件大小（用于内存账） */
    uint64_t runtime_bytes; /* 引擎报告的运行时内存占用 */
    uint64_t load_us;       /* 加载耗时（首响延迟的重要组成） */
} aura_model_info_t;

/* 加载模型。form 决定用哪种运行形态；路径不存在返回 AURA_ERR_IO，
 * 形态不支持（如未编译 MNN-LLM）返回 AURA_ERR_UNSUPPORTED。 */
aura_infer_model_t *aura_infer_load(const char *path, aura_infer_form_t form);
aura_err_t          aura_infer_unload(aura_infer_model_t *model);

aura_err_t aura_model_get_info(const aura_infer_model_t *model, aura_model_info_t *out);

int32_t    aura_model_input_count(const aura_infer_model_t *model);
int32_t    aura_model_output_count(const aura_infer_model_t *model);
aura_err_t aura_model_input_info(const aura_infer_model_t *model, int32_t idx,
                                 aura_tensor_info_t *out);
aura_err_t aura_model_output_info(const aura_infer_model_t *model, int32_t idx,
                                  aura_tensor_info_t *out);

/* 按名字取张量；name 为 NULL 时取第 0 个。找不到返回 NULL。 */
aura_tensor_t *aura_model_input(aura_infer_model_t *model, const char *name);
aura_tensor_t *aura_model_output(aura_infer_model_t *model, const char *name);

/* ---------------------------------------------------------------- 张量 I/O */

/* 写入张量数据（frames/样本，无需关心底层是 device 还是 host 内存）。
 * bytes 必须等于 aura_tensor_bytes。 */
aura_err_t aura_tensor_write(aura_tensor_t *t, const void *data, uint64_t bytes);

/* 读取张量数据（run 之后调用）。 */
aura_err_t aura_tensor_read(const aura_tensor_t *t, void *out, uint64_t bytes);

/* 改变形状（流式模型的 T 维、VAD 的变长输入都依赖它）。
 * 会触发 MNN 的 resizeTensor/resizeSession。 */
aura_err_t aura_tensor_resize(aura_tensor_t *t, const int32_t *dims, int32_t dim_count);

uint64_t     aura_tensor_bytes(const aura_tensor_t *t);
aura_err_t   aura_tensor_get_info(const aura_tensor_t *t, aura_tensor_info_t *out);
aura_dtype_t aura_tensor_dtype(const aura_tensor_t *t);

/* 执行一次前向。所有已 write 的输入会被提交，输出在 run 后可直接 read。 */
aura_err_t aura_model_run(aura_infer_model_t *model);

/* -------------------------------------------------------------- 辅助函数 */

const char *aura_infer_form_name(aura_infer_form_t form);
const char *aura_dtype_name(aura_dtype_t dtype);

/* 元素个数（各维乘积；含 -1 动态维时返回 0）。 */
uint64_t aura_tensor_info_elements(const aura_tensor_info_t *info);

/* dtype 的字节宽度。 */
uint32_t aura_dtype_size(aura_dtype_t dtype);

#ifdef __cplusplus
}
#endif

#endif /* AURA_INFERENCE_INFERENCE_H */
