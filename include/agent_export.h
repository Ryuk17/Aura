/* Aura — 对外 API（纯 C）
 *
 * 这是框架**唯一**允许上层业务接触的接口面（todo.md 4.6）：
 * 只暴露 初始化 / 启停 / 事件回调 / 配置，上层不得直接操作 pipeline、node、
 * event_bus 等内部对象。约束这样定的理由：
 *   - 上层换成 C/C++/Rust/Android JNI 都能直接用，不需要 ABI 兼容的 C++ 编译器；
 *   - 内部拓扑（节点增删、连线、内存池布局）可以自由演进而不破坏调用方。
 *
 * 线程约定：
 *   - aura_agent_init/start/stop/deinit 由**同一个外部线程**串行调用；
 *   - aura_agent_feed_audio 由采集线程调用（可与之并发）；
 *   - 事件回调在框架内部线程执行 —— **禁止长阻塞**，需要耗时处理请自行投递到业务线程。
 *
 * 错误处理：所有接口返回 aura_err_t（0 成功，负数为错误，见 utils/error.h）。
 */
#ifndef AURA_AGENT_EXPORT_H
#define AURA_AGENT_EXPORT_H

#include <stdbool.h>
#include <stdint.h>

#include "utils/error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- 版本信息 */

/* 语义化版本字符串，如 "0.1.0"。 */
const char *aura_agent_version(void);

/* 编译期配置摘要（OS、平台、已编译模块），用于现场排查"这份库到底编了什么"。 */
const char *aura_agent_build_info(void);

/* ------------------------------------------------------------ 状态（只读） */

/* 对外可见的对话状态。与内部状态机一一对应（内部状态机在 core/state_machine）。
 * 数值刻意保持一致，agent_core.c 内有编译期断言校验。 */
typedef enum {
    AURA_AGENT_STATE_IDLE = 0,      /* 待机：KWS/VAD 常跑，等待唤醒 */
    AURA_AGENT_STATE_LISTENING = 1, /* 已唤醒：等待用户说话 / ASR 收音中 */
    AURA_AGENT_STATE_THINKING = 2,  /* 已识别：等待 LLM 输出 */
    AURA_AGENT_STATE_SPEAKING = 3,  /* 播放中：仍可被打断 */
    AURA_AGENT_STATE_ERROR = 4,     /* 异常：可配置自动复位回 Idle */
    AURA_AGENT_STATE_COUNT
} aura_agent_state_t;

typedef enum {
    AURA_AGENT_EVENT_NONE = 0,
    AURA_AGENT_EVENT_STATE_CHANGED, /* from/to 见 payload/…（见 aura_agent_event_t） */
    AURA_AGENT_EVENT_KWS_HIT,       /* 唤醒词命中 */
    AURA_AGENT_EVENT_VAD_START,     /* 检测到人声起点 */
    AURA_AGENT_EVENT_VAD_END,       /* 人声结束 */
    AURA_AGENT_EVENT_VOICEPRINT,    /* 声纹校验结果 */
    AURA_AGENT_EVENT_ASR_PARTIAL,   /* 识别中间结果 */
    AURA_AGENT_EVENT_ASR_FINAL,     /* 识别最终结果 */
    AURA_AGENT_EVENT_LLM_FIRST,     /* 首 token 到达（首响指标关键点） */
    AURA_AGENT_EVENT_LLM_TOKEN,     /* token 流 */
    AURA_AGENT_EVENT_LLM_DONE,      /* 生成结束 */
    AURA_AGENT_EVENT_TTS_FIRST,     /* 首个音频包 */
    AURA_AGENT_EVENT_TTS_DONE,      /* 播报结束 */
    AURA_AGENT_EVENT_BARGE_IN,      /* 有效打断 */
    AURA_AGENT_EVENT_ERROR,         /* 内部异常（错误码见 code 字段） */
    AURA_AGENT_EVENT_USER,          /* 业务自定义事件 */
} aura_agent_event_t;

#define AURA_AGENT_TEXT_MAX 256

typedef struct aura_agent_event_data {
    aura_agent_event_t type;
    int32_t            code;   /* 错误码 / 结果码 / 状态码 */
    int32_t            value;  /* 置信度 / 分数等（语义随 type 定） */
    uint64_t           ts_ms;  /* 事件发生时刻（单调毫秒） */
    /* 状态的 from/to（仅 STATE_CHANGED 有效） */
    aura_agent_state_t from_state;
    aura_agent_state_t to_state;
    /* 文本负载（仅 ASR/LLM 类事件有效），UTF-8，始终以 '\0' 结尾 */
    char text[AURA_AGENT_TEXT_MAX];
} aura_agent_event_data_t;

/* 事件回调。在框架内部线程执行，禁止长阻塞。 */
typedef void (*aura_agent_event_cb)(const aura_agent_event_data_t *event, void *user);

/* ------------------------------------------------------------------ 配置 */

typedef struct aura_agent_config {
    /* 链路参数 */
    uint32_t sample_rate;  /* 主采样率，默认 16000 */
    uint32_t frame_ms;     /* 帧长（毫秒），默认 10 */
    uint32_t channels;     /* 采集通道数（多麦 + 参考通道），默认 1 */

    /* 队列与内存池（调优用；为 0 时取默认值） */
    uint32_t audio_queue_depth;
    uint32_t text_queue_depth;
    uint32_t frame_pool_blocks;

    /* 日志等级：0=error 1=warn 2=info 3=debug 4=trace（默认 2） */
    int32_t  log_level;
    /* 是否开启 profiler 打点（默认 true） */
    bool     profiler_enable;
    /* Error 后是否自动复位回 Idle（默认 true）与延迟 */
    bool     auto_reset;
    uint32_t auto_reset_ms;

    /* 路径（UTF-8；为 NULL 表示用默认值） */
    const char *board;      /* 板级标识，用于选择 configs/board_xxx/ */
    const char *config_dir; /* 配置目录 */
    const char *model_dir;  /* 模型根目录 */
} aura_agent_config_t;

/* 用默认值填充配置结构（调用方先 memset 再调用，或直接用本函数）。 */
void aura_agent_config_default(aura_agent_config_t *cfg);

/* 从 .conf 文件加载配置（key = value 文本，见 core/config/config.h）。
 * 未出现在文件中的字段保持原值 —— 典型用法是先 default 再 load。 */
aura_err_t aura_agent_config_load(aura_agent_config_t *cfg, const char *path);

/* ---------------------------------------------------------------- 生命周期 */

/* 初始化：装载配置、建 pipeline / event_bus / 状态机、按配置初始化推理引擎。
 * 此时不启动音频链路（不占用实时资源）。重复调用返回 AURA_ERR_EXIST。 */
aura_err_t aura_agent_init(const aura_agent_config_t *cfg);

/* 启动链路：各节点任务开始运行、状态机进 Idle、开始等待唤醒。
 * 必须已 init。重复调用返回 AURA_ERR_STATE。 */
aura_err_t aura_agent_start(void);

/* 停止链路：停采集、清队列、节点任务退出。可重复调用（幂等）。 */
aura_err_t aura_agent_stop(void);

/* 反初始化：释放全部资源；之后可再次 init。可重复调用（幂等）。 */
aura_err_t aura_agent_deinit(void);

/* ------------------------------------------------------------ 运行期控制 */

/* 注册/替换事件回调（user 原样回传）。cb 传 NULL 表示注销。 */
aura_err_t aura_agent_set_event_callback(aura_agent_event_cb cb, void *user);

/* 查询当前状态。 */
aura_err_t aura_agent_get_state(aura_agent_state_t *out);

/* 喂入音频（采集线程 / 文件回放 / 测试）。
 *   pcm         ：交错 PCM，fmt 见 aura_agent_audio_fmt_t
 *   frame_count ：**每通道**样本数
 *   channels    ：本帧通道数，需与配置一致
 *   pts_us      ：帧首样本时戳；传 0 表示由框架按帧长自动推进
 *   timeout_ms  ：入队等待上限（AURA_AGENT_WAIT_FOREVER 表示阻塞到入队成功）
 * 文件喂入场景用阻塞等待可自然形成背压，麦克风场景应传较小值并容忍丢帧。 */
typedef enum {
    AURA_AGENT_AUDIO_S16 = 0, /* int16_t 交错 */
    AURA_AGENT_AUDIO_F32 = 1, /* float 交错 */
} aura_agent_audio_fmt_t;

#define AURA_AGENT_WAIT_FOREVER 0xFFFFFFFFu

aura_err_t aura_agent_feed_audio(const void *pcm, uint32_t frame_count, uint32_t channels,
                                 aura_agent_audio_fmt_t fmt, uint64_t pts_us,
                                 uint32_t timeout_ms);

/* 主动打断当前播报（等价于业务侧判定"有效打断"）：停播 + 中止推理 + 回 Listening。 */
aura_err_t aura_agent_interrupt(void);

/* 打印一轮交互的延迟报告（对照 docs/todo.md 第 6 节验收指标）。 */
aura_err_t aura_agent_report_latency(void);

#ifdef __cplusplus
}
#endif

#endif /* AURA_AGENT_EXPORT_H */
