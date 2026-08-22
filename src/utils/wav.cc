#include "wav.h"

#include <cstdio>
#include <cstring>

namespace aura {

namespace {

struct RiffHeader {
    char riff[4];        // "RIFF"
    uint32_t chunk_size;
    char wave[4];        // "WAVE"
    char fmt[4];         // "fmt "
    uint32_t fmt_size;
    uint16_t audio_format;
    uint16_t channels;
    uint32_t sample_rate;
    uint32_t byte_rate;
    uint16_t block_align;
    uint16_t bits_per_sample;
};

bool IsRiff(const char* s) { return memcmp(s, "RIFF", 4) == 0; }
bool IsWave(const char* s) { return memcmp(s, "WAVE", 4) == 0; }
bool IsFmt(const char* s) { return memcmp(s, "fmt ", 4) == 0; }
bool IsData(const char* s) { return memcmp(s, "data", 4) == 0; }

}  // namespace

bool ReadWav(const std::string& path, std::vector<int16_t>* samples, WavInfo* info) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;

    bool ok = false;
    RiffHeader hdr;
    do {
        if (fread(&hdr, sizeof(hdr), 1, f) != 1) break;
        if (!IsRiff(hdr.riff) || !IsWave(hdr.wave) || !IsFmt(hdr.fmt)) break;
        if (hdr.audio_format != 1) break;  // 仅支持 PCM
        if (hdr.bits_per_sample != 16) break;

        // 跳过 fmt 之后的附加块（如 "fact"），找到 "data"
        long data_pos = sizeof(hdr) + hdr.fmt_size - 16;  // fmt 块剩余部分
        if (fseek(f, data_pos, SEEK_SET) != 0) break;

        char chunk_id[4];
        uint32_t chunk_size;
        bool found_data = false;
        while (fread(chunk_id, 4, 1, f) == 1 && fread(&chunk_size, 4, 1, f) == 1) {
            if (IsData(chunk_id)) {
                found_data = true;
                break;
            }
            if (chunk_size % 2 == 1) ++chunk_size;  // 对齐
            if (fseek(f, chunk_size, SEEK_CUR) != 0) break;
        }
        if (!found_data) break;

        size_t sample_count = chunk_size / 2;
        samples->resize(sample_count);
        if (sample_count > 0 && fread(samples->data(), 2, sample_count, f) != sample_count) break;

        if (info) {
            info->sample_rate = static_cast<int>(hdr.sample_rate);
            info->channels = hdr.channels;
            info->bits_per_sample = 16;
        }
        ok = true;
    } while (false);

    fclose(f);
    return ok;
}

bool WriteWav(const std::string& path, const int16_t* samples, size_t sample_count,
              int sample_rate, int channels) {
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;

    bool ok = false;
    RiffHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    memcpy(hdr.riff, "RIFF", 4);
    memcpy(hdr.wave, "WAVE", 4);
    memcpy(hdr.fmt, "fmt ", 4);
    hdr.fmt_size = 16;
    hdr.audio_format = 1;
    hdr.channels = static_cast<uint16_t>(channels);
    hdr.sample_rate = static_cast<uint32_t>(sample_rate);
    hdr.bits_per_sample = 16;
    hdr.block_align = static_cast<uint16_t>(channels * 2);
    hdr.byte_rate = hdr.sample_rate * hdr.block_align;
    hdr.chunk_size = static_cast<uint32_t>(36 + sample_count * 2);

    do {
        if (fwrite(&hdr, sizeof(hdr), 1, f) != 1) break;
        if (fwrite("data", 4, 1, f) != 1) break;
        uint32_t data_size = static_cast<uint32_t>(sample_count * 2);
        if (fwrite(&data_size, 4, 1, f) != 1) break;
        if (sample_count > 0 && fwrite(samples, 2, sample_count, f) != sample_count) break;
        ok = true;
    } while (false);

    fclose(f);
    return ok;
}

}  // namespace aura
