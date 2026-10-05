/* Aura — 语音前端测试台：装真链 + 插探针 + 喂数据 + 收尾
 *
 * tests/audio_front 下的两个测试（合成回声场景 / 真实阵列录音）共用这一份。
 * 共用不是图省事：链的装配顺序、帧池块数、喂帧节奏、收尾顺序里任何一处两边写得不
 * 一样，两个测试就不再跑在"同一条链"上，一边的结论也解释不了另一边的现象。
 * （与 tools/audio_debug 复用 front_probe.c 的理由是同一条：这类"基础设置各写一遍"
 * 的重复，最后都表现为没人解释得清的两套结果。）
 *
 * 只管"把链跑起来"，不管指标 —— 指标是各测试自己的事（合成场景能算 ERLE，
 * 真实录音没有参考就算不了）。
 */
#ifndef AURA_TESTS_AUDIO_FRONT_FRONT_RIG_H
#define AURA_TESTS_AUDIO_FRONT_FRONT_RIG_H

#include <stdio.h>
#include <string.h>

#include "core/algorithm.h"
#include "core/config/config.h"
#include "core/pipeline/pipeline.h"
#include "dsp/aura_dsp_adapter.h"
#include "front_probe.h"
#include "utils/error.h"
#include "wav_io.h"
#include "wavtap.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct front_rig {
    aura_pipeline_t      *pipe;
    aura_node_t          *nodes[AURA_CHAIN_MAX_LINKS];
    uint32_t              ncount;
    aura_probe_tap_t      taps[AURA_PROBE_MAX_TAPS];
    uint32_t              ntap;
    aura_pipeline_stats_t ps;
    uint32_t              frames_fed;
} front_rig_t;

/* 按 conf 组装真链、插探针、跑完 mic（+可选 ref）后停止并留下统计。
 * ref 为 NULL = 无参考（AEC 走"缺参考 → 静音顶替"的降级路径，真实录音场景）。
 * out_dir 不存在会被建出来；写不出文件时 wavtap 只记 write_errors，所以这里
 * 建目录失败直接返回错误，不留给后面猜。 */
static inline aura_err_t front_rig_run(front_rig_t *rig, const char *conf_path, const char *out_dir,
                                       const int16_t *mic, uint32_t mic_ch, const int16_t *ref,
                                       uint32_t ref_ch, uint32_t frames, uint32_t frame_count)
{
    memset(rig, 0, sizeof(*rig));

    if (wavtap_register() != AURA_OK || aura_dsp_trickroom_register() != AURA_OK) {
        fprintf(stderr, "算法注册失败\n");
        return AURA_ERR_STATE;
    }
    if (wav_ensure_dir(out_dir) != AURA_OK) {
        fprintf(stderr, "建输出目录 %s 失败\n", out_dir);
        return AURA_ERR_IO;
    }
    wavtap_set_out_dir(out_dir);

    aura_config_t cfg;
    aura_config_default(&cfg);
    aura_err_t rc = aura_config_load_file(&cfg, conf_path);
    if (rc != AURA_OK) {
        fprintf(stderr, "读配置 %s 失败: %s\n", conf_path, aura_strerror(rc));
        return rc;
    }

    aura_chain_t chain;
    aura_chain_init(&chain);
    rc = aura_chain_from_config(&cfg, &chain);
    if (rc != AURA_OK) {
        return rc;
    }

    aura_chain_t probe;
    rc = aura_probe_chain(&chain, &probe, rig->taps, &rig->ntap);
    if (rc != AURA_OK) {
        return rc;
    }

    aura_pipeline_cfg_t pc;
    aura_pipeline_config_default(&pc);
    pc.sample_rate = cfg.sample_rate;
    pc.frame_ms    = cfg.frame_ms;
    pc.channels    = cfg.channels;
    /* 扇出成倍消耗帧池：探针链上同一帧同时在途"链长 × 扇出"份，默认 16 块会以
     * frames_dropped 的形式丢帧（不报错，只让所有指标悄悄偏掉）。 */
    pc.frame_pool_blocks = 32;

    rig->pipe = aura_pipeline_create(&pc);
    if (rig->pipe == NULL) {
        return AURA_ERR_NOMEM;
    }
    rc = aura_chain_build(rig->pipe, &probe, rig->nodes, &rig->ncount);
    if (rc != AURA_OK) {
        aura_pipeline_destroy(rig->pipe);
        rig->pipe = NULL;
        return rc;
    }
    rc = aura_pipeline_start(rig->pipe);
    if (rc != AURA_OK) {
        aura_pipeline_stop(rig->pipe);
        aura_pipeline_destroy(rig->pipe);
        aura_chain_destroy_nodes(rig->nodes, rig->ncount);
        rig->pipe = NULL;
        return rc;
    }

    aura_probe_input_t in;
    memset(&in, 0, sizeof(in));
    in.mic          = mic;
    in.mic_channels = mic_ch;
    in.ref          = ref;
    in.ref_channels = ref_ch;
    in.frames       = frames;
    in.frame_count  = frame_count;
    in.frame_ms     = cfg.frame_ms;
    rig->frames_fed = frames;

    /* 喂帧节奏（同轴参考 + 近端）在 front_probe.c 里，与 audio_debug 是同一份：
     * 差一帧不报错，只会让 ERLE 静默归零。 */
    rc = aura_probe_feed(rig->pipe, &in);
    if (rc != AURA_OK) {
        fprintf(stderr, "喂数据失败: %s\n", aura_strerror(rc));
    }

    const aura_err_t drc = aura_pipeline_wait_drained(rig->pipe, 30000);
    /* stop → 各节点 deinit → wavtap 回填 WAV 头。统计必须在 stop 之后取。 */
    aura_pipeline_stop(rig->pipe);
    aura_pipeline_stats(rig->pipe, &rig->ps);
    return (rc != AURA_OK) ? rc : drc;
}

static inline void front_rig_done(front_rig_t *rig)
{
    if (rig->pipe != NULL) {
        aura_pipeline_destroy(rig->pipe);
        aura_chain_destroy_nodes(rig->nodes, rig->ncount);
        rig->pipe = NULL;
    }
}

/* 按**算法名**找探针：链元素的实例名可以改（conf 里给了 name），算法名不会。
 * 拿位置当身份的话，改一次配置就会静默指向别的节点。 */
static inline const aura_probe_tap_t *front_find_tap(const front_rig_t *rig, const char *algo)
{
    for (uint32_t i = 0; i < rig->ntap; i++) {
        if (strcmp(rig->taps[i].algo, algo) == 0) {
            return &rig->taps[i];
        }
    }
    return NULL;
}

/* rig 取非 const：aura_probe_find 收 `aura_node_t **`（它不写节点，但签名如此），
 * 传 const 指针会触发一次没意义的 const 丢弃告警。 */
static inline bool front_tap_stats(front_rig_t *rig, const char *algo, wavtap_stats_t *out)
{
    const aura_probe_tap_t *t    = front_find_tap(rig, algo);
    aura_node_t            *node = (t != NULL) ? aura_probe_find(rig->nodes, rig->ncount, t->tap)
                                               : NULL;
    return (node != NULL) && (wavtap_get_stats(node, out) == AURA_OK);
}

static inline bool front_node_stats(front_rig_t *rig, const char *algo, aura_dsp_stats_t *out)
{
    const aura_probe_tap_t *t    = front_find_tap(rig, algo);
    aura_node_t            *node = (t != NULL) ? aura_probe_find(rig->nodes, rig->ncount, t->node)
                                               : NULL;
    return (node != NULL) && (aura_dsp_get_stats(node, out) == AURA_OK);
}

#ifdef __cplusplus
}
#endif

#endif /* AURA_TESTS_AUDIO_FRONT_FRONT_RIG_H */
