/* Aura — 统一算法接口：提供者契约 + 注册表 + 链组装
 *
 * 目的（todo.md 第 3 节"算法统一接口"，Phase 2 前置）：
 * 让 TrickRoom 的 3A/BSS、MNN 的 KWS/ASR/LLM/TTS、以及将来任何新算法，都通过
 * **同一套接口**接入 pipeline，从而使链可以自由组合、按名字装配、按配置裁剪。
 *
 * 三层结构（本文档定义前两层，第三层在各适配器里）：
 *
 *   ① 提供者契约 aura_algo_desc_t —— 算法只回答两个问题：
 *      "我的流形状是什么"（aura_algo_io_t）和"怎么把我造出来"（create 工厂）。
 *      没有第三个问题：算法不知道 pipeline、bus、其它节点的存在。
 *
 *   ② 注册表 —— 名字 → 描述表。装配时按名字取算法，不出现函数符号硬编码，
 *      于是"换一个 KWS"是从配置文件改一个词，而不是改一行 include。
 *
 *   ③ 适配器（dsp/aura_dsp_adapter、algorithm/aura_nn_adapter）—— 把具体引擎
 *      的 API 形状（TrickRoom 的句柄族 / MNN 的会话）翻译成契约。
 *
 * ---------------------------------------------------------------------------
 * 三种算法类型（aura_algo_kind_t）的区别只在**适配器怎么写**，不在契约：
 *
 *   DSP       同步、逐帧、无模型。TrickRoom 一族。
 *   NN_SYNC   有模型，单次推理耗时 ≲ 一帧时长，可在 process 内同步跑（KWS/声纹/VAD）。
 *   NN_ASYNC  单次推理远超一帧时长，必须抛子任务（ASR/LLM/TTS）。
 *             子任务**不得** emit_audio/emit_text（那些接口绑定节点 task 上下文），
 *             结果经私有队列 + 内部事件回到 on_event 再产出。见 aura_nn_adapter.h。
 *
 * ---------------------------------------------------------------------------
 * 分层注意：本文件属于 core，**只能依赖 core 及以下**。
 * 因此这里不做任何全局注册动作（register_all 会反向依赖 dsp/算法模块）。
 * 装配层（agent_core / host_sim / 测试）负责按裁剪开关调用各族的 register_all，
 * 再调 aura_chain_build。core 提供空注册表与 reset。
 */
#ifndef AURA_CORE_ALGORITHM_H
#define AURA_CORE_ALGORITHM_H

#include <stdbool.h>
#include <stdint.h>

#include "core/node.h"
#include "core/pipeline/pipeline.h"
#include "utils/error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ========================= ① 参数 ========================= */

/* 算法类型标签：仅用于装配层的分类决策与日志，不影响契约本身。 */
typedef enum {
    AURA_ALGO_KIND_DSP = 0, /* 传统信号处理（TrickRoom 一族） */
    AURA_ALGO_KIND_NN_SYNC, /* 神经网络，单帧内同步推理 */
    AURA_ALGO_KIND_NN_ASYNC /* 神经网络，子任务异步推理 */
} aura_algo_kind_t;

typedef enum {
    AURA_ALGO_PARAM_I32 = 0,
    AURA_ALGO_PARAM_F32,
    AURA_ALGO_PARAM_BOOL,
    AURA_ALGO_PARAM_STR,     /* 生命周期由调用方保证（通常指向静态字符串） */
    AURA_ALGO_PARAM_F32_ARR, /* v.fa.v 指向调用方数组，不拷贝 */
} aura_algo_param_type_t;

typedef struct aura_algo_param {
    const char            *name;
    aura_algo_param_type_t type;
    union {
        int32_t i;
        float   f;
        bool    b;
        const char *s;
        struct {
            const float *v;
            uint32_t     n;
        } fa;
    } v;
} aura_algo_param_t;

#define AURA_ALGO_MAX_EXTRA_PARAMS 8

struct aura_algo_desc; /* 见下文提供者契约 */

/* 工厂入参。前三个字段是**链上传播后的生效形状**，不是全局配置 ——
 * 链中间的重采样节点会改变它们，chain_build 负责逐级算好再交给工厂。 */
typedef struct aura_algo_params {
    uint32_t sample_rate; /* 生效采样率 */
    uint32_t frame_ms;    /* 帧长（毫秒）。TrickRoom 硬约束 frame_ms*100 == sample_rate */
    uint32_t in_channels; /* 生效输入通道数（主音频流；不含 AEC 参考） */
    const char *model_path; /* 模型文件路径，可为 NULL（DSP 类不需要） */

    /* 节点实例名（链装配时给出，如同一算法在链上出现两次需要区分）。
     * **契约：工厂必须把它拷进自己的存储**并用作节点名 —— 它指向链描述，
     * 生命周期只到装配结束；直接存指针会在链释放后变成悬垂。 */
    const char *instance_name;

    /* 提供者自指：chain_build 在调用工厂前填入查到的描述表指针。
     * 用途是让**一族算法共用一个工厂**（适配器场景）：工厂从 params->provider
     * 取回自己的描述表，从而知道该造哪种节点。算法自身的处理逻辑不应依赖它，
     * 单算法工厂忽略即可。 */
    const struct aura_algo_desc *provider;

    uint32_t          extra_count;
    aura_algo_param_t extra[AURA_ALGO_MAX_EXTRA_PARAMS]; /* 算法私有参数 */
} aura_algo_params_t;

/* 参数写入：重名则覆盖（后写优先，便于"配置覆盖默认"）。返回 AURA_ERR_FULL
 * 表示 extra 槽位用尽 —— 这是装配期错误，不该被忽略。 */
aura_err_t aura_algo_params_set_i32(aura_algo_params_t *p, const char *name, int32_t v);
aura_err_t aura_algo_params_set_f32(aura_algo_params_t *p, const char *name, float v);
aura_err_t aura_algo_params_set_bool(aura_algo_params_t *p, const char *name, bool v);
aura_err_t aura_algo_params_set_str(aura_algo_params_t *p, const char *name, const char *v);
aura_err_t aura_algo_params_set_f32_array(aura_algo_params_t *p, const char *name,
                                          const float *v, uint32_t n);

/* 取参数；不存在返回 NULL。适配器用它读取可选调参项。 */
const aura_algo_param_t *aura_algo_params_get(const aura_algo_params_t *p, const char *name);

/* 类型化取值的便捷封装：缺省即返回 def。 */
int32_t     aura_algo_params_i32(const aura_algo_params_t *p, const char *name, int32_t def);
float       aura_algo_params_f32(const aura_algo_params_t *p, const char *name, float def);
bool        aura_algo_params_bool(const aura_algo_params_t *p, const char *name, bool def);
const char *aura_algo_params_str(const aura_algo_params_t *p, const char *name, const char *def);

/* ========================= ② 流形状声明 ========================= */

/* 算法对"我在链上是什么角色"的自述。chain_build 据此接线、传播形状、做校验。
 *
 * 一致性要求（描述表写错 = 装配期报错，而不是运行期丢帧）：
 *   produces_audio=true  的节点必须真的在 process 里 emit_audio；
 *   produces_audio=false 且 consumes_audio=true 的节点是**观察者或终点**：
 *     它在链中间时，帧从上一个生产者的**旁路**送给它，主线继续（见 pipeline.h 拓扑）。
 */
typedef struct aura_algo_io {
    bool consumes_audio;      /* 消费主音频流 */
    bool produces_audio;      /* 产出主音频流 */
    bool consumes_ref;        /* 消费 AEC 参考流（播放回采），全链至多一个 */
    bool consumes_text;
    bool produces_text;
    bool wants_events;        /* 订阅 event_bus */
    /* 本节点改变采样率/帧长（如 SRC）。置位后：
     *  - 该节点产出帧的 pts 不被 pipeline 覆盖（见 node.h 的 pts 契约）；
     *  - 必须有 shape_out 告诉下游新形状；
     *  - 全链至多一个（chain_build 强制）—— 两个变帧率节点串在一起，
     *    中间那段"是什么采样率"会失去锚点，链的可读性也随之崩掉。 */
    bool changes_frame_rate;
    /* 本节点改变通道数（如 BF：N 麦 → 1 波束）。必须有 shape_out。
     * 与 changes_frame_rate 分开是因为两者约束强度不同：通道数变化不改变时间轴，
     * 不涉及 pts，也就不受"全链至多一个"的限制（BF 与 SRC 可以共存）。 */
    bool changes_channels;
} aura_algo_io_t;

/* 流形状变换钩子：输入是已生效的样本率/通道数，输出写到 out（可只改其一）。
 * io.changes_frame_rate 或 io.changes_channels 为 true 时必须提供，否则
 * chain_build 报错。实现通常是读自己的 extra 参数（如 "out_sample_rate"），
 * 不碰硬件、不做实际转换 —— 它只回答"我会变成什么形状"。 */
typedef aura_err_t (*aura_algo_shape_fn)(const aura_algo_params_t *params, uint32_t *sample_rate,
                                         uint32_t *channels);

/* ========================= ③ 参数规格（可选） ========================= */

/* 声明算法支持哪些 extra 键。提供后 chain_build 会校验未知键 ——
 * 拼错一个键名而参数静默失效，是最难查的一类配置 bug。 */
typedef struct aura_algo_param_spec {
    const char            *name;
    aura_algo_param_type_t type;
    float                  f_min; /* 数值型范围（仅用于越界告警，不做钳位） */
    float                  f_max;
    const char            *help;
} aura_algo_param_spec_t;

/* ========================= ④ 提供者契约 ========================= */

/* 工厂：造一个节点出来。节点结构体**必须一次分配、aura_node_t 放首位**
 * （pipeline 靠 free(node) 回收；资源释放走 ops->deinit）。
 * 失败时返回 NULL 并把原因写进 *err（err 可为 NULL）。 */
typedef aura_node_t *(*aura_algo_factory_fn)(const aura_algo_params_t *params, aura_err_t *err);

typedef struct aura_algo_desc {
    const char       *name;    /* 注册名：配置里 chain = aec, ns, ... 用的就是它 */
    uint32_t          version; /* 提供者版本，仅用于日志/诊断 */
    aura_algo_kind_t  kind;
    aura_algo_io_t    io;
    aura_algo_shape_fn shape_out; /* changes_frame_rate 时必需 */

    const aura_algo_param_spec_t *param_specs; /* 可为 NULL = 不校验 extra 键 */
    uint32_t                      param_spec_count;

    aura_algo_factory_fn create;
} aura_algo_desc_t;

/* ========================= ⑤ 注册表 ========================= */

/* 注册（通常在装配层 init 时一次性做完）。重名返回 AURA_ERR_EXIST ——
 * 静默覆盖会让"我改了配置怎么没生效"变成一个下午的谜题。 */
aura_err_t aura_algo_register(const aura_algo_desc_t *desc);

/* 批量注册到表尾（count 条），遇错即停并返回错误。 */
aura_err_t aura_algo_register_all(const aura_algo_desc_t *descs, uint32_t count);

const aura_algo_desc_t *aura_algo_find(const char *name);
const aura_algo_desc_t *aura_algo_at(uint32_t idx);
uint32_t                aura_algo_count(void);

/* 清空注册表（测试与重复 init 用）。 */
void aura_algo_reset(void);

/* ========================= ⑥ 链描述 ========================= */

#define AURA_CHAIN_MAX_LINKS 16
#define AURA_CHAIN_NAME_MAX  32

typedef struct aura_chain_link {
    char                algo[AURA_CHAIN_NAME_MAX]; /* 注册名 */
    char                name[AURA_CHAIN_NAME_MAX]; /* 节点名（缺省 = algo，同名算法复用时区分） */
    aura_algo_params_t  params;
    /* tee(...) 里列出的旁路观察者。chain_build 会校验它确实"只消费不产出"，
     * 顺序无意义 —— 观察者一律挂在最近的主链生产者上。 */
    bool                observer_hint;
} aura_chain_link_t;

typedef struct aura_chain {
    /* 链首输入形状。填 0 表示取 pipeline 的配置。 */
    uint32_t sample_rate;
    uint32_t frame_ms;
    uint32_t channels;

    aura_chain_link_t links[AURA_CHAIN_MAX_LINKS];
    uint32_t          count;
} aura_chain_t;

void aura_chain_init(aura_chain_t *chain);

/* 追加一个链元素（线性顺序）。name 为 NULL 时用 algo 名。
 * 返回 AURA_ERR_FULL 表示链长超限。 */
aura_err_t aura_chain_add(aura_chain_t *chain, const char *algo, const char *name);

/* 取链元素的参数区以便填参（越界返回 NULL）。 */
aura_algo_params_t *aura_chain_params(aura_chain_t *chain, uint32_t idx);

/* 解析链语法：`aec, ns, tee(vad, kws), asr`
 *  - 逗号分隔，元素为算法名或 tee(...)；
 *  - tee(...) 内以逗号分隔的算法被标记为旁路观察者；
 *  - 名字只做语法检查，是否已注册由 aura_chain_build 判断（本函数不依赖注册表）。
 * 语法里出现空白会被忽略。返回 AURA_ERR_INVALID_ARG 表示语法错误。 */
aura_err_t aura_chain_parse(const char *syntax, aura_chain_t *chain);

/* ========================= ⑦ 链组装 ========================= */

/* 按链描述建节点、接进 pipeline、传播流形状。
 *
 * 形状传播：从链首 (sample_rate, frame_ms, channels) 起逐级推进 ——
 * 每一步把当前生效形状填进该节点的 params，节点若声明 changes_frame_rate
 * 则调其 shape_out 算出新形状供下游使用。于是"链中间挡了个重采样"不需要
 * 上层手工告诉每个节点自己跑在什么采样率上。
 *
 * 校验（全部在装配期报错，不留到运行期）：
 *   - 算法名未注册 / tee 里的节点其实会产出音频；
 *   - 全链超过一个 changes_frame_rate 节点；
 *   - extra 里出现 param_specs 未声明的键（数值越界只告警）。
 *
 * out_nodes/out_count 可为 NULL。非 NULL 时返回本次建出的节点指针数组
 * （含按顺序的所有节点），由调用方用 aura_chain_destroy_nodes 回收。 */
aura_err_t aura_chain_build(aura_pipeline_t *p, const aura_chain_t *chain,
                            aura_node_t **out_nodes, uint32_t *out_count);

/* 释放 aura_chain_build 建出的节点（free 每个节点基址）。
 * **不**负责从 pipeline 摘除 —— 调用前应先 aura_pipeline_stop/destroy。 */
void aura_chain_destroy_nodes(aura_node_t **nodes, uint32_t count);

/* 类型名（日志用，永不返回 NULL）。 */
const char *aura_algo_kind_name(aura_algo_kind_t kind);

/* ========================= ⑧ 由配置组装链 ========================= */

/* 前向声明：本头不 include core/config/config.h（config 是核心配置，不该为了
 * 一个函数把 node/pipeline 的重头拉进每个使用者）。实现里才 include。 */
struct aura_config;

/* 解析 cfg->chain 语法、按 `chain_param_<algo>.<key>` 填每个节点的私有参数，
 * 产出可直接交给 aura_chain_build 的链描述。链首形状取 cfg 的
 * sample_rate / frame_ms / channels。
 *
 * 为什么类型转换在这里而不是在 config 解析器里：配置文件是纯文本，"这个键是
 * int 还是 float" 只有算法自己（param_specs）知道，而解析配置的时候注册表还
 * 没就绪。放在这一层，顺带把三类**静默失效**变成装配期报错：
 *   算法名没注册 → AURA_ERR_NOT_FOUND
 *   键不在 param_specs 里（多半是拼错）→ AURA_ERR_INVALID_ARG
 *   值不是声明的类型 → AURA_ERR_INVALID_ARG
 * 日志会指出是哪个算法、哪个键、哪个值。
 *
 * 前置：各算法族的 register_all 已调用（注册表就绪）。
 * cfg 的生命周期必须覆盖 aura_chain_build —— STR 型参数**不做拷贝**，直接指向
 * cfg 内部的缓冲（见 aura_algo_param_t 的字符串约定）。
 * cfg->chain 为空 = 空链：返回 AURA_OK 且 out->count == 0，不是错误。 */
aura_err_t aura_chain_from_config(const struct aura_config *cfg, aura_chain_t *out);

#ifdef __cplusplus
}
#endif

#endif /* AURA_CORE_ALGORITHM_H */
