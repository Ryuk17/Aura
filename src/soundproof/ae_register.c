/* Aura — soundproof 算法注册入口（实现） */
#include "soundproof/ae_register.h"

#include "aura_config.h"

#if AURA_WITH_SOUNDPROOF
#include "soundproof/ae_common.h"
#include "utils/logger/logger.h"

#define TAG "ae_register"

/* 注册是幂等的：agent init 失败后再次 init（或测试里先手工注册）不该在
 * 这里被 "already exists" 卡死。同名但不同描述表的情况由注册表本身拒绝覆盖，
 * 这里只把“已存在”视为“已就绪”。 */
static aura_err_t accept_existing(aura_err_t rc)
{
    return (rc == AURA_ERR_EXIST) ? AURA_OK : rc;
}

aura_err_t ae_register_all(void)
{
    aura_err_t rc = accept_existing(ae_audio_register_all());
    if (rc != AURA_OK) {
        return rc;
    }
    rc = accept_existing(ae_vad_register_all());
    if (rc != AURA_OK) {
        return rc;
    }

#if AURA_WITH_SOUNDPROOF_MNN
#if AURA_BUILD_KWS
    rc = accept_existing(ae_kws_register_all());
    if (rc != AURA_OK) {
        return rc;
    }
#endif
#if AURA_BUILD_VOICEPRINT
    rc = accept_existing(ae_sr_register_all());
    if (rc != AURA_OK) {
        return rc;
    }
#endif
#if AURA_BUILD_ASR
    rc = accept_existing(ae_asr_register_all());
    if (rc != AURA_OK) {
        return rc;
    }
#endif
#if AURA_BUILD_TTS
    rc = accept_existing(ae_tts_register_all());
    if (rc != AURA_OK) {
        return rc;
    }
#endif
#endif

    AURA_LOGI(TAG, "soundproof adapters registered (%u algorithms)", aura_algo_count());
    return AURA_OK;
}

#else /* AURA_WITH_SOUNDPROOF */

aura_err_t ae_register_all(void)
{
    return AURA_OK;
}

#endif
