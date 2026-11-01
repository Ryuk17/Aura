/* voiceprint_enroll — 声纹离线注册工具
 *
 * 架构（与运行时零共享）：音频 → fbank → embedding（独立模型实例）→ 写入库文件。
 * 声纹节点初始化时读同一文件（见 src/voiceprint/vp_lib.h）。
 *
 * 用法：
 *   voiceprint_enroll <wav> <spk_id> --model <model.mnn> --lib <library.bin>
 *     [--list]   # 只列库内容
 *   wav：16k 单声道 16-bit PCM（多声道取通道 0）
 *   同名 spk_id 覆盖；不带 --lib 时默认 ./library.bin
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "inference/inference.h"
#include "utils/error.h"
#include "voiceprint/vp_lib.h"

static int usage(const char *argv0)
{
    printf("用法:\n"
           "  %s <wav> <spk_id> [--model X.mnn] [--lib library.bin]\n"
           "  %s --list [--lib library.bin]\n",
           argv0, argv0);
    return 2;
}

/* ---- 最小 WAV 读取（16-bit PCM，取通道 0） ---- */
static int16_t *wav16_load(const char *path, uint32_t *sr_out, uint32_t *frames_out)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    uint8_t hdr[12];
    if (fread(hdr, 1, 12, f) != 12 || memcmp(hdr, "RIFF", 4) != 0 ||
        memcmp(hdr + 8, "WAVE", 4) != 0) {
        fclose(f);
        return NULL;
    }
    uint16_t fmt = 0, bits = 0, channels = 0;
    uint32_t sr = 0, data_len = 0;
    for (;;) {
        uint8_t ck[8];
        if (fread(ck, 1, 8, f) != 8) {
            break;
        }
        uint32_t sz = (uint32_t)ck[4] | ((uint32_t)ck[5] << 8) |
                      ((uint32_t)ck[6] << 16) | ((uint32_t)ck[7] << 24);
        if (memcmp(ck, "fmt ", 4) == 0) {
            uint8_t fb[16];
            if (fread(fb, 1, 16, f) != 16) {
                break;
            }
            fmt      = (uint16_t)(fb[0] | (fb[1] << 8));
            channels = (uint16_t)(fb[2] | (fb[3] << 8));
            sr       = (uint32_t)fb[4] | ((uint32_t)fb[5] << 8) |
                 ((uint32_t)fb[6] << 16) | ((uint32_t)fb[7] << 24);
            bits = (uint16_t)(fb[14] | (fb[15] << 8));
            if (sz > 16) {
                fseek(f, (long)(sz - 16), SEEK_CUR);
            }
        } else if (memcmp(ck, "data", 4) == 0) {
            data_len = sz;
            break;
        } else {
            fseek(f, (long)(sz + (sz & 1u)), SEEK_CUR);
        }
    }
    if (fmt != 1 || bits != 16 || sr == 0 || channels == 0 || data_len == 0) {
        fclose(f);
        return NULL;
    }
    uint32_t n = data_len / (2u * channels);
    int16_t *tmp = (int16_t *)malloc(data_len);
    int16_t *out = (int16_t *)malloc((size_t)n * 2);
    if (tmp == NULL || out == NULL) {
        free(tmp);
        fclose(f);
        return NULL;
    }
    if (fread(tmp, 1, data_len, f) != data_len) {
        free(tmp);
        free(out);
        fclose(f);
        return NULL;
    }
    for (uint32_t i = 0; i < n; i++) {
        out[i] = tmp[(size_t)i * channels];
    }
    free(tmp);
    fclose(f);
    *sr_out     = sr;
    *frames_out = n;
    return out;
}

int main(int argc, char **argv)
{
    const char *wav   = NULL;
    const char *spk   = NULL;
    const char *model = "speaker_recognition/"
                        "3dspeaker_speech_eres2netv2_sv_zh-cn_16k-common.mnn";
    const char *lib   = "library.bin";
    bool        list  = false;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--list") == 0) {
            list = true;
        } else if (strcmp(argv[i], "--model") == 0 && i + 1 < argc) {
            model = argv[++i];
        } else if (strcmp(argv[i], "--lib") == 0 && i + 1 < argc) {
            lib = argv[++i];
        } else if (wav == NULL) {
            wav = argv[i];
        } else if (spk == NULL) {
            spk = argv[i];
        } else {
            return usage(argv[0]);
        }
    }

    aura_infer_cfg_t icfg;
    memset(&icfg, 0, sizeof(icfg));
    icfg.backend = AURA_INFER_BACKEND_CPU;
    icfg.threads = 2;
    aura_err_t rc = aura_infer_init(&icfg);
    if (rc != AURA_OK) {
        fprintf(stderr, "推理引擎初始化失败: %s\n", aura_strerror(rc));
        return 1;
    }

    if (list) {
        aura_vp_entry_t entries[AURA_VP_LIB_MAX];
        int32_t count = 0;
        rc = aura_vp_lib_load(lib, entries, AURA_VP_LIB_MAX, &count);
        if (rc != AURA_OK) {
            fprintf(stderr, "读库失败: %s（%s）\n", lib, aura_strerror(rc));
            aura_infer_deinit();
            return 1;
        }
        printf("库 %s：%d 人\n", lib, count);
        for (int32_t i = 0; i < count; i++) {
            printf("  [%d] %s\n", i, entries[i].id);
        }
        aura_infer_deinit();
        return 0;
    }

    if (wav == NULL || spk == NULL) {
        usage(argv[0]);
        aura_infer_deinit();
        return 2;
    }

    uint32_t sr = 0, n = 0;
    int16_t *pcm = wav16_load(wav, &sr, &n);
    if (pcm == NULL || sr != 16000) {
        fprintf(stderr, "读 WAV 失败或采样率 ≠ 16000: %s\n", wav);
        aura_infer_deinit();
        return 1;
    }

    float emb[AURA_VP_EMB_DIM];
    rc = aura_vp_embed_offline(pcm, n, sr, model, emb);
    free(pcm);
    if (rc != AURA_OK) {
        fprintf(stderr, "embedding 失败: %s\n", aura_strerror(rc));
        aura_infer_deinit();
        return 1;
    }

    /* 入库（同名覆盖） */
    aura_vp_entry_t entries[AURA_VP_LIB_MAX];
    int32_t count = 0;
    rc = aura_vp_lib_load(lib, entries, AURA_VP_LIB_MAX, &count);
    if (rc != AURA_OK && rc != AURA_ERR_NOT_FOUND) {
        fprintf(stderr, "读库失败: %s\n", aura_strerror(rc));
        aura_infer_deinit();
        return 1;
    }
    for (int32_t i = 0; i < count; i++) {
        if (strcmp(entries[i].id, spk) == 0) {
            memcpy(entries[i].emb, emb, sizeof(emb));
            goto save;
        }
    }
    if (count >= AURA_VP_LIB_MAX) {
        fprintf(stderr, "库已满（%d 人）\n", AURA_VP_LIB_MAX);
        aura_infer_deinit();
        return 1;
    }
    snprintf(entries[count].id, sizeof(entries[count].id), "%s", spk);
    memcpy(entries[count].emb, emb, sizeof(emb));
    count++;

save:
    rc = aura_vp_lib_save(lib, entries, count);
    if (rc != AURA_OK) {
        fprintf(stderr, "写库失败: %s\n", aura_strerror(rc));
        aura_infer_deinit();
        return 1;
    }
    printf("已注册 '%s' → %s（共 %d 人）\n", spk, lib, count);
    aura_infer_deinit();
    return 0;
}
