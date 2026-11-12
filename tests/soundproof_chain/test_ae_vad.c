/* Aura — soundproof VAD 适配链（真实算法，model-free）
 *
 * 用 soundproof 自带的 audio_short16.wav 驱动 `chain = vad`（WebRTC 模式），
 * 断言 Aura 侧收到 VAD_SPEECH_START / VAD_SPEECH_END。
 *
 * 这是适配层闭环证据：
 *   soundproof AudioEngine_Vad_* → src/soundproof/ae_vad_node.c →
 *   core/algorithm 注册表 → pipeline → event_bus → 对外回调。
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "agent_export.h"
#include "osal/osal.h"
#include "wav_io.h"

#define VAD_RATE 16000
#define VAD_FRAME 160 /* 10ms */

static volatile int g_starts;
static volatile int g_ends;

static void on_event(const aura_agent_event_data_t *ev, void *user)
{
    (void)user;
    if (ev->type == AURA_AGENT_EVENT_VAD_START) {
        g_starts++;
    } else if (ev->type == AURA_AGENT_EVENT_VAD_END) {
        g_ends++;
    }
}

int main(void)
{
    const char *wav_path = AURA_SOUNDPROOF_SOURCE_DIR "/assets/audio/audio_short16.wav";
    int16_t    *pcm      = NULL;
    uint32_t    count    = 0;
    uint32_t    rate     = 0;
    if (wav_read(wav_path, &pcm, &count, &rate) != AURA_OK) {
        printf("SKIP: cannot read %s\n", wav_path);
        return 0;
    }
    if (rate != VAD_RATE) {
        printf("SKIP: expected %d Hz, got %u Hz\n", VAD_RATE, rate);
        free(pcm);
        return 0;
    }

    aura_agent_chain_param_t params[2] = {
        {"vad.vad_mode", "0"}, /* WebRTC DSP: no model */
        {"vad.threshold", "0.5"},
    };
    aura_agent_config_t cfg;
    aura_agent_config_default(&cfg);
    /* 链首必须有音频生产者：ns 做透传，vad 挂在 tee 旁路。 */
    cfg.chain           = "ns, tee(vad)";
    cfg.chain_params    = params;
    cfg.chain_param_count = 2;
    cfg.profiler_enable = false;

    aura_err_t rc = aura_agent_init(&cfg);
    if (rc != AURA_OK) {
        printf("FAIL: aura_agent_init: %s\n", aura_strerror(rc));
        free(pcm);
        return 1;
    }
    (void)aura_agent_set_event_callback(on_event, NULL);
    rc = aura_agent_start();
    if (rc != AURA_OK) {
        printf("FAIL: aura_agent_start: %s\n", aura_strerror(rc));
        aura_agent_deinit();
        free(pcm);
        return 1;
    }

    /* 只喂前 5 秒，足够触发多段语音；WAIT_FOREVER 提供自然背压。 */
    uint32_t limit = count < (5u * VAD_RATE) ? count : (5u * VAD_RATE);
    uint32_t fed   = 0;
    for (uint32_t i = 0; i + VAD_FRAME <= limit; i += VAD_FRAME) {
        rc = aura_agent_feed_audio(pcm + i, VAD_FRAME, 1, AURA_AGENT_AUDIO_S16, 0,
                                   AURA_AGENT_WAIT_FOREVER);
        if (rc != AURA_OK) {
            printf("FAIL: feed at frame %u: %s\n", i / VAD_FRAME, aura_strerror(rc));
            break;
        }
        fed += VAD_FRAME;
    }
    /* 事件分发是异步的；等节点队列排空 + bus 分发。 */
    aura_osal_sleep_ms(800);

    aura_agent_stop();
    aura_agent_deinit();
    free(pcm);

    printf("soundproof VAD: fed %u samples (%.1fs), starts=%d ends=%d\n", fed,
           (double)fed / VAD_RATE, g_starts, g_ends);
    if (g_starts <= 0) {
        printf("== test_ae_vad FAIL (no speech start event) ==\n");
        return 1;
    }
    printf("== test_ae_vad PASS ==\n");
    return 0;
}
