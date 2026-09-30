#include "wav_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma pack(push, 1)
typedef struct {
    char     riff[4];
    uint32_t size;
    char     wave[4];
} wav_riff_t;

typedef struct {
    char     fmt_id[4];
    uint32_t fmt_size;
    uint16_t audio_format;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits;
} wav_fmt_t;

typedef struct {
    char     data_id[4];
    uint32_t data_size;
} wav_data_t;
#pragma pack(pop)

aura_err_t wav_read(const char *path, int16_t **samples, uint32_t *sample_count,
                    uint32_t *sample_rate)
{
    if (path == NULL || samples == NULL || sample_count == NULL || sample_rate == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    *samples = NULL;
    FILE *fp = fopen(path, "rb");
    if (fp == NULL) {
        return AURA_ERR_IO;
    }
    wav_riff_t riff;
    wav_fmt_t  fmt;
    wav_data_t data;
    aura_err_t rc = AURA_ERR_IO;

    if (fread(&riff, sizeof(riff), 1, fp) != 1 || memcmp(riff.riff, "RIFF", 4) != 0 ||
        memcmp(riff.wave, "WAVE", 4) != 0) {
        goto out;
    }
    if (fread(&fmt, sizeof(fmt), 1, fp) != 1 || memcmp(fmt.fmt_id, "fmt ", 4) != 0 ||
        fmt.audio_format != 1 || fmt.bits != 16) {
        goto out;
    }
    /* 跳过 fmt 块的剩余字节 */
    if (fmt.fmt_size > 16) {
        fseek(fp, (long)(fmt.fmt_size - 16), SEEK_CUR);
    }
    /* 找 data 块 */
    for (;;) {
        if (fread(&data, sizeof(data), 1, fp) != 1) {
            goto out;
        }
        if (memcmp(data.data_id, "data", 4) == 0) {
            break;
        }
        fseek(fp, (long)data.data_size, SEEK_CUR); /* 跳过其他块 */
    }

    uint32_t total = data.data_size / 2; /* 总样本数（各通道合计） */
    uint32_t channels = (fmt.channels >= 1) ? fmt.channels : 1;
    uint32_t n = total / channels;
    if (n == 0) {
        goto out;
    }
    int16_t *buf = (int16_t *)malloc((size_t)n * sizeof(int16_t));
    if (buf == NULL) {
        rc = AURA_ERR_NOMEM;
        goto out;
    }
    int16_t *tmp = (int16_t *)malloc((size_t)total * sizeof(int16_t));
    if (tmp == NULL) {
        free(buf);
        rc = AURA_ERR_NOMEM;
        goto out;
    }
    if (fread(tmp, sizeof(int16_t), total, fp) != total) {
        free(tmp);
        free(buf);
        goto out;
    }
    /* 立体声 → 单声道下混 */
    if (channels == 1) {
        memcpy(buf, tmp, (size_t)n * sizeof(int16_t));
    } else {
        for (uint32_t i = 0; i < n; i++) {
            int32_t acc = 0;
            for (uint32_t c = 0; c < channels; c++) {
                acc += tmp[i * channels + c];
            }
            buf[i] = (int16_t)(acc / (int32_t)channels);
        }
    }
    free(tmp);
    *samples      = buf;
    *sample_count = n;
    *sample_rate  = fmt.sample_rate;
    rc = AURA_OK;
out:
    fclose(fp);
    return rc;
}

aura_err_t wav_write(const char *path, const int16_t *samples, uint32_t sample_count,
                     uint32_t sample_rate)
{
    if (path == NULL || samples == NULL) {
        return AURA_ERR_INVALID_ARG;
    }
    FILE *fp = fopen(path, "wb");
    if (fp == NULL) {
        return AURA_ERR_IO;
    }
    uint32_t data_bytes = sample_count * 2;
    wav_riff_t riff = {{'R', 'I', 'F', 'F'}, 36 + data_bytes, {'W', 'A', 'V', 'E'}};
    wav_fmt_t  fmt  = {{'f', 'm', 't', ' '}, 16, 1, 1, sample_rate, sample_rate * 2, 2, 16};
    wav_data_t data = {{'d', 'a', 't', 'a'}, data_bytes};

    aura_err_t rc = AURA_ERR_IO;
    if (fwrite(&riff, sizeof(riff), 1, fp) != 1) {
        goto out;
    }
    if (fwrite(&fmt, sizeof(fmt), 1, fp) != 1) {
        goto out;
    }
    if (fwrite(&data, sizeof(data), 1, fp) != 1) {
        goto out;
    }
    if (fwrite(samples, 2, sample_count, fp) != sample_count) {
        goto out;
    }
    rc = AURA_OK;
out:
    fclose(fp);
    return rc;
}
