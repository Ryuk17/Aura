/* Aura — DSP 算法适配模板（TrickRoom 一族的通用接入层）
 *
 * 问题：TrickRoom 有 12 个算法，但它们的 C API 是**同构不同形**的 ——
 * 生命周期完全一致（Create/Init/SetParam/ResetParam/Process/Deinit/Reset/Destroy），
 * 差异只在两处：Process 的签名（1→1 / 2→1 / N→1+flag / 1→0 / 变帧率）和
 * InitConfig/RtConfig 的字段。
 *
 * 结论：没有必要为每个算法写一个适配器，只需要**一张描述表 + 一组 shim**。
 * 本文件把这个共形部分固化下来：
 *
 *   - 形变（Process 签名差异）由描述表里的 `process` 函数适配 —— 每个算法写一个
 *     5 行的静态 shim 转成统一的最宽签名。**不做函数指针强转**：不同原型之间
 *     强转在多数 ABI 上能跑，但它依赖调用约定恰好兼容，属于"换编译器就炸"的
 *     那类技巧，不值得省这 5 行。
 *
 *   - 配置（InitConfig/RtConfig 的字段）由描述表里的 `build_init_cfg` /
 *     `build_rt_cfg` 填充：**先铺默认值，再用 extra 参数按名覆盖**。参数名与
 *     引擎字段的对应关系只有算法自己知道，框架不猜。
 *
 *   - 其余全部共享：攒帧、多通道、参考流对齐、emit、事件边沿检测、错误映射、
 *     统计。这层是本文件的主体（aura_dsp_adapter.c）。
 *
 * 分层：本层在 core 之上（依赖 core/algorithm.h），TrickRoom 头**只出现在**
 * dsp/trickroom/ 下的描述表里 —— 换 3A 引擎时只改那一张表。
 */
#ifndef AURA_DSP_ADAPTER_H
#define AURA_DSP_ADAPTER_H

#include <stdbool.h>
#include <stdint.h>

#include "core/algorithm.h"
#include "core/node.h"
#include "utils/error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================== Process 形态 ==============================
 * 五种形态覆盖了 TrickRoom 现有 12 个算法，也覆盖了绝大多数音频算法：
 * 若将来出现"多路音频分别输出"这类新形态，加枚举值 + 适配分支，而不是改契约。 */
typedef enum {
    AURA_DSP_IO_1TO1 = 0,  /* NS/AGC/AGC2/HS/IE/TS/DR：in → out，同帧长 */
    AURA_DSP_IO_2TO1,      /* AEC/AECM：near + ref → out（caps.consumes_ref） */
    AURA_DSP_IO_NIN1_FLAG, /* BF：N 通道 → 1 通道 + 目标存在标志 */
    AURA_DSP_IO_1TO0,      /* VAD：in → 标志，不产出音频（观察者） */
    AURA_DSP_IO_RESAMPLE,  /* SRC：变采样率（caps.changes_frame_rate） */
} aura_dsp_io_kind_t;

/* 统一的最宽 Process 签名：不适用的参数由 shim 忽略。
 *   in/ref  交错 int16；in_samples 为**单通道样本数**（TrickRoom 口径）
 *   out/max_out_samples 同理；out_samples 由引擎写回
 *   flag    仅 1TO0 / NIN1_FLAG 使用（VAD 语音标志 / BF 目标存在）
 * 返回 0 成功，非 0 为引擎错误码（TrickRoom 语义）。 */
typedef int (*aura_dsp_process_fn)(void *h, const int16_t *in, const int16_t *ref, int in_samples,
                                   int16_t *out, int max_out_samples, int *out_samples,
                                   int *flag);

/* ============================== 描述表 ============================== */

typedef struct aura_dsp_desc {
    /* ---- 由 aura_dsp_finalize() 依下列字段填好，注册时直接用 ---- */
    aura_algo_desc_t algo;

    const char        *name; /* 注册名（配置里 chain = <name> 用它） */
    aura_dsp_io_kind_t io_kind;

    /* 引擎状态码 → aura_err_t。**由引擎侧提供**（TrickRoom 表里实现），
     * 因为状态码是引擎的 ABI：在这里硬编码一份等价表，等于把它的枚举值抄了一遍，
     * 引擎改了值这边不会报错，只会悄悄分类错。留 NULL 则退化为
     * "0 = 成功，非 0 = AURA_ERR_DSP"。 */
    aura_err_t (*map_status)(int status);

    /* ---- 生命周期（TrickRoom 全族同形；不需要的直接留 NULL） ---- */
    void *(*create)(void);
    int (*destroy)(void *h);
    int (*init)(void *h, const void *cfg);
    int (*set_param)(void *h, const void *cfg);
    int (*reset_param)(void *h, const void *cfg);
    int (*deinit)(void *h);
    int (*reset)(void *h);

    aura_dsp_process_fn process; /* 必需 */

    /* ---- 参数规格：声明本算法认哪些 extra 键 ----
     * finalize 会把它接到 algo.param_specs 上，于是配置里拼错键名会在装配期
     * 报错，而不是静默变成"改了配置没生效"。可空 = 不校验。 */
    const aura_algo_param_spec_t *param_specs;
    uint32_t                      param_spec_count;

    /* ---- 配置填充：params → 引擎的 InitConfig/RtConfig ----
     * 实现约定：先把 cfg 填成该算法的默认值，再对 extra 里出现的键覆盖。
     * cap 是 cfg 的字节容量（防越界写）。可空 = 该算法无需配置。 */
    aura_err_t (*build_init_cfg)(const aura_algo_params_t *p, void *cfg, uint32_t cap);
    uint32_t   init_cfg_size;
    aura_err_t (*build_rt_cfg)(const aura_algo_params_t *p, void *cfg, uint32_t cap);
    uint32_t   rt_cfg_size;

    /* ---- 标志输出的边沿事件（VAD 类）----
     * flag 0→1 发 rise_type，1→0 发 fall_type；类型为 AURA_EVENT_NONE 表示不发。
     * 只发边沿而非每帧：每帧发事件会刷爆总线，且下游要的本来就是状态变化。 */
    int flag_rise_type; /* aura_event_type_t */
    int flag_fall_type;

    /* ---- 降级策略 ----
     * Process 返回错误时**丢弃**该帧（默认 false = 把原始输入原样透传给下游）。
     * 取"默认透传"是因为 3A 出问题时"没降噪的语音"仍远好于"静音"，链路不该
     * 因为一个降噪器挂掉而断掉；字段取反命名（drop 而非 passthrough）正是为了让
     * 零值即安全默认 —— 描述表忘了写这一项时，行为是透传而不是丢帧。 */
    bool drop_on_error;
} aura_dsp_desc_t;

/* 依 name/io_kind 填好内嵌的 algo 描述（io 能力、create、shape_out）。
 * 描述表在 static const 里无法调用函数，因此每个表在注册前**必须**逐条调用一次。
 * 返回 AURA_ERR_INVALID_ARG 表示表本身写错了（缺 process、缺 name 等）。 */
aura_err_t aura_dsp_finalize(aura_dsp_desc_t *desc);

/* 批量 finalize + 注册（注册表见 core/algorithm.h）。 */
aura_err_t aura_dsp_register_all(aura_dsp_desc_t *descs, uint32_t count);

/* TrickRoom 描述表（定义在 dsp/trickroom/aura_dsp_trickroom.c）：
 * 先调一次拿到表与条数，再 aura_dsp_register_all 注册。
 * 只在 AURA_WITH_TRICKROOM=ON 时编入。 */
aura_dsp_desc_t *aura_dsp_trickroom_descs(uint32_t *count);

/* ============================== 统计 ============================== */

typedef struct aura_dsp_stats {
    uint64_t frames_in;      /* 投喂给引擎的帧数（攒满一帧算一帧） */
    uint64_t frames_out;     /* 引擎产出的帧数 */
    uint64_t samples_in;     /* 累计输入样本（单通道） */
    uint64_t process_errors; /* 引擎返回非 0 的次数 */
    uint64_t ref_missed;     /* AEC：参考流不足、以静音顶替的次数 */
    uint64_t flag_rise;      /* 标志 0→1 次数 */
    uint64_t flag_fall;
} aura_dsp_stats_t;

/* 读节点统计（node 非本适配器创建时返回 AURA_ERR_INVALID_ARG）。 */
aura_err_t aura_dsp_get_stats(const aura_node_t *node, aura_dsp_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* AURA_DSP_ADAPTER_H */
