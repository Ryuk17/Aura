/* Aura — 统一错误码
 *
 * 全框架（含对外 API agent_export.h）共用一套错误码，禁止各模块自定义私有错误码。
 * 约定：0 为成功，负数为错误；错误码可安全跨线程/跨层传递。
 */
#ifndef AURA_UTILS_ERROR_H
#define AURA_UTILS_ERROR_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AURA_OK              = 0,
    AURA_ERR_FAIL        = -1,  /* 未分类失败 */
    AURA_ERR_INVALID_ARG = -2,  /* 入参非法（指针空/越界/取值不合法） */
    AURA_ERR_NOMEM       = -3,  /* 内存不足（mem_pool 耗尽 / malloc 失败） */
    AURA_ERR_TIMEOUT     = -4,  /* 等待超时 */
    AURA_ERR_AGAIN       = -5,  /* 暂时不可用，可重试 */
    AURA_ERR_FULL        = -6,  /* 队列/缓冲已满 */
    AURA_ERR_EMPTY       = -7,  /* 队列/缓冲为空 */
    AURA_ERR_NOT_FOUND   = -8,  /* 节点/订阅者/模型不存在 */
    AURA_ERR_UNSUPPORTED = -9,  /* 当前平台或配置不支持 */
    AURA_ERR_STATE       = -10, /* 状态机当前状态不允许该操作 */
    AURA_ERR_IO          = -11, /* 文件/设备读写失败 */
    AURA_ERR_MODEL       = -12, /* 模型加载/推理失败 */
    AURA_ERR_ABORTED     = -13, /* 被外部中止（barge-in / stop） */
    AURA_ERR_BUSY        = -14, /* 资源被占用 */
    AURA_ERR_EXIST       = -15, /* 对象已存在 */
    AURA_ERR_DSP         = -16, /* DSP/音频引擎内部失败（算法未初始化、处理失败等） */
} aura_err_t;

/* 返回错误码的静态可读字符串（永不返回 NULL）。 */
const char *aura_strerror(aura_err_t err);

/* 便捷宏：出错即返回。用于 init/start 等一次性路径。 */
#define AURA_RETURN_IF_ERR(expr)          \
    do {                                  \
        aura_err_t _e = (expr);           \
        if (_e != AURA_OK) return _e;     \
    } while (0)

#ifdef __cplusplus
}
#endif

#endif /* AURA_UTILS_ERROR_H */
