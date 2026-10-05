/* Aura — audio_debug 专用 WAV 读写（实现，约定见 wav_io.h） */
#include "wav_io.h"

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h> /* _mkdir */
#endif
#include <sys/stat.h> /* mkdir */

/* WAV 是小端。所有目标平台（x86_64 / aarch64 / armv7）都是小端，样本区直接按
 * int16_t 读写即可；真要跑大端平台，这里加一次逐样本字节翻转就行 —— 不值得
 * 为它现在就写一遍 memcpy 循环。 */

static uint32_t rd_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t rd_u16(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8);
}

static void wr_u32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
    p[2] = (uint8_t)((v >> 16) & 0xFF);
    p[3] = (uint8_t)((v >> 24) & 0xFF);
}

static void wr_u16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFF);
    p[1] = (uint8_t)((v >> 8) & 0xFF);
}

static void wav_data_clear(wav_data_t *d)
{
    if (d != NULL) {
        memset(d, 0, sizeof(*d));
    }
}

void wav_free(wav_data_t *d)
{
    if (d == NULL) {
        return;
    }
    free(d->samples);
    wav_data_clear(d);
}

aura_err_t wav_read_ex(const char *path, wav_data_t *out)
{
    if (path == NULL || out == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    wav_data_clear(out);

    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return AURA_ERR_IO;
    }

    uint8_t riff[12];
    if (fread(riff, 1, sizeof(riff), fp) != sizeof(riff) || memcmp(riff, "RIFF", 4) != 0 ||
        memcmp(riff + 8, "WAVE", 4) != 0) {
        fclose(fp);
        return AURA_ERR_INVALID_ARG;
    }

    uint32_t fmt_tag = 0, channels = 0, rate = 0, bits = 0, data_bytes = 0;
    bool     have_fmt = false, have_data = false;

    /* 逐个 chunk 走，直到拿到 data。不假设 fmt 一定在 data 前面，
     * 也不假设中间没有 LIST/fact 这类块（别的工具写出来的文件常有）。 */
    while (!have_data) {
        uint8_t ck[8];
        if (fread(ck, 1, sizeof(ck), fp) != sizeof(ck)) {
            break; /* 文件尾：data 缺失 */
        }
        const uint32_t csize = rd_u32(ck + 4);
        if (memcmp(ck, "fmt ", 4) == 0) {
            uint8_t f[16];
            const uint32_t need = (csize < sizeof(f)) ? csize : (uint32_t)sizeof(f);
            if (fread(f, 1, need, fp) != need) {
                break;
            }
            if (csize > need) {
                fseek(fp, (long)(csize - need), SEEK_CUR);
            }
            fmt_tag  = rd_u16(f);
            channels = rd_u16(f + 2);
            rate     = rd_u32(f + 4);
            bits     = (need >= 16) ? rd_u16(f + 14) : 0;
            have_fmt = true;
        } else if (memcmp(ck, "data", 4) == 0) {
            data_bytes = csize;
            have_data  = true;
        } else {
            /* 未知块跳过；chunk 长度为奇数时后面还有一个对齐字节 */
            fseek(fp, (long)(csize + (csize & 1u)), SEEK_CUR);
        }
    }

    if (!have_fmt || !have_data || fmt_tag != 1 || bits != 16 || channels == 0 || channels > 8 ||
        rate == 0) {
        fclose(fp);
        return AURA_ERR_INVALID_ARG;
    }

    const uint32_t frames = data_bytes / (channels * 2u);
    if (frames == 0) {
        fclose(fp);
        return AURA_ERR_INVALID_ARG;
    }
    int16_t *buf = (int16_t *)malloc((size_t)frames * channels * sizeof(int16_t));
    if (buf == NULL) {
        fclose(fp);
        return AURA_ERR_NOMEM;
    }
    const size_t want = (size_t)frames * channels;
    if (fread(buf, sizeof(int16_t), want, fp) != want) {
        free(buf);
        fclose(fp);
        return AURA_ERR_IO;
    }
    fclose(fp);

    out->samples     = buf;
    out->frames      = frames;
    out->channels    = channels;
    out->sample_rate = rate;
    return AURA_OK;
}

aura_err_t wav_read_interleave2(const char *path0, const char *path1, wav_data_t *out)
{
    if (path0 == NULL || path1 == NULL || out == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    wav_data_clear(out);

    wav_data_t a, b;
    aura_err_t rc = wav_read_ex(path0, &a);
    if (rc != AURA_OK) {
        return rc;
    }
    rc = wav_read_ex(path1, &b);
    if (rc != AURA_OK) {
        wav_free(&a);
        return rc;
    }
    if (a.channels != 1 || b.channels != 1 || a.sample_rate != b.sample_rate) {
        wav_free(&a);
        wav_free(&b);
        return AURA_ERR_INVALID_ARG;
    }
    const uint32_t frames = (a.frames < b.frames) ? a.frames : b.frames;
    const uint32_t rate   = a.sample_rate; /* wav_free 会清零，先取出来 */
    int16_t       *buf    = (int16_t *)malloc((size_t)frames * 2u * sizeof(int16_t));
    if (buf == NULL) {
        wav_free(&a);
        wav_free(&b);
        return AURA_ERR_NOMEM;
    }
    for (uint32_t i = 0; i < frames; i++) {
        buf[2u * i]      = a.samples[i];
        buf[2u * i + 1u] = b.samples[i];
    }
    wav_free(&a);
    wav_free(&b);

    out->samples     = buf;
    out->frames      = frames;
    out->channels    = 2;
    out->sample_rate = rate;
    return AURA_OK;
}

/* ------------------------------- 流式写 ------------------------------- */

struct wav_writer {
    FILE    *fp;
    uint32_t channels;
    uint32_t sample_rate;
    uint64_t frames;      /* 已写出的每通道样本数 */
};

wav_writer_t *wav_writer_open(const char *path, uint32_t sample_rate, uint32_t channels)
{
    if (path == NULL || sample_rate == 0 || channels == 0) {
        return NULL;
    }
    FILE *fp = fopen(path, "wb");
    if (fp == NULL) {
        return NULL;
    }
    wav_writer_t *w = (wav_writer_t *)calloc(1, sizeof(*w));
    if (w == NULL) {
        fclose(fp);
        return NULL;
    }
    w->fp          = fp;
    w->channels    = channels;
    w->sample_rate = sample_rate;

    uint8_t h[44];
    memset(h, 0, sizeof(h));
    memcpy(h, "RIFF", 4);
    memcpy(h + 8, "WAVE", 4);
    memcpy(h + 12, "fmt ", 4);
    wr_u32(h + 16, 16);                                   /* fmt chunk 长度 */
    wr_u16(h + 20, 1);                                    /* PCM */
    wr_u16(h + 22, (uint16_t)channels);
    wr_u32(h + 24, sample_rate);
    wr_u32(h + 28, sample_rate * channels * 2u);          /* byte rate */
    wr_u16(h + 32, (uint16_t)(channels * 2u));            /* block align */
    wr_u16(h + 34, 16);                                   /* 位深 */
    memcpy(h + 36, "data", 4);
    /* h+4（RIFF 长度）与 h+40（data 长度）留 0，close 时回填 */

    if (fwrite(h, 1, sizeof(h), fp) != sizeof(h)) {
        free(w);
        fclose(fp);
        return NULL;
    }
    return w;
}

aura_err_t wav_writer_write(wav_writer_t *w, const int16_t *samples, uint32_t frames)
{
    if (w == NULL || samples == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    if (frames == 0) {
        return AURA_OK;
    }
    const size_t want = (size_t)frames * w->channels;
    if (fwrite(samples, sizeof(int16_t), want, w->fp) != want) {
        return AURA_ERR_IO;
    }
    w->frames += frames;
    return AURA_OK;
}

aura_err_t wav_writer_close(wav_writer_t *w)
{
    if (w == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    const uint64_t data_bytes = w->frames * w->channels * 2u;
    aura_err_t     rc         = AURA_OK;

    if (data_bytes > 0xFFFFFFFFull - 36ull) {
        /* 4 GB 上限：调试工具用不到，遇上了说明哪个环节失控了 */
        rc = AURA_ERR_INVALID_ARG;
    } else {
        uint8_t l[4];
        wr_u32(l, (uint32_t)(36u + (uint32_t)data_bytes));
        if (fseek(w->fp, 4, SEEK_SET) != 0 || fwrite(l, 1, 4, w->fp) != 4) {
            rc = AURA_ERR_IO;
        }
        wr_u32(l, (uint32_t)data_bytes);
        if (rc == AURA_OK && (fseek(w->fp, 40, SEEK_SET) != 0 || fwrite(l, 1, 4, w->fp) != 4)) {
            rc = AURA_ERR_IO;
        }
    }
    if (fclose(w->fp) != 0 && rc == AURA_OK) {
        rc = AURA_ERR_IO;
    }
    free(w);
    return rc;
}

aura_err_t wav_ensure_dir(const char *dir)
{
    if (dir == NULL || dir[0] == '\0') {
        return AURA_ERR_INVALID_ARG;
    }
#ifdef _WIN32
    if (_mkdir(dir) == 0) {
        return AURA_OK;
    }
#else
    if (mkdir(dir, 0755) == 0) {
        return AURA_OK;
    }
#endif
    /* 已存在不算失败：调用方（工具与 CTest）本来就会反复往同一个目录落盘，
     * 每次跑都先删一遍才是多此一举。 */
    return (errno == EEXIST) ? AURA_OK : AURA_ERR_IO;
}
