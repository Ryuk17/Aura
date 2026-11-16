/* Aura — 播放 sink 节点（实现） */
#include "core/playback/playback.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "core/algorithm.h"
#include "core/node.h"
#include "osal/osal.h"
#include "utils/logger/logger.h"

#define TAG "playback"

/* ------------------------------------------------------------ 后端注册 */

static aura_playback_backend_t g_backend;
static aura_playback_ref_fn    g_ref_fn;
static void                   *g_ref_user;
static bool                    g_backend_set;

static aura_err_t null_open(void *user, uint32_t sample_rate, uint32_t channels)
{
    (void)user;
    (void)sample_rate;
    (void)channels;
    return AURA_OK;
}

static aura_err_t null_write(void *user, const int16_t *pcm, uint32_t frame_count,
                             uint32_t channels, uint64_t pts_us)
{
    (void)user;
    (void)pcm;
    (void)frame_count;
    (void)channels;
    (void)pts_us;
    return AURA_OK;
}

static const aura_playback_backend_t kNullBackend = {
    .name  = "null",
    .user  = NULL,
    .open  = null_open,
    .write = null_write,
    .flush = NULL,
    .close = NULL,
};

void aura_playback_set_backend(const aura_playback_backend_t *backend)
{
    if (backend == NULL || backend->write == NULL) {
        g_backend     = kNullBackend;
        g_backend_set = false;
        return;
    }
    g_backend     = *backend;
    g_backend_set = true;
}

const aura_playback_backend_t *aura_playback_backend(void)
{
    return g_backend_set ? &g_backend : &kNullBackend;
}

void aura_playback_set_ref_sink(aura_playback_ref_fn fn, void *user)
{
    g_ref_fn   = fn;
    g_ref_user = user;
}

/* ------------------------------------------------------------ 节点 */

typedef struct playback_node {
    aura_node_t base;
    uint32_t    rate;
    uint32_t    channels;
    bool        opened;
    uint64_t    frames_written;
    uint64_t    flushes;
} playback_node_t;

static aura_err_t playback_start(aura_node_t *self)
{
    playback_node_t               *n = (playback_node_t *)self;
    const aura_playback_backend_t *b = aura_playback_backend();
    n->opened                        = false;
    if (b->open != NULL) {
        aura_err_t rc = b->open(b->user, n->rate, n->channels);
        if (rc != AURA_OK) {
            AURA_LOGE(TAG, "backend '%s' open failed: %s", b->name ? b->name : "-",
                      aura_strerror(rc));
            return rc;
        }
    }
    n->opened = true;
    AURA_LOGI(TAG, "playback started: %s @ %u Hz / %u ch", b->name ? b->name : "-", n->rate,
              n->channels);
    return AURA_OK;
}

static aura_err_t playback_stop(aura_node_t *self)
{
    playback_node_t               *n = (playback_node_t *)self;
    const aura_playback_backend_t *b = aura_playback_backend();
    if (n->opened && b->close != NULL) {
        b->close(b->user);
    }
    n->opened = false;
    return AURA_OK;
}

static aura_err_t playback_process_audio(aura_node_t *self, const aura_audio_frame_t *frame)
{
    playback_node_t               *n = (playback_node_t *)self;
    const aura_playback_backend_t *b = aura_playback_backend();
    if (!n->opened) {
        return AURA_ERR_STATE;
    }
    if (frame->fmt != AURA_SAMPLE_S16 || frame->frame_count == 0) {
        return AURA_ERR_INVALID_ARG;
    }

    aura_err_t rc = AURA_OK;
    if (b->write != NULL) {
        rc = b->write(b->user, (const int16_t *)frame->data, frame->frame_count, frame->channels,
                      frame->pts_us);
        if (rc != AURA_OK) {
            return rc;
        }
    }
    n->frames_written++;

    /* 回采给 AEC：与真正送播的数据保持一致（TTS 24k → SRC 16k 之后）。 */
    if (g_ref_fn != NULL) {
        g_ref_fn(g_ref_user, (const int16_t *)frame->data, frame->frame_count, frame->channels,
                 frame->pts_us);
    }
    return AURA_OK;
}

static aura_err_t playback_control(aura_node_t *self, aura_node_cmd_t cmd, const void *arg)
{
    (void)arg;
    playback_node_t               *n = (playback_node_t *)self;
    const aura_playback_backend_t *b = aura_playback_backend();
    if (cmd == AURA_CMD_FLUSH || cmd == AURA_CMD_STOP) {
        if (b->flush != NULL) {
            b->flush(b->user);
        }
        n->flushes++;
    }
    return AURA_OK;
}

static void playback_deinit(aura_node_t *self)
{
    (void)playback_stop(self);
}

static aura_err_t playback_selftest(aura_node_t *self)
{
    playback_node_t *n = (playback_node_t *)self;
    return n->opened ? AURA_OK : AURA_ERR_STATE;
}

static const aura_node_ops_t kPlaybackOps = {
    .start         = playback_start,
    .stop          = playback_stop,
    .process_audio = playback_process_audio,
    .control       = playback_control,
    .deinit        = playback_deinit,
    .selftest      = playback_selftest,
};

static aura_node_t *playback_create(const aura_algo_params_t *p, aura_err_t *err)
{
    if (p == NULL) {
        if (err != NULL) {
            *err = AURA_ERR_INVALID_ARG;
        }
        return NULL;
    }
    playback_node_t *n = (playback_node_t *)calloc(1, sizeof(*n));
    if (n == NULL) {
        if (err != NULL) {
            *err = AURA_ERR_NOMEM;
        }
        return NULL;
    }
    n->rate     = p->sample_rate;
    n->channels = p->in_channels ? p->in_channels : 1;

    aura_node_caps_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.consumes_audio = true;
    aura_node_init(&n->base, (p->instance_name != NULL) ? p->instance_name : "playback",
                   &kPlaybackOps, &caps);
    return &n->base;
}

static const aura_algo_desc_t kPlaybackDesc = {
    .name        = "playback",
    .version     = 1,
    .kind        = AURA_ALGO_KIND_DSP,
    .io          = {.consumes_audio = true},
    .param_specs = NULL,
    .param_spec_count = 0,
    .create      = playback_create,
};

aura_err_t aura_playback_register(void)
{
    aura_err_t rc = aura_algo_register(&kPlaybackDesc);
    return (rc == AURA_ERR_EXIST) ? AURA_OK : rc;
}
