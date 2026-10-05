/* Aura — Agent 组装层（Phase 1 骨架）
 *
 * 本文件是 include/agent_export.h 的实现。它只做"把框架部件组装成闭环"：
 *   init  : 装载配置 → 初始化推理引擎 → 建 pipeline / event_bus / 状态机
 *   start : 启动 pipeline → 状态机进 Idle → 开始等待唤醒
 *   feed  : 音频帧喂入 pipeline（采集线程）
 *   事件  : event_bus 事件 → 上层回调（aura_agent_event_data_t）
 *
 * 组装约束（todo.md 4.6）：agent 层不做信号处理、不跑推理、不直接调用
 * kws/asr/llm 函数 —— 只消费事件，通过 pipeline 消息下发启停。
 *
 * 算法节点有两条来源（Phase 2 起）：
 *   配置 `chain = aec3, ns, tee(silero_vad), ...` —— init 时按名字装配（本层不
 *     认识任何具体算法，算法族由上层在 init 前注册，见 core/algorithm.h）；
 *   aura_agent_add_node()（agent_core.h，内部接口）—— 手工注入，测试与定制用。
 */
#include "agent/agent_core.h"
#include "agent_export.h"

#include <stdio.h>
#include <string.h>

#include "aura_config.h" /* CMake 生成的编译期配置 */
#if AURA_WITH_MNN
#include "algorithm/aura_nn_adapter.h" /* 只为把 model_dir 交给 NN 节点（见 init） */
#endif
#if AURA_WITH_TRICKROOM
#include "dsp/aura_dsp_adapter.h" /* 注册 TrickRoom 描述表（aec3/bf/ns/agc/src/vad） */
#endif
#include "core/algorithm.h"
#include "core/config/config.h"
#include "utils/logger/logger.h"
#include "utils/profiler/profiler.h"

#define TAG "agent"

/* ------------------------------------------------------------ 单例状态 */

static struct {
    bool     inited;
    bool     started;
    aura_agent_config_t  cfg;
    aura_pipeline_t     *pipeline;
    aura_event_bus_t    *bus;
    aura_sm_t           *sm;
    aura_model_mgr_t    *models;
    aura_agent_event_cb  cb;
    void                *cb_user;
    aura_mutex_t        *lock;
    uint32_t             node_count;
    uint32_t             last_event_seq;
    /* 由配置链建出的节点（aura_chain_build 的产物）。deinit 时由本层回收 ——
     * pipeline 只管输入队列，节点结构体归调用方（见 aura_pipeline_remove）。 */
    aura_node_t         *chain_nodes[AURA_CHAIN_MAX_LINKS];
    uint32_t             chain_node_count;
} g;

/* 状态枚举值必须与内部状态机一致（agent_export.h 注释里的约定）。 */
_Static_assert(AURA_AGENT_STATE_COUNT == AURA_STATE_COUNT,
               "aura_agent_state_t must stay in sync with aura_state_t");

/* ------------------------------------------------------------ 内部工具 */

static void on_sm_change(aura_sm_t *sm, aura_state_t from, aura_state_t to, aura_trigger_t trig,
                         void *user)
{
    (void)sm;
    (void)from;
    (void)trig;
    (void)user;
    /* 状态变化 → 上层事件回调 */
    aura_agent_event_data_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type       = AURA_AGENT_EVENT_STATE_CHANGED;
    ev.from_state = (aura_agent_state_t)from;
    ev.to_state   = (aura_agent_state_t)to;
    ev.ts_ms      = aura_osal_time_ms();
    aura_agent_event_cb cb;
    void                *cb_user;
    aura_osal_mutex_lock(g.lock);
    cb      = g.cb;
    cb_user = g.cb_user;
    aura_osal_mutex_unlock(g.lock);
    if (cb != NULL) {
        cb(&ev, cb_user);
    }
}

/* event_bus 事件 → 上层事件数据（只挑上层关心的类型转发）。 */
static void on_bus_event(const aura_event_t *event, void *user)
{
    (void)user;

    /* 先驱动状态机：SILENCE_TIMEOUT 等事件不转发给上层，但必须被状态机消费。
     * （sm_handle 短小，在分发线程执行是安全的。） */
    if (g.sm != NULL) {
        aura_trigger_t trig = aura_sm_trigger_from_event(event);
        if (trig != AURA_TRIG_NONE) {
            (void)aura_sm_handle(g.sm, trig);
        }
    }

    /* 再按需转发给上层回调 */
    aura_agent_event_data_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.ts_ms = event->ts_us / 1000;
    ev.code  = event->code;
    ev.value = event->code;

    switch (event->type) {
    case AURA_EVENT_KWS_HIT:
        ev.type  = AURA_AGENT_EVENT_KWS_HIT;
        ev.value = event->u.kws.score;
        break;
    case AURA_EVENT_VAD_SPEECH_START:
        ev.type = AURA_AGENT_EVENT_VAD_START;
        break;
    case AURA_EVENT_VAD_SPEECH_END:
        ev.type = AURA_AGENT_EVENT_VAD_END;
        break;
    case AURA_EVENT_VOICEPRINT_RESULT:
        ev.type  = AURA_AGENT_EVENT_VOICEPRINT;
        ev.value = event->u.voiceprint.score;
        ev.code  = event->u.voiceprint.passed ? 0 : -1;
        break;
    case AURA_EVENT_ASR_PARTIAL:
        ev.type = AURA_AGENT_EVENT_ASR_PARTIAL;
        snprintf(ev.text, sizeof(ev.text), "%s", event->payload);
        break;
    case AURA_EVENT_ASR_FINAL:
        ev.type = AURA_AGENT_EVENT_ASR_FINAL;
        snprintf(ev.text, sizeof(ev.text), "%s", event->payload);
        break;
    case AURA_EVENT_LLM_FIRST_TOKEN:
        ev.type = AURA_AGENT_EVENT_LLM_FIRST;
        break;
    case AURA_EVENT_LLM_TOKEN:
        ev.type = AURA_AGENT_EVENT_LLM_TOKEN;
        snprintf(ev.text, sizeof(ev.text), "%s", event->payload);
        break;
    case AURA_EVENT_LLM_DONE:
        ev.type = AURA_AGENT_EVENT_LLM_DONE;
        break;
    case AURA_EVENT_TTS_FIRST_CHUNK:
        ev.type = AURA_AGENT_EVENT_TTS_FIRST;
        break;
    case AURA_EVENT_TTS_DONE:
        ev.type = AURA_AGENT_EVENT_TTS_DONE;
        break;
    case AURA_EVENT_BARGE_IN:
        ev.type = AURA_AGENT_EVENT_BARGE_IN;
        break;
    case AURA_EVENT_ERROR:
        ev.type = AURA_AGENT_EVENT_ERROR;
        snprintf(ev.text, sizeof(ev.text), "%s", event->payload);
        break;
    default:
        return; /* 不转发 */
    }

    aura_agent_event_cb cb;
    void                *cb_user;
    aura_osal_mutex_lock(g.lock);
    cb      = g.cb;
    cb_user = g.cb_user;
    g.last_event_seq = event->seq;
    aura_osal_mutex_unlock(g.lock);
    if (cb != NULL) {
        cb(&ev, cb_user);
    }
}

static uint32_t all_events_mask(void)
{
    uint32_t mask = 0;
    for (int i = 1; i < AURA_EVENT_TYPE_COUNT; i++) {
        mask |= (1u << i);
    }
    return mask;
}

/* ------------------------------------------------------------ 对外 API */

const char *aura_agent_version(void)
{
    return AURA_VERSION_STRING;
}

const char *aura_agent_build_info(void)
{
    static char info[256];
    snprintf(info, sizeof(info),
             "os=%s platform=%s kws=%d voiceprint=%d asr=%d llm=%d tts=%d barge_in=%d net=%d "
             "mnn=%d mnn_llm=%d",
             AURA_OS_NAME, AURA_PLATFORM_NAME, AURA_BUILD_KWS, AURA_BUILD_VOICEPRINT,
             AURA_BUILD_ASR, AURA_BUILD_LLM, AURA_BUILD_TTS, AURA_BUILD_BARGE_IN, AURA_BUILD_NET,
             AURA_WITH_MNN, AURA_WITH_MNN_LLM);
    return info;
}

void aura_agent_config_default(aura_agent_config_t *cfg)
{
    if (cfg == NULL) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->sample_rate       = 16000;
    cfg->frame_ms          = 10;
    cfg->channels          = 1;
    cfg->audio_queue_depth = 8;
    cfg->text_queue_depth  = 16;
    cfg->frame_pool_blocks = 16;
    cfg->log_level         = AURA_LOG_LVL_INFO;
    cfg->profiler_enable   = true;
    cfg->auto_reset        = true;
    cfg->auto_reset_ms     = 1000;
    cfg->board             = "default";
    cfg->model_dir         = "models";
}

aura_err_t aura_agent_config_load(aura_agent_config_t *cfg, const char *path)
{
    if (cfg == NULL || path == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    /* 桥接：内部 core/config 的格式与对外配置共用同一份 .conf。 */
    aura_config_t core;
    aura_config_default(&core);
    aura_err_t rc = aura_config_load_file(&core, path);
    if (rc != AURA_OK) {
        return rc;
    }
    /* 字符串字段需要稳定存储：agent 是单实例，用静态缓冲承接。 */
    static char s_board[32];
    static char s_config_dir[256];
    static char s_model_dir[256];
    static char s_chain[256];
    static char s_chain_keys[AURA_CONFIG_MAX_CHAIN_PARAMS][48];
    static char s_chain_vals[AURA_CONFIG_MAX_CHAIN_PARAMS][64];
    static aura_agent_chain_param_t s_chain_params[AURA_CONFIG_MAX_CHAIN_PARAMS];

    aura_agent_config_default(cfg);
    cfg->sample_rate       = core.sample_rate;
    cfg->frame_ms          = core.frame_ms;
    cfg->channels          = core.channels;
    cfg->audio_queue_depth = core.audio_queue_depth;
    cfg->text_queue_depth  = core.text_queue_depth;
    cfg->frame_pool_blocks = core.frame_pool_blocks;
    cfg->log_level         = core.log_level;
    cfg->profiler_enable   = core.profiler_enable;
    cfg->auto_reset        = core.sm_auto_reset;
    cfg->auto_reset_ms     = core.sm_auto_reset_ms;
    snprintf(s_board, sizeof(s_board), "%s", core.board);
    snprintf(s_config_dir, sizeof(s_config_dir), "%s", core.config_dir);
    snprintf(s_model_dir, sizeof(s_model_dir), "%s", core.model_dir);
    cfg->board      = s_board;
    cfg->config_dir = s_config_dir;
    cfg->model_dir  = s_model_dir;

    /* 算法链：字符串同样落到静态缓冲（init 时才解析，早于它的字面量早就没了）。 */
    snprintf(s_chain, sizeof(s_chain), "%s", core.chain);
    cfg->chain = (s_chain[0] != '\0') ? s_chain : NULL;
    for (uint32_t i = 0; i < core.chain_param_count && i < AURA_CONFIG_MAX_CHAIN_PARAMS; i++) {
        snprintf(s_chain_keys[i], sizeof(s_chain_keys[i]), "%s", core.chain_params[i].key);
        snprintf(s_chain_vals[i], sizeof(s_chain_vals[i]), "%s", core.chain_params[i].val);
        s_chain_params[i].key = s_chain_keys[i];
        s_chain_params[i].val = s_chain_vals[i];
    }
    cfg->chain_param_count = core.chain_param_count;
    cfg->chain_params      = (cfg->chain_param_count > 0) ? s_chain_params : NULL;
    return AURA_OK;
}

/* -------------------------------------------------- 配置链自动装配 */

/* 对外配置 → core 配置（只搬链相关字段）后交给 aura_chain_from_config。
 * s_chain_cfg 必须是静态的：STR 型参数不做拷贝，会直接指向这里的缓冲
 * （见 core/algorithm.h 里字符串生命周期的约定）。agent 是单实例，够用。 */
static aura_config_t s_chain_cfg;
static aura_chain_t  s_chain; /* 同上：参数里的名字/字符串指向 s_chain_cfg */

/* 解析配置里的链 —— 只看注册表，不碰 pipeline，因此在 init 的**最开始**调用：
 * .conf 写错是最常见的失败，让它发生在分配任何资源之前，重试就不会漏掉
 * 一套 pipeline / bus / 锁。 */
static aura_err_t parse_chain_config(void)
{
    aura_config_default(&s_chain_cfg);
    snprintf(s_chain_cfg.chain, sizeof(s_chain_cfg.chain), "%s", g.cfg.chain);

    for (uint32_t i = 0; i < g.cfg.chain_param_count; i++) {
        const aura_agent_chain_param_t *src = &g.cfg.chain_params[i];
        if (src->key == NULL || src->val == NULL) {
            return AURA_ERR_INVALID_ARG;
        }
        if (s_chain_cfg.chain_param_count >= AURA_CONFIG_MAX_CHAIN_PARAMS) {
            AURA_LOGE(TAG, "too many chain params (max %d)", AURA_CONFIG_MAX_CHAIN_PARAMS);
            return AURA_ERR_FULL;
        }
        aura_config_kv_t *kv = &s_chain_cfg.chain_params[s_chain_cfg.chain_param_count];
        if (strlen(src->key) >= sizeof(kv->key) || strlen(src->val) >= sizeof(kv->val)) {
            AURA_LOGE(TAG, "chain param '%s' too long", src->key);
            return AURA_ERR_INVALID_ARG;
        }
        snprintf(kv->key, sizeof(kv->key), "%s", src->key);
        snprintf(kv->val, sizeof(kv->val), "%s", src->val);
        s_chain_cfg.chain_param_count++;
    }
    return aura_chain_from_config(&s_chain_cfg, &s_chain);
}

/* 把解析好的链建成节点挂进 pipeline（此时 pipeline/event_bus 已就绪）。 */
static aura_err_t build_chain(void)
{
    uint32_t   built = 0;
    aura_err_t rc    = aura_chain_build(g.pipeline, &s_chain, g.chain_nodes, &built);
    if (rc != AURA_OK) {
        /* chain_build 失败时已自行回滚（节点摘除并释放），这里别留半截计数。 */
        g.chain_node_count = 0;
        return rc;
    }
    g.chain_node_count = built;
    g.node_count += built;
    AURA_LOGI(TAG, "chain assembled: %u node(s) from '%s'", built, g.cfg.chain);
    return AURA_OK;
}

/* 释放 init 申请的全部资源（幂等）。init 的失败路径与 deinit 共用 ——
 * 否则一次失败的 init 会把引擎/pipeline/锁留在原地，调用方既无法 deinit
 * （inited 还是 false），第二次 init 又会撞上 "infer already exists"，
 * 整个进程再没有救回来的路。 */
static void teardown(void)
{
    if (g.started && g.pipeline != NULL) {
        (void)aura_pipeline_stop(g.pipeline);
    }
    g.started = false;
    if (g.sm != NULL) {
        aura_sm_destroy(g.sm);
        g.sm = NULL;
    }
    if (g.pipeline != NULL) {
        aura_pipeline_destroy(g.pipeline);
        g.pipeline = NULL;
    }
    /* 节点结构体归本层所有（pipeline 只回收它在 add 时建的输入队列），
     * 因此必须在 pipeline 销毁**之后**释放。 */
    if (g.chain_node_count > 0) {
        aura_chain_destroy_nodes(g.chain_nodes, g.chain_node_count);
        g.chain_node_count = 0;
    }
    if (g.bus != NULL) {
        aura_event_bus_destroy(g.bus);
        g.bus = NULL;
    }
    if (g.models != NULL) {
        aura_model_mgr_destroy(g.models);
        g.models = NULL;
    }
    if (AURA_WITH_MNN) {
        aura_infer_deinit(); /* 未初始化时是空操作 */
    }
    if (g.lock != NULL) {
        aura_osal_mutex_destroy(g.lock);
        g.lock = NULL;
    }
    g.inited = false;
}

aura_err_t aura_agent_init(const aura_agent_config_t *cfg)
{
    if (cfg == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    if (g.inited) {
        return AURA_ERR_EXIST;
    }

    memset(&g, 0, sizeof(g));
    g.cfg = *cfg;
    aura_log_init();
    aura_log_set_level((aura_log_level_t)g.cfg.log_level);
    aura_profiler_init();

#if AURA_WITH_TRICKROOM
    /* 编译期已带 TrickRoom 时，DSP 族在这里自注册（幂等）—— 不用上层记得去注册，
     * 否则"编进了引擎但链上写 aec3 说不认识"是最没道理的一种失败。
     * 必须在 parse_chain_config 之前：链解析要查注册表。 */
    {
        aura_err_t rc = aura_dsp_trickroom_register();
        if (rc != AURA_OK) {
            AURA_LOGE(TAG, "trickroom algo register failed: %s", aura_strerror(rc));
            return rc; /* 此时还没申请任何资源 */
        }
    }
#endif

    /* 链配置先解析（不占资源）：其余算法族（NN）由上层在 init 前注册。 */
    if (g.cfg.chain != NULL && g.cfg.chain[0] != '\0') {
        aura_err_t rc = parse_chain_config();
        if (rc != AURA_OK) {
            AURA_LOGE(TAG, "chain config invalid: %s", aura_strerror(rc));
            return rc; /* 此时还没申请任何资源 */
        }
    } else {
        /* 清掉上一次 init 留下的链描述：否则"先配有链、再改成不带链"会
         * 神不知鬼不觉地把旧链建回来。 */
        aura_chain_init(&s_chain);
        if (g.cfg.chain_param_count > 0) {
            AURA_LOGW(TAG, "%u chain_params configured but no chain — ignored",
                      g.cfg.chain_param_count);
        }
    }

    g.lock = aura_osal_mutex_create();
    if (g.lock == NULL) {
        return AURA_ERR_NOMEM;
    }

    /* 推理引擎（MNN）：全局统一配置。 */
    if (AURA_WITH_MNN) {
        aura_infer_cfg_t infer;
        memset(&infer, 0, sizeof(infer));
        infer.backend       = AURA_INFER_BACKEND_CPU;
        infer.threads       = 2;
        infer.precision_low = true;
        infer.memory_low    = false;
        aura_err_t rc = aura_infer_init(&infer);
        if (rc != AURA_OK) {
            AURA_LOGE(TAG, "infer init failed: %s", aura_strerror(rc));
            teardown();
            return rc;
        }
        /* 模型根目录：链上的 NN 节点按 <root>/<desc.model_file> 找模型，描述表里
         * 只写相对路径（换板子只改配置）。指向 g.cfg.model_dir，生命周期同配置。 */
        (void)aura_nn_set_model_root(g.cfg.model_dir);
        g.models = aura_model_mgr_create();
        if (g.models == NULL) {
            teardown();
            return AURA_ERR_NOMEM;
        }
    }

    /* pipeline + event_bus + 状态机 */
    aura_pipeline_cfg_t pc;
    aura_pipeline_config_default(&pc);
    pc.sample_rate       = g.cfg.sample_rate;
    pc.frame_ms          = g.cfg.frame_ms;
    pc.channels          = g.cfg.channels;
    pc.audio_queue_depth = g.cfg.audio_queue_depth;
    pc.text_queue_depth  = g.cfg.text_queue_depth;
    pc.frame_pool_blocks = g.cfg.frame_pool_blocks;

    g.pipeline = aura_pipeline_create(&pc);
    if (g.pipeline == NULL) {
        teardown();
        return AURA_ERR_NOMEM;
    }

    g.bus = aura_event_bus_create(64);
    if (g.bus == NULL) {
        teardown();
        return AURA_ERR_NOMEM;
    }
    aura_pipeline_attach_bus(g.pipeline, g.bus);

    aura_sm_cfg_t sc;
    memset(&sc, 0, sizeof(sc));
    sc.auto_reset     = g.cfg.auto_reset;
    sc.auto_reset_ms  = g.cfg.auto_reset_ms;
    sc.publish_events = true;
    g.sm = aura_sm_create(&sc, g.bus);
    if (g.sm == NULL) {
        teardown();
        return AURA_ERR_NOMEM;
    }
    aura_sm_set_change_cb(g.sm, on_sm_change, NULL);

    /* 事件转发（内部总线 → 上层回调） */
    aura_event_bus_subscribe(g.bus, all_events_mask(), on_bus_event, NULL);

    /* 建链（配置已在 init 开头解析过）。 */
    if (s_chain.count > 0) {
        aura_err_t rc = build_chain();
        if (rc != AURA_OK) {
            AURA_LOGE(TAG, "chain build failed: %s", aura_strerror(rc));
            teardown();
            return rc;
        }
    }

    g.inited = true;
    AURA_LOGI(TAG, "initialized: %s", aura_agent_build_info());
    AURA_LOGI(TAG, "config: rate=%u frame=%ums ch=%u board=%s model_dir=%s", g.cfg.sample_rate,
              g.cfg.frame_ms, g.cfg.channels, g.cfg.board ? g.cfg.board : "-",
              g.cfg.model_dir ? g.cfg.model_dir : "-");
    return AURA_OK;
}

aura_err_t aura_agent_start(void)
{
    if (!g.inited) {
        return AURA_ERR_STATE;
    }
    if (g.started) {
        return AURA_ERR_STATE;
    }
    if (g.node_count == 0) {
        AURA_LOGW(TAG, "no algorithm nodes registered — feed_audio will fail until Phase 2–4 "
                       "modules are added");
    }
    if (g.cfg.profiler_enable) {
        aura_profiler_begin_session();
    }
    aura_err_t rc = aura_pipeline_start(g.pipeline);
    if (rc != AURA_OK) {
        return rc;
    }
    g.started = true;
    /* 状态机进 Idle（触发 START，同时给上层第一个 STATE_CHANGED）。 */
    (void)aura_sm_handle(g.sm, AURA_TRIG_START);
    return AURA_OK;
}

aura_err_t aura_agent_stop(void)
{
    if (!g.inited || !g.started) {
        return AURA_OK; /* 幂等 */
    }
    g.started = false;
    return aura_pipeline_stop(g.pipeline);
}

aura_err_t aura_agent_deinit(void)
{
    if (!g.inited) {
        return AURA_OK; /* 幂等（含"init 失败过"的情形：那时也没东西可放） */
    }
    teardown();
    return AURA_OK;
}

aura_err_t aura_agent_set_event_callback(aura_agent_event_cb cb, void *user)
{
    if (!g.inited) {
        return AURA_ERR_STATE;
    }
    aura_osal_mutex_lock(g.lock);
    g.cb      = cb;
    g.cb_user = user;
    aura_osal_mutex_unlock(g.lock);
    return AURA_OK;
}

aura_err_t aura_agent_get_state(aura_agent_state_t *out)
{
    if (out == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    if (!g.inited || g.sm == NULL) {
        *out = AURA_AGENT_STATE_IDLE;
        return AURA_OK;
    }
    *out = (aura_agent_state_t)aura_sm_state(g.sm);
    return AURA_OK;
}

aura_err_t aura_agent_feed_audio(const void *pcm, uint32_t frame_count, uint32_t channels,
                                 aura_agent_audio_fmt_t fmt, uint64_t pts_us,
                                 uint32_t timeout_ms)
{
    if (pcm == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    if (!g.inited || !g.started || g.pipeline == NULL) {
        return AURA_ERR_STATE;
    }
    return aura_pipeline_feed(g.pipeline, pcm, frame_count, channels, (aura_sample_fmt_t)fmt,
                              pts_us, timeout_ms);
}

aura_err_t aura_agent_feed_ref_audio(const void *pcm, uint32_t frame_count, uint32_t channels,
                                     aura_agent_audio_fmt_t fmt, uint64_t pts_us,
                                     uint32_t timeout_ms)
{
    if (pcm == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    if (!g.inited || !g.started || g.pipeline == NULL) {
        return AURA_ERR_STATE;
    }
    return aura_pipeline_feed_ref(g.pipeline, pcm, frame_count, channels, (aura_sample_fmt_t)fmt,
                                  pts_us, timeout_ms);
}

aura_err_t aura_agent_interrupt(void)
{
    if (!g.inited || g.pipeline == NULL) {
        return AURA_ERR_STATE;
    }
    /* 打断语义：清在途数据 + 广播 BARGE_IN（各节点据此停播/停推理）+ 状态机回 Listening。 */
    aura_err_t rc = aura_pipeline_flush(g.pipeline);
    if (g.bus != NULL) {
        aura_event_bus_post(g.bus, AURA_EVENT_BARGE_IN, 0, "external");
    }
    if (g.sm != NULL) {
        (void)aura_sm_handle(g.sm, AURA_TRIG_BARGE_IN);
    }
    return rc;
}

aura_err_t aura_agent_report_latency(void)
{
    aura_profiler_report();
    return AURA_OK;
}

/* ------------------------------------------------------------ 内部接口 */

aura_err_t aura_agent_add_node(aura_node_t *node)
{
    if (!g.inited || g.pipeline == NULL || node == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    if (g.started) {
        return AURA_ERR_STATE; /* 拓扑只允许在 start 前变更 */
    }
    aura_err_t rc = aura_pipeline_add(g.pipeline, node);
    if (rc == AURA_OK) {
        g.node_count++;
    }
    return rc;
}

aura_node_t *aura_agent_chain_node(const char *name)
{
    if (name == NULL) {
        return NULL;
    }
    for (uint32_t i = 0; i < g.chain_node_count; i++) {
        aura_node_t *n = g.chain_nodes[i];
        if (n != NULL && n->name != NULL && strcmp(n->name, name) == 0) {
            return n;
        }
    }
    return NULL;
}

aura_event_bus_t *aura_agent_bus(void)
{
    return g.bus;
}

aura_model_mgr_t *aura_agent_models(void)
{
    return g.models;
}

aura_sm_t *aura_agent_sm(void)
{
    return g.sm;
}
