/* Aura — NN 算法适配模板（MNN 一族的通用接入层）
 *
 * 与 dsp/aura_dsp_adapter.h 是**同一套契约的两种引擎形态**：算法作者都只回答
 * "我的流形状是什么"（aura_algo_io_t）和"怎么造我"（描述表 + 回调），
 * 其余（接进 pipeline、事件、统计、控制面）由适配层负责。
 * 区别只在"推理"这一步长什么样：
 *
 *   DSP  ：Create/Init/Process(h, in, out) —— 逐帧同步、无模型。
 *   NN   ：加载模型 → 定形张量 → 攒窗 → 前向 → 读输出。
 *          这一串在 TrickRoom 侧不存在，在 MNN 侧每个算法都要写一遍，
 *          本文件把它固化成模板，于是新增一个 NN 算法 = 填一张表 + 4 个回调。
 *
 * ---------------------------------------------------------------------------
 * 三种 io 形态（aura_nn_io_kind_t）：
 *
 *   OBSERVER     只消费音频、不产出音频（VAD / 声纹 / 轮次检测）。
 *                挂主线**旁路**，主线继续（见 pipeline.h 拓扑）。
 *   PASSTHROUGH  透传：输出 = 输入，判定结果走事件（KWS 闸门、VAD 过滤位）。
 *                在链上占一个位置，下游拿到的是未改动的 PCM。
 *   ASYNC_SOURCE 由子任务异步产出音频（TTS）。**本轮不实现**：finalize 会拒绝。
 *
 * ---------------------------------------------------------------------------
 * 同步 / 异步（aura_algo_kind_t）：
 *
 *   NN_SYNC   单次前向 ≲ 一帧时长（10ms），在 process_audio 里同步跑（KWS/VAD/声纹）。
 *             适配层统计单次前向耗时，超预算（> frame_ms）只告警不丢帧 ——
 *             丢帧会让"算法太慢"变成"音频断断续续"，更难查。
 *
 *   NN_ASYNC  单次前向远超一帧（ASR/LLM/TTS）。已实现（Phase 3 起）：
 *             process 只入队，子任务做推理，结果经**私有队列 + 内部事件**回到
 *             节点的 on_event 上下文再产出 —— 因为 emit_audio/emit_text 依赖
 *             节点 task 的 process 上下文（pts 字段非线程安全，见 node.h）。
 *             子任务线程里**不得**调用任何 emit_*，只能 aura_nn_post_result()。
 *
 *             任务流：frame_in 攒够一窗 → 模板把**算法状态开头 job_size 字节**
 *             （算法在 frame_in 里构造好的 job，含按值拷贝的数据）拷进任务队列
 *             → worker 线程弹 job：magic==0 = 复位哨兵（跑 algo_reset，与
 *             process 线程无并发），否则 async_run() → 结果走
 *             aura_nn_post_result()（私有结果队列 + NN_RESULT 事件）→ 模板在
 *             bus 分发任务里认领（payload = 节点名）→ result_consume()。
 *
 * ---------------------------------------------------------------------------
 * 分层：本层在 inference 之上（依赖 inference.h），与 dsp 适配层同层。
 * 算法模块看不到 MNN 头（那是 engine_mnn 的事），也看不到 pipeline 内部。
 */
#ifndef AURA_ALGORITHM_AURA_NN_ADAPTER_H
#define AURA_ALGORITHM_AURA_NN_ADAPTER_H

#include <stdbool.h>
#include <stdint.h>

#include "core/algorithm.h"
#include "core/node.h"
#include "inference/inference.h"
#include "utils/error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ============================== io 形态 ============================== */

typedef enum {
    AURA_NN_IO_OBSERVER = 0,/* 消费音频，不产出音频 */
    AURA_NN_IO_PASSTHROUGH, /* 消费音频，原样产出（判定走事件） */
    AURA_NN_IO_ASYNC_SOURCE,/* 异步产出音频（TTS）：接口预留，未实现 */
} aura_nn_io_kind_t;

/* ============================== 滑窗 ==============================
 * 流式 NN 的通用口径：每次前向吃 (ctx + new) 个样本，其中 ctx 是上一次窗口的
 * 尾部（silero v5 = 64 上文 + 512 新样本）。漏掉上文会让模型在句内/句间判错 ——
 * 这是"接上了但效果不对"的典型来源，所以框架提供这个工具，不给各算法自由发挥。
 *
 * 用法跨 frame_in / infer_out 两个回调（顺序不能变）：
 *
 *     // frame_in：攒窗 + 写输入张量，返回"够一窗了"
 *     const float *win = aura_nn_window_push(&s->win, pcm, n);
 *     if (win == NULL) return false;
 *     aura_tensor_write(input, win, (ctx + new) * sizeof(float));
 *     return true;
 *
 *     // infer_out：前向已经跑完，读输出 → 判定 → 再消费窗口
 *     ...
 *     aura_nn_window_consume(&s->win);   // ← 忘了这句，下一帧的窗就拼错了
 *
 * 为什么是"追加 + 消费"两段式（而不是 push 里直接旋转缓冲）：
 * 帧长与窗长一般不成整数倍（16k/10ms 的 160 样本 vs 512 的窗），跨窗那一帧必须
 * 拆成两段；而后半段若在 push 里就写回缓冲头部，会盖掉**还没被前向读走**的当前窗。
 * 于是"攒够就返回"的写法只剩两条路：丢掉多出来的样本（每 3.2 帧丢 128 个，
 * 表现为 VAD 判定漂移、ASR 识别率莫名下降，且不报任何错），或者丢在别处。
 * 这里改为：窗取缓冲**头部**的 ctx+new 个（FIFO，最旧的先入窗），多出来的样本
 * 原样留在缓冲尾部，consume 时接到下一次的窗首 —— 于是任意帧长都无损。
 *
 * 定长内联数组，运行期不 malloc（todo.md 4.4）。不够就调这两个宏，别改成动态分配。 */
#ifndef AURA_NN_WINDOW_BUF_MAX
#define AURA_NN_WINDOW_BUF_MAX 4096 /* 缓冲上限（样本数）= 256ms @16k */
#endif
#ifndef AURA_NN_WINDOW_FRAME_MAX
#define AURA_NN_WINDOW_FRAME_MAX 480 /* 单次 push 的样本上限 = 10ms @48k */
#endif

typedef struct aura_nn_window {
    uint32_t ctx_samples; /* 上文样本数（每次前向复用上一窗尾部） */
    uint32_t new_samples; /* 每次前向需要的新样本数 */
    uint32_t len;         /* buf 内有效样本数（自 buf[0] 起连续） */
    float    buf[AURA_NN_WINDOW_BUF_MAX];
} aura_nn_window_t;

/* 配置窗口。要求 ctx + new + AURA_NN_WINDOW_FRAME_MAX <= AURA_NN_WINDOW_BUF_MAX，
 * 否则返回 AURA_ERR_INVALID_ARG（留一帧的余量，保证 push 永不越界）。 */
aura_err_t aura_nn_window_init(aura_nn_window_t *w, uint32_t ctx_samples, uint32_t new_samples);

/* 追加一帧 PCM（**int16 单通道**），内部转 float 并归一化到 [-1,1]。
 * 返回非 NULL = 已攒够一窗，指针指向窗首（[ctx][new] 连续，可直接写输入张量）；
 * 返回 NULL = 还没攒够。**单次 push 最多产出一次非 NULL**（见上方用法说明）。 */
const float *aura_nn_window_push(aura_nn_window_t *w, const int16_t *pcm, uint32_t samples);

/* 前向读完窗口后调用：把本窗末尾 ctx_samples 个样本挪到缓冲头部，作为下一次的上文。 */
void aura_nn_window_consume(aura_nn_window_t *w);

/* 清空（FLUSH：丢弃在途音频，但保留窗口配置）。 */
void aura_nn_window_reset(aura_nn_window_t *w);

/* ============================== 节点上下文 ============================== */

typedef struct aura_nn_node aura_nn_node_t;

/* 节点运行时句柄。回调收到的就是它；算法私有状态用 aura_nn_state() 取。 */
struct aura_nn_node {
    aura_node_t               base;
    const struct aura_nn_desc *desc;
    aura_algo_params_t        params; /* 装配期快照（栈上的 params 早已失效） */
    aura_infer_model_t       *model;  /* 可为 NULL（无模型算法） */
    aura_infer_group_t       *group;  /* 私有推理组；NULL = 用默认组 */
    char                      namebuf[AURA_CHAIN_NAME_MAX];

    uint32_t rate;     /* 生效采样率 */
    uint32_t channels;
    uint32_t frame_len;

    /* NN_ASYNC 运行时（SYNC 节点不用） */
    aura_task_t  *worker;   /* 子任务线程 */
    aura_queue_t *job_q;    /* 任务队列（item = job_size 字节的 job） */
    aura_queue_t *result_q; /* 结果队列（item = {len, data}，见 aura_nn_post_result） */

    int  last_flag; /* 标志输出的边沿检测（见 desc.flag_*_type） */
    bool flag_valid;

    /* 自有统计（与 aura_node_stats_t 分开：那个的 processed/errors 由 pipeline 记，
     * 这里记的是"算法视角"的量 —— 攒了几窗、单次前向多慢、判定了多少次）。 */
    uint64_t frames_in;
    uint64_t infer_calls;
    uint64_t infer_us_total;
    uint64_t infer_us_max;
    uint64_t flag_rise;
    uint64_t flag_fall;
    uint64_t dropped;   /* 输入攒不下 / 前向失败导致的丢帧 */
    bool     slow_warned;
    bool     err_logged; /* 前向失败只告警一次 */

    /* 算法私有状态紧随其后（desc.state_size 字节，创建时清零）。
     * 用 aura_nn_state() 取，别直接算偏移。 */
    unsigned char state[];
};

/* 算法私有状态指针（已清零，生命周期同节点）。 */
void *aura_nn_state(const aura_nn_node_t *self);

/* ============================== 描述表 ============================== */

typedef struct aura_nn_desc {
    /* ---- 由 aura_nn_finalize() 依下列字段填好，注册时直接用 ---- */
    aura_algo_desc_t algo;

    const char       *name;    /* 注册名（chain = <name> 用它） */
    aura_algo_kind_t  kind;    /* NN_SYNC（实现）；NN_ASYNC（接口预留，finalize 拒绝） */
    aura_nn_io_kind_t io_kind;
    aura_infer_form_t form;    /* SESSION / MODULE / LLM */

    /* 模型路径：params.model_path 优先（配置可覆盖），否则 model_root/<model_file>
     * （见 aura_nn_set_model_root）。两者都空 = **无模型算法**（纯逻辑/自检节点），
     * 此时 algo_init 收到 model == NULL。 */
    const char *model_file;

    /* 订阅 event_bus（barge_in / VAD 边沿等，如 ASR 需要 VAD 结束事件）。
     * 置真要同时给 on_event —— 订阅了却没人处理，是描述表写错（finalize 报错）。 */
    bool wants_events;

    /* 用私有推理组（见 inference.h「推理组」）。
     * 置位的场合：单次前向明显超过一帧预算的模型（zipformer KWS 20–50ms > 10ms）。
     * 不置位的话它和别的 SYNC 节点共用默认组，组锁会把 VAD 的短前向挡在它后面，
     * 表现为"VAD 判定偶尔晚几十毫秒"——不报错，只是所有时间指标一起漂。
     * 代价是多一份 Runtime 与线程池；对每 160ms 才跑一次的模型不值一提。 */
    bool private_group;

    /* 私有状态大小（字节），创建时清零并跟在节点结构体后面。 */
    size_t state_size;

    /* ---- 回调（除 algo_init 外都可为 NULL）---- */

    /* 模型加载后调用一次：resize/tensor 绑定、读超参、按模型口径初始化滑窗。
     * model 可能为 NULL（无模型算法）。返回非 OK 则装配失败。 */
    aura_err_t (*algo_init)(aura_nn_node_t *self, aura_infer_model_t *model);

    /* 每帧调用：攒窗 + 写输入张量。返回 true = 攒够一窗，可以前向。
     * **由算法自己写输入张量**（只有它知道张量名与窗口口径），
     * 适配层只负责 run 与随后的 infer_out。 */
    bool (*frame_in)(aura_nn_node_t *self, const aura_audio_frame_t *frame);

    /* 前向结束后调用：读输出张量、判定，并用 aura_nn_report() 报结果。 */
    aura_err_t (*infer_out)(aura_nn_node_t *self);

    /* 控制面：RESET/FLUSH —— 清滑窗、清递推状态（LSTM hc）、清判定状态。
     * 模型与会话状态**不**重载（重载是 stop/start 的事）。
     * NN_ASYNC 的 RESET/FLUSH 走队列在 worker 里跑（见 nn_control）。 */
    aura_err_t (*algo_reset)(aura_nn_node_t *self);

    /* 其余命令（SET_CONFIG 等，语义由算法自定）：模板只转发，不理解内容。
     * 在调用线程内联执行 —— 实现里只做"就地换算 + 投 job"这类短动作。 */
    aura_err_t (*algo_control)(aura_nn_node_t *self, aura_node_cmd_t cmd,
                               const void *arg);

    /* 事件回调（仅 wants_events = true 时有意义）：barge-in、别的节点发的 VAD
     * 边沿、控制类事件。典型用途是"播放期间抬高判定阈值"这类策略。
     * **在 event_bus 分发任务里跑** —— 禁止长阻塞，只做标记/入队（node.h）。 */
    void (*on_event)(aura_nn_node_t *self, const aura_event_t *event);

    /* 停止时调用（模型卸载之前）：释放算法自有的额外资源。 */
    void (*algo_deinit)(aura_nn_node_t *self);

    /* ---- NN_ASYNC 专属（SYNC 节点忽略）---- */

    /* 产出文本 chunk（ASR）。仅 NN_ASYNC 允许（SYNC 的文本面没有旁路观察场景）。 */
    bool produces_text;

    /* 任务队列深度（默认 8）。队满丢新任务并计数（dropped）——
     * 任务带的是快照数据，丢一个只会丢一段识别结果，不会错位。 */
    uint32_t job_queue_depth;

    /* job 结构体大小。job 必须放在算法状态（state）的**开头**，frame_in 返回
     * true 时模板把前 job_size 字节拷进任务队列（按值穿队列，无竞态）。
     * job 的前 4 字节必须是 aura_nn_job_head_t（magic 用于识别哨兵）。 */
    size_t job_size;

    /* worker 线程里跑：读 job、做推理。不得调用任何 emit_*。 */
    aura_err_t (*async_run)(aura_nn_node_t *self, const void *job);

    /* 结果消费：在 bus 分发任务上下文里跑（可以 emit_text/emit 事件）。 */
    aura_err_t (*result_consume)(aura_nn_node_t *self, const void *result, uint32_t len);

    /* ---- 标志输出的边沿事件（VAD 类）----
     * aura_nn_report() 报告的 0→1 发 rise_type、1→0 发 fall_type；
     * AURA_EVENT_NONE 表示不发。只发边沿：每帧发事件会刷爆总线。 */
    int flag_rise_type;
    int flag_fall_type;

    /* ---- 参数规格：声明本算法认哪些 extra 键（同 DSP 侧，装配期校验） ---- */
    const aura_algo_param_spec_t *param_specs;
    uint32_t                      param_spec_count;
} aura_nn_desc_t;

/* job 头：job 结构体的前 8 字节必须是它（模板识别哨兵用）。
 * magic == AURA_NN_JOB_MAGIC = 真任务（其余字节算法自定）；
 * magic == 0 = 复位哨兵（worker 里跑 algo_reset）；magic == AURA_NN_JOB_EXIT
 * = 退出哨兵（模板内部用，算法不要碰）。 */
typedef struct aura_nn_job_head {
    uint32_t magic;
    uint32_t kind; /* 算法自定义任务类别 */
} aura_nn_job_head_t;

#define AURA_NN_JOB_MAGIC 0x4E4E4A42u /* "NNJB" */
#define AURA_NN_JOB_EXIT  0x58495421u /* "!TIX" */

/* worker 线程里投递结果：进私有结果队列 + 发 AURA_EVENT_NN_RESULT
 * （payload = 节点名，模板在 bus 分发任务里认领后调 result_consume）。
 * len 上限 AURA_NN_RESULT_MAX_DATA。只可在 worker 上下文调用。 */
void aura_nn_post_result(aura_nn_node_t *self, const void *result, uint32_t len);

/* 依 name/io_kind 填好内嵌的 algo 描述（io 能力、create）。
 * 描述表在 static 里无法调用函数，故每条注册前必须调一次。
 * ASYNC_SOURCE（TTS 音频源）仍未实现，在此返回 AURA_ERR_UNSUPPORTED。 */
aura_err_t aura_nn_finalize(aura_nn_desc_t *desc);

/* 批量 finalize + 注册。 */
aura_err_t aura_nn_register_all(aura_nn_desc_t *descs, uint32_t count);

/* 模型根目录（配置里的 models 路径）。装配层在 chain_build 前调用一次；
 * 只存指针，调用方保证生命周期（通常是配置里的静态字符串或 main 的 argv 缓冲）。 */
aura_err_t aura_nn_set_model_root(const char *dir);
const char *aura_nn_model_root(void);

/* ============================== 回调内使用 ============================== */

/* 报告本窗判定结果（0/1）。适配层做边沿检测并发事件（见 desc.flag_*_type），
 * 事件带当前输入帧的 pts（下游按时间切片靠它，见 aura_nn_emit）。 */
void aura_nn_report(aura_nn_node_t *self, bool hit);

/* 发一个带值的事件（KWS 命中、声纹结果、ASR 分段…）。**不做边沿检测** ——
 * 一次性结果每次都发。只填 type/code/u/payload，seq/ts_us 由总线补。
 * ev->u.audio.pts_us 写 AURA_PTS_AUTO 表示"代填为当前输入帧的 pts"，
 * 写真实值则原样使用（**只有 SYNC 路径能代填**：异步结果回调里没有"当前帧"）。 */
void aura_nn_emit(aura_nn_node_t *self, int type, const aura_event_t *ev);

/* 当前标志状态（不回看历史）。 */
bool aura_nn_flag(const aura_nn_node_t *self);

/* 取推理引擎句柄（frame_in/infer_out 写读张量用）。 */
aura_infer_model_t *aura_nn_model(aura_nn_node_t *self);

/* 引擎级初始化（aura_infer_init + 模型根目录）。装配层调一次即可；
 * 重复调用返回 AURA_ERR_EXIST（透传推理层语义）。 */
aura_err_t aura_nn_init(const char *model_root, const aura_infer_cfg_t *cfg);

/* ============================== 统计 ============================== */

typedef struct aura_nn_stats {
    uint64_t frames_in;    /* 投入算法的音频帧数 */
    uint64_t windows;      /* 触发前向的窗口数 */
    uint64_t infer_us_max; /* 单次前向最大耗时（判断"是否超一帧预算"的依据） */
    uint64_t infer_us_avg;
    uint64_t flag_rise;
    uint64_t flag_fall;
    uint64_t errors;
    uint64_t dropped;
} aura_nn_stats_t;

aura_err_t aura_nn_get_stats(const aura_node_t *node, aura_nn_stats_t *out);

#ifdef __cplusplus
}
#endif

#endif /* AURA_ALGORITHM_AURA_NN_ADAPTER_H */
