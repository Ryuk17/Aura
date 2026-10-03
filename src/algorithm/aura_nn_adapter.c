/* Aura — NN 算法适配模板的实现（NN_SYNC 路径）
 *
 * 与 dsp/aura_dsp_adapter.c 是同构的两份：都把"引擎怎么调"收敛进描述表，
 * 把"接进 pipeline"的部分——攒帧、emit、边沿事件、控制面、统计——全族共享。
 * 阅读顺序建议先 dsp 那份（简单些），再回来看这里多出来的两件事：
 * 模型加载/卸载，和滑窗。
 */
#include "algorithm/aura_nn_adapter.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "utils/logger/logger.h"

#define TAG "nn"

/* 模型根目录（配置里的 models 路径）。只存指针 —— 调用方保证生命周期，
 * 通常是配置结构里的静态字符串或 main 的 argv 缓冲。 */
static const char *g_model_root;

aura_err_t aura_nn_set_model_root(const char *dir)
{
    g_model_root = dir;
    return AURA_OK;
}

const char *aura_nn_model_root(void)
{
    return g_model_root;
}

/* ============================== 滑窗 ============================== */

aura_err_t aura_nn_window_init(aura_nn_window_t *w, uint32_t ctx_samples, uint32_t new_samples)
{
    if (w == NULL || new_samples == 0) {
        return AURA_ERR_INVALID_ARG;
    }
    if (ctx_samples + new_samples + AURA_NN_WINDOW_FRAME_MAX > AURA_NN_WINDOW_BUF_MAX) {
        AURA_LOGE(TAG, "window %u+%u needs more than buf %d (minus one frame of %d)", ctx_samples,
                  new_samples, AURA_NN_WINDOW_BUF_MAX, AURA_NN_WINDOW_FRAME_MAX);
        return AURA_ERR_INVALID_ARG;
    }
    memset(w, 0, sizeof(*w));
    w->ctx_samples = ctx_samples;
    w->new_samples = new_samples;
    /* 首窗的上文补零：这样第一窗出现在第 new_samples 个样本处（而不是
     * ctx+new），与 silero 等参考实现的"零初始化上下文"口径一致。
     * 不补的话首窗会把前 ctx 个真实样本当上文吃掉，等于白丢一段语音。 */
    w->len = ctx_samples;
    return AURA_OK;
}

const float *aura_nn_window_push(aura_nn_window_t *w, const int16_t *pcm, uint32_t samples)
{
    if (w == NULL || pcm == NULL || w->new_samples == 0 || samples == 0) {
        return NULL;
    }
    if (samples > AURA_NN_WINDOW_FRAME_MAX) {
        /* 超过声明的一帧上限：多半是链上换了帧长（如 48k 全带宽）而没同步这个宏。
         * 按上限截断而不是越界写 —— 但这是配置问题，得让人看见。 */
        AURA_LOGW(TAG, "window push %u samples > frame max %d, truncated", samples,
                  AURA_NN_WINDOW_FRAME_MAX);
        samples = AURA_NN_WINDOW_FRAME_MAX;
    }
    uint32_t need = w->ctx_samples + w->new_samples;
    if (w->len + samples > AURA_NN_WINDOW_BUF_MAX) {
        /* 正常用法到不了这里（init 已按"一帧的余量"校验过容量）。只可能是
         * 调用方拿到窗口后没 consume 就接着 push —— 丢掉新来的样本并告警：
         * 保住那个还没被读走的窗，比保住最新一帧更接近正确。 */
        AURA_LOGW(TAG, "window not consumed (len=%u), dropping %u incoming samples", w->len,
                  samples);
        return NULL;
    }

    float *dst = w->buf + w->len;
    for (uint32_t i = 0; i < samples; i++) {
        dst[i] = (float)pcm[i] / 32768.f;
    }
    w->len += samples;

    if (w->len < need) {
        return NULL;
    }
    /* 窗取缓冲**头部**的 need 个样本（FIFO，最旧的先入窗）。
     * 取"最新 need 个"看着更自然，但那样夹在中间、还没进过任何窗的样本会被
     * 悄悄挤掉 —— 160 样本的帧配 512 的窗时，每窗丢 128 个样本。
     * 多出来的部分留在缓冲里，由 consume 接到下一次的窗首。 */
    return w->buf;
}

void aura_nn_window_consume(aura_nn_window_t *w)
{
    if (w == NULL) {
        return;
    }
    uint32_t need = w->ctx_samples + w->new_samples;
    uint32_t left = (w->len > need) ? (w->len - need) : 0;

    /* ① 本窗末尾 ctx 个样本 → 下一次的上文；② 窗后剩余的样本（left 个）紧跟其后，
     * 它们是下一个窗的开头，一个都不能丢。两步的源区间不重叠（第①步写 [0,ctx)，
     * 第②步读 [need, need+left)，而 ctx < need），顺序无所谓。 */
    if (w->ctx_samples > 0) {
        memmove(w->buf, w->buf + (need - w->ctx_samples), (size_t)w->ctx_samples * sizeof(float));
    }
    if (left > 0) {
        memmove(w->buf + w->ctx_samples, w->buf + need, (size_t)left * sizeof(float));
    }
    w->len = w->ctx_samples + left;
}

void aura_nn_window_reset(aura_nn_window_t *w)
{
    if (w == NULL) {
        return;
    }
    /* 只清数据不清配置：FLUSH 后还要接着用同一个窗。
     * 回到与 init 相同的状态（上文补零），而不是 len=0 —— 否则打断后的
     * 第一窗会短 ctx 个样本，喂给模型的张量形状就不对了。 */
    memset(w->buf, 0, sizeof(w->buf));
    w->len = w->ctx_samples;
}

/* ============================== 节点内取值 ============================== */

void *aura_nn_state(const aura_nn_node_t *self)
{
    return (self == NULL) ? NULL : (void *)(uintptr_t)self->state;
}

aura_infer_model_t *aura_nn_model(aura_nn_node_t *self)
{
    return (self == NULL) ? NULL : self->model;
}

bool aura_nn_flag(const aura_nn_node_t *self)
{
    return (self != NULL) && self->flag_valid && (self->last_flag != 0);
}

/* 边沿检测 + 发事件。与 dsp_handle_flag 同形：只发边沿，每帧发会刷爆总线。 */
void aura_nn_report(aura_nn_node_t *self, bool hit)
{
    if (self == NULL) {
        return;
    }
    int v = hit ? 1 : 0;

    if (self->flag_valid && v == self->last_flag) {
        return;
    }
    bool rise        = (v != 0);
    self->flag_valid = true;
    self->last_flag  = v;
    if (rise) {
        self->flag_rise++;
    } else {
        self->flag_fall++;
    }

    int type = rise ? self->desc->flag_rise_type : self->desc->flag_fall_type;
    if (type != AURA_EVENT_NONE) {
        aura_node_post_event(&self->base, type, v);
    }
}

/* ============================== 模型路径 ============================== */

/* params.model_path（配置/链描述显式指定）优先，其次 model_root + desc.model_file。
 * 两者都空 = 无模型算法，返回 AURA_ERR_NOT_FOUND 由调用方当 NULL 处理。 */
static aura_err_t nn_resolve_model_path(const aura_nn_node_t *n, char *out, size_t cap)
{
    const char *p = n->params.model_path;
    if (p != NULL && p[0] != '\0') {
        snprintf(out, cap, "%s", p);
        return AURA_OK;
    }
    if (n->desc->model_file == NULL) {
        return AURA_ERR_NOT_FOUND;
    }
    if (g_model_root == NULL) {
        AURA_LOGE(TAG, "%s: needs '%s' but model root is unset (aura_nn_set_model_root)",
                  n->base.name, n->desc->model_file);
        return AURA_ERR_STATE;
    }
    int wrote = snprintf(out, cap, "%s/%s", g_model_root, n->desc->model_file);
    if (wrote < 0 || (size_t)wrote >= cap) {
        AURA_LOGE(TAG, "%s: model path too long (%s/%s)", n->base.name, g_model_root,
                  n->desc->model_file);
        return AURA_ERR_INVALID_ARG;
    }
    return AURA_OK;
}

/* ============================== 生命周期 ============================== */

static aura_err_t nn_init(aura_node_t *self)
{
    aura_nn_node_t *n = (aura_nn_node_t *)self;

    /* 模型在 init（= pipeline start）时加载：此时还没开闸放音频，
     * 加载耗时（首响延迟的大头）不落在音频线程上，失败也能让 start 直接报错。 */
    char path[512];
    aura_err_t rc = nn_resolve_model_path(n, path, sizeof(path));
    if (rc == AURA_OK) {
        n->model = aura_infer_load(path, n->desc->form);
        if (n->model == NULL) {
            AURA_LOGE(TAG, "%s: load failed: %s", self->name, path);
            return AURA_ERR_MODEL;
        }
        aura_model_info_t info;
        if (aura_model_get_info(n->model, &info) == AURA_OK) {
            AURA_LOGI(TAG, "%s: model loaded (%s, %s, %d in / %d out, load %llu us)", self->name,
                      path, aura_infer_form_name(n->desc->form), info.input_count,
                      info.output_count, (unsigned long long)info.load_us);
        }
    } else if (rc != AURA_ERR_NOT_FOUND) {
        return rc; /* 无模型算法是 AURA_ERR_NOT_FOUND，其余都是真错误 */
    }

    if (n->desc->algo_init != NULL) {
        rc = n->desc->algo_init(n, n->model);
        if (rc != AURA_OK) {
            AURA_LOGE(TAG, "%s: algo_init failed: %s", self->name, aura_strerror(rc));
            if (n->model != NULL) {
                aura_infer_unload(n->model);
                n->model = NULL;
            }
            return rc;
        }
    }
    return AURA_OK;
}

static aura_err_t nn_stop(aura_node_t *self)
{
    aura_nn_node_t *n = (aura_nn_node_t *)self;

    if (n->desc->algo_deinit != NULL) {
        n->desc->algo_deinit(n); /* 必须早于 unload：它可能要读模型句柄释放张量绑定 */
    }
    if (n->model != NULL) {
        aura_infer_unload(n->model);
        n->model = NULL;
    }
    return AURA_OK;
}

static void nn_deinit(aura_node_t *self)
{
    /* stop 已卸过就是空操作；"没 start 就 destroy"的路径也走这里。 */
    nn_stop(self);
}

static aura_err_t nn_reset_state(aura_node_t *self)
{
    aura_nn_node_t *n = (aura_nn_node_t *)self;

    /* 判定状态必须一起清：清了滑窗却留着 last_flag，打断后第一帧就会被
     * 边沿检测吞掉（"之前就是 1，现在还是 1，不发事件"）。 */
    n->flag_valid = false;
    n->last_flag  = 0;

    if (n->desc->algo_reset != NULL) {
        return n->desc->algo_reset(n); /* 算法清自己的滑窗 / LSTM 状态 */
    }
    return AURA_OK;
}

static aura_err_t nn_control(aura_node_t *self, aura_node_cmd_t cmd, const void *arg)
{
    (void)arg;
    switch (cmd) {
    case AURA_CMD_START:
    case AURA_CMD_STOP:
        return AURA_OK; /* 模型在 init/stop 时装卸，这里无额外动作 */
    case AURA_CMD_RESET:
    case AURA_CMD_FLUSH:
        return nn_reset_state(self);
    default:
        return AURA_ERR_UNSUPPORTED;
    }
}

/* ============================== 处理 ============================== */

static aura_err_t nn_process_audio(aura_node_t *self, const aura_audio_frame_t *frame)
{
    aura_nn_node_t *n = (aura_nn_node_t *)self;

    if (frame->fmt != AURA_SAMPLE_S16 || frame->channels != n->channels) {
        n->dropped++;
        /* 不在这里 report_error：返回值已经让 pipeline 代为上抛并计数，
         * 再报一次会让总线每失败一帧收到两个 ERROR 事件（dsp 侧踩过）。 */
        return AURA_ERR_INVALID_ARG;
    }
    n->frames_in++;

    bool       ready = (n->desc->frame_in != NULL) ? n->desc->frame_in(n, frame) : false;
    aura_err_t rc    = AURA_OK;

    if (ready) {
        /* 窗口数：与"是否真跑了前向"无关 —— 无模型算法（纯逻辑/自检节点）
         * 一样会攒窗，只是没得跑。按模型调用计数会让它们永远显示 0 窗。 */
        n->infer_calls++;

        if (n->model != NULL) {
            uint64_t   t0 = aura_osal_time_us();
            aura_err_t ir = aura_model_run(n->model);
            uint64_t   dt = aura_osal_time_us() - t0;

            n->infer_us_total += dt;
            if (dt > n->infer_us_max) {
                n->infer_us_max = dt;
            }
            /* 预算 = 一帧时长。超了只告警不丢帧：丢帧会把"算法太慢"变成
             * "音频断断续续"，反而更难查（且 sync 类算法的下游是音频链，
             * 少一帧就是实打实的语音缺口）。 */
            uint64_t budget = (uint64_t)n->frame_len * 1000000ull / (n->rate ? n->rate : 1);
            if (budget > 0 && dt > budget) {
                AURA_LOGW(TAG, "%s: inference %llu us > frame budget %llu us (nn_sync)", self->name,
                          (unsigned long long)dt, (unsigned long long)budget);
            }
            if (ir != AURA_OK) {
                n->dropped++;
                rc = ir;
                if (!n->err_logged) {
                    n->err_logged = true;
                    AURA_LOGW(TAG, "%s: inference failed: %s", self->name, aura_strerror(ir));
                }
            }
        }
        /* 前向失败时输出张量内容无意义，跳过读取（infer_out 里不该自己判错误码）。 */
        if (rc == AURA_OK && n->desc->infer_out != NULL) {
            rc = n->desc->infer_out(n);
        }
    }

    if (n->desc->io_kind == AURA_NN_IO_PASSTHROUGH) {
        /* 透传：判定走事件，音频不动地交给下游。直接复用输入帧 ——
         * emit 内部会拷进 pipeline 自己的池帧（见 node.h 的数据生命周期约定）。 */
        aura_node_emit_audio(self, frame);
    }
    return rc;
}

/* 事件在 bus 分发任务里到达：只转发给算法，不做任何框架侧解释
 * （事件语义是算法的事，框架只负责把事件送到它面前）。 */
static void nn_on_event(aura_node_t *self, const aura_event_t *event)
{
    aura_nn_node_t *n = (aura_nn_node_t *)self;
    if (n->desc->on_event != NULL) {
        n->desc->on_event(n, event);
    }
}

static const aura_node_ops_t NN_OPS = {
    .init          = nn_init,
    .stop          = nn_stop,
    .deinit        = nn_deinit,
    .control       = nn_control,
    .process_audio = nn_process_audio,
    .on_event      = nn_on_event,
};

/* ============================== 工厂 ============================== */

/* 全族共用一个工厂：靠 params->provider 取回自己的描述表
 * （aura_nn_desc_t 的首成员是内嵌的 algo，故指针可直接互转）。 */
static aura_node_t *nn_create_node(const aura_algo_params_t *params, aura_err_t *err)
{
    if (params == NULL || params->provider == NULL) {
        if (err) {
            *err = AURA_ERR_INVALID_ARG;
        }
        return NULL;
    }
    const aura_nn_desc_t *d = (const aura_nn_desc_t *)params->provider;

    if (params->sample_rate == 0 || params->in_channels == 0 || params->in_channels > 8) {
        if (err) {
            *err = AURA_ERR_INVALID_ARG;
        }
        return NULL;
    }

    /* 一次分配：节点 + 私有状态（运行期不再 malloc，见 todo.md 4.4）。
     * state 是柔性数组，位于结构体尾，创建时已清零。 */
    aura_nn_node_t *n = (aura_nn_node_t *)calloc(1, sizeof(*n) + d->state_size);
    if (n == NULL) {
        if (err) {
            *err = AURA_ERR_NOMEM;
        }
        return NULL;
    }
    n->desc      = d;
    n->rate      = params->sample_rate;
    n->channels  = params->in_channels;
    n->frame_len = n->rate / 100; /* TrickRoom/窗口口径统一在 10ms */

    /* 参数快照：init 在 pipeline start 时才跑，那时栈上的 params 早已失效。 */
    n->params               = *params;
    snprintf(n->namebuf, sizeof(n->namebuf), "%s",
             (params->instance_name != NULL) ? params->instance_name : d->name);
    n->params.instance_name = n->namebuf; /* 不再指向中转瞬即逝的链描述 */
    n->params.provider      = NULL;       /* 快照里不留描述表指针，避免误导 */

    aura_node_caps_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.consumes_audio = (d->io_kind != AURA_NN_IO_ASYNC_SOURCE);
    caps.produces_audio = (d->io_kind != AURA_NN_IO_OBSERVER);
    caps.wants_events   = d->wants_events;
    /* 文本产出（ASR）与 changes_frame_rate 属于 NN_ASYNC 那一批，
     * finalize 现在会拒绝，故此处无需声明。 */
    aura_node_init(&n->base, n->namebuf, &NN_OPS, &caps);
    return &n->base;
}

/* ============================== 注册 ============================== */

aura_err_t aura_nn_finalize(aura_nn_desc_t *desc)
{
    if (desc == NULL || desc->name == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    /* 未实现的形态在这里就拒绝，而不是让一个"看起来能用"的异步节点在
     * 运行期静默丢结果 —— 接口留好与假装实现是两回事（见文件头注释）。 */
    if (desc->kind == AURA_ALGO_KIND_NN_ASYNC) {
        AURA_LOGE(TAG, "algo %s: NN_ASYNC not implemented yet (Phase 3/4)", desc->name);
        return AURA_ERR_UNSUPPORTED;
    }
    if (desc->kind != AURA_ALGO_KIND_NN_SYNC) {
        AURA_LOGE(TAG, "algo %s: nn adapter only takes NN_SYNC (got %s)", desc->name,
                  aura_algo_kind_name(desc->kind));
        return AURA_ERR_INVALID_ARG;
    }
    if (desc->io_kind == AURA_NN_IO_ASYNC_SOURCE) {
        AURA_LOGE(TAG, "algo %s: async audio source (TTS) not implemented yet", desc->name);
        return AURA_ERR_UNSUPPORTED;
    }
    if (desc->io_kind != AURA_NN_IO_OBSERVER && desc->io_kind != AURA_NN_IO_PASSTHROUGH) {
        AURA_LOGE(TAG, "algo %s: bad io_kind %d", desc->name, (int)desc->io_kind);
        return AURA_ERR_INVALID_ARG;
    }
    if (desc->wants_events && desc->on_event == NULL) {
        /* 订阅了事件却没人处理：多半是描述表抄漏了一行，运行期表现为
         * "策略没生效"而毫无报错。 */
        AURA_LOGE(TAG, "algo %s: wants_events without on_event", desc->name);
        return AURA_ERR_INVALID_ARG;
    }
    if (desc->frame_in != NULL && desc->state_size == 0) {
        /* 有逐帧回调却没有状态可放：多半是忘了写 state_size。
         * 状态为零字节的算法只需声明一个占位 struct 即可。 */
        AURA_LOGE(TAG, "algo %s: frame_in without state_size", desc->name);
        return AURA_ERR_INVALID_ARG;
    }

    aura_algo_io_t io;
    memset(&io, 0, sizeof(io));
    io.consumes_audio = (desc->io_kind != AURA_NN_IO_ASYNC_SOURCE);
    io.produces_audio = (desc->io_kind != AURA_NN_IO_OBSERVER);
    io.wants_events   = desc->wants_events;

    desc->algo.name             = desc->name;
    desc->algo.version          = 1;
    desc->algo.kind             = desc->kind;
    desc->algo.io               = io;
    desc->algo.shape_out        = NULL;
    desc->algo.param_specs      = desc->param_specs;
    desc->algo.param_spec_count = desc->param_spec_count;
    desc->algo.create           = nn_create_node;
    return aura_algo_register(&desc->algo);
}

aura_err_t aura_nn_register_all(aura_nn_desc_t *descs, uint32_t count)
{
    if (descs == NULL) {
        return (count == 0) ? AURA_OK : AURA_ERR_INVALID_ARG;
    }
    for (uint32_t i = 0; i < count; i++) {
        aura_err_t rc = aura_nn_finalize(&descs[i]);
        if (rc != AURA_OK) {
            AURA_LOGE(TAG, "nn desc[%u] (%s) finalize failed: %s", i,
                      (descs[i].name != NULL) ? descs[i].name : "?", aura_strerror(rc));
            return rc;
        }
    }
    return AURA_OK;
}

aura_err_t aura_nn_init(const char *model_root, const aura_infer_cfg_t *cfg)
{
    aura_nn_set_model_root(model_root);
    return aura_infer_init(cfg);
}

aura_err_t aura_nn_get_stats(const aura_node_t *node, aura_nn_stats_t *out)
{
    /* 用 ops 指针认亲：拿到别的族的节点时 ops 不同，直接拒绝。
     * 靠 desc/其它字段判断的话，读到的是别的结构体的内存（UB）。 */
    if (node == NULL || out == NULL || node->ops != &NN_OPS) {
        return AURA_ERR_INVALID_ARG;
    }
    const aura_nn_node_t *n = (const aura_nn_node_t *)node;

    memset(out, 0, sizeof(*out));
    out->frames_in    = n->frames_in;
    out->windows      = n->infer_calls;
    out->infer_us_max = n->infer_us_max;
    out->infer_us_avg = (n->infer_calls > 0) ? (n->infer_us_total / n->infer_calls) : 0;
    out->flag_rise    = n->flag_rise;
    out->flag_fall    = n->flag_fall;
    out->errors       = n->base.stats.errors;
    out->dropped      = n->dropped;
    return AURA_OK;
}
