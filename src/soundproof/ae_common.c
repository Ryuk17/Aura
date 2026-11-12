/* Aura — soundproof 适配层公共设施（实现） */
#include "soundproof/ae_common.h"

#include <stdio.h>
#include <string.h>

#include "audio_engine_def.h"
#include "utils/logger/logger.h"

#define TAG "soundproof"

static char g_model_root[256];

void ae_set_model_root(const char *dir)
{
    if (dir == NULL) {
        g_model_root[0] = '\0';
        return;
    }
    snprintf(g_model_root, sizeof(g_model_root), "%s", dir);
}

const char *ae_model_root(void)
{
    return g_model_root;
}

static bool path_is_absolute(const char *p)
{
    if (p == NULL || p[0] == '\0') {
        return false;
    }
    if (p[0] == '/' || p[0] == '\\') {
        return true;
    }
    /* Windows 盘符：C:\ / C:/ */
    if (p[1] == ':' && (p[2] == '/' || p[2] == '\\')) {
        return true;
    }
    return false;
}

const char *ae_resolve_path(const char *path, char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) {
        return NULL;
    }
    if (path == NULL || path[0] == '\0') {
        buf[0] = '\0';
        return buf;
    }
    if (path_is_absolute(path) || g_model_root[0] == '\0') {
        int n = snprintf(buf, cap, "%s", path);
        return (n > 0 && (size_t)n < cap) ? buf : NULL;
    }
    size_t root_len = strlen(g_model_root);
    const char *sep = (g_model_root[root_len - 1] == '/' || g_model_root[root_len - 1] == '\\')
                          ? ""
                          : "/";
    int n = snprintf(buf, cap, "%s%s%s", g_model_root, sep, path);
    return (n > 0 && (size_t)n < cap) ? buf : NULL;
}

aura_err_t ae_status_to_err(int status, const char *what)
{
    switch (status) {
    case AUDIO_ENGINE_SUCCESS:
        return AURA_OK;
    case AUDIO_ENGINE_ERR_NULL_POINTER:
        return AURA_ERR_INVALID_ARG;
    case AUDIO_ENGINE_ERR_INVALID_HANDLE:
    case AUDIO_ENGINE_ERR_NOT_INITIALIZED:
        return AURA_ERR_STATE;
    case AUDIO_ENGINE_ERR_INIT_FAILED:
        return AURA_ERR_DSP;
    case AUDIO_ENGINE_ERR_MODEL_LOAD_FAILED:
    case AUDIO_ENGINE_ERR_UNSUPPORTED_MODEL:
        return AURA_ERR_MODEL;
    case AUDIO_ENGINE_ERR_INVALID_PARAM:
    case AUDIO_ENGINE_ERR_AUDIO_TOO_SHORT:
    case AUDIO_ENGINE_ERR_UNSUPPORTED_INPUT:
        return AURA_ERR_INVALID_ARG;
    case AUDIO_ENGINE_ERR_PROCESS_FAILED:
    case AUDIO_ENGINE_ERR_FEATURE_FAILED:
    case AUDIO_ENGINE_ERR_INFERENCE_FAILED:
    case AUDIO_ENGINE_ERR_EMBEDDING_DIM_MISMATCH:
        return AURA_ERR_FAIL;
    default:
        break;
    }
    AURA_LOGW(TAG, "%s: unmapped soundproof status 0x%x", what ? what : "?", status);
    return AURA_ERR_FAIL;
}

bool ae_frame_ok(const aura_audio_frame_t *f, uint32_t channels, const char *node)
{
    if (f == NULL) {
        return false;
    }
    if (f->fmt != AURA_SAMPLE_S16) {
        AURA_LOGW(TAG, "%s: only S16 is supported (fmt=%d)", node ? node : "?", (int)f->fmt);
        return false;
    }
    if (channels != 0 && f->channels != channels) {
        AURA_LOGW(TAG, "%s: expected %u channels, got %u", node ? node : "?", channels,
                  (unsigned)f->channels);
        return false;
    }
    if (f->frame_count == 0 || (uint32_t)f->frame_count * f->channels > AE_MAX_SAMPLES) {
        AURA_LOGW(TAG, "%s: frame too large (%u x %u samples)", node ? node : "?",
                  (unsigned)f->frame_count, (unsigned)f->channels);
        return false;
    }
    if (f->data_bytes < (uint32_t)f->frame_count * f->channels * sizeof(int16_t)) {
        AURA_LOGW(TAG, "%s: data_bytes %u too small", node ? node : "?", f->data_bytes);
        return false;
    }
    return true;
}

aura_err_t ae_emit_audio(aura_node_t *self, const int16_t *data, uint32_t samples,
                         uint32_t sample_rate, uint32_t channels)
{
    if (self == NULL || data == NULL || samples == 0 || channels == 0) {
        return AURA_ERR_INVALID_ARG;
    }
    aura_audio_frame_t out;
    memset(&out, 0, sizeof(out));
    out.pts_us      = self->last_in_pts_valid ? self->last_in_pts_us : 0;
    out.sample_rate = sample_rate;
    out.channels    = (uint16_t)channels;
    out.frame_count = (uint16_t)samples;
    out.fmt         = AURA_SAMPLE_S16;
    out.data_bytes  = samples * channels * sizeof(int16_t);
    out.data        = (uint8_t *)(uintptr_t)data;
    return aura_node_emit_audio(self, &out);
}

void ae_name_copy(char *dst, size_t cap, const char *src)
{
    if (dst == NULL || cap == 0) {
        return;
    }
    if (src == NULL || src[0] == '\0') {
        snprintf(dst, cap, "ae");
        return;
    }
    snprintf(dst, cap, "%s", src);
}

int32_t ae_param_i32(const aura_algo_params_t *p, const char *name, int32_t def)
{
    return aura_algo_params_i32(p, name, def);
}

float ae_param_f32(const aura_algo_params_t *p, const char *name, float def)
{
    return aura_algo_params_f32(p, name, def);
}

bool ae_param_bool(const aura_algo_params_t *p, const char *name, bool def)
{
    return aura_algo_params_bool(p, name, def);
}

const char *ae_param_str(const aura_algo_params_t *p, const char *name, const char *def)
{
    return aura_algo_params_str(p, name, def);
}
