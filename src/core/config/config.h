/* Aura — 配置加载
 *
 * 目标（todo.md）：**代码与配置分离** —— 板子参数（采样率、麦数、队列深度、
 * 模型路径）写在 configs/ 下，改配置不改代码。
 *
 * 格式：极简 `key = value` 文本（`#` 起注释），刻意不引入 yaml/json 依赖 ——
 * 解析器要小到能整段读完、能单独测。configs/board_xxx/ 下的 .conf 即此格式。
 */
#ifndef AURA_CORE_CONFIG_CONFIG_H
#define AURA_CORE_CONFIG_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#include "osal/osal.h"
#include "utils/error.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 全框架运行时配置。字段按"链路 → 队列 → 状态机 → 路径"分组。 */
typedef struct aura_config {
    /* 链路 */
    uint32_t sample_rate;  /* 主采样率，16 kHz */
    uint32_t frame_ms;     /* 帧长（毫秒），默认 10 */
    uint32_t channels;     /* 采集通道数（多麦 + AEC 参考） */

    /* 队列 / 池 */
    uint32_t audio_queue_depth;
    uint32_t text_queue_depth;
    uint32_t event_queue_depth;
    uint32_t frame_pool_blocks;

    /* 日志 */
    aura_log_level_t log_level;

    /* 状态机 */
    bool     sm_auto_reset;
    uint32_t sm_auto_reset_ms;

    /* profiler */
    bool     profiler_enable;

    /* 路径 / 板级标识（字符串由 caller 持有，配置加载只存指针） */
    char     board[32];
    char     config_dir[256];
    char     model_dir[256];

    /* Kconfig 式的模块裁剪开关（与编译期 AURA_BUILD_* 选项对应，
     * 运行期对未编译进来的模块给出明确错误而不是静默失效）。 */
    bool     enable_kws;
    bool     enable_voiceprint;
    bool     enable_asr;
    bool     enable_llm;
    bool     enable_tts;
    bool     enable_barge_in;
    bool     enable_cloud;
} aura_config_t;

void aura_config_default(aura_config_t *cfg);

/* 解析配置文件并覆盖到 cfg（未出现的 key 保持原值）。
 * 文件不存在返回 AURA_ERR_IO；语法错误返回 AURA_ERR_INVALID_ARG 并报出错误行号。 */
aura_err_t aura_config_load_file(aura_config_t *cfg, const char *path);

/* 打印当前配置（启动日志用，方便现场核对用了哪份配置）。 */
void aura_config_dump(const aura_config_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* AURA_CORE_CONFIG_CONFIG_H */
