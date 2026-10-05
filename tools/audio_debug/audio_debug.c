/* Aura — audio_debug：Phase 2 语音前端的文件化验证工具
 *
 * 用法：
 *   audio_debug --gen-scene <prefix>                     造合成回声场景后退出
 *   audio_debug --in <mic.wav> [--ref <ref.wav>] \
 *               --config <conf> --out-dir <dir> [--check] [--max-frames N]
 *
 * 干什么：按 conf 里的 chain 装配**真链**（aec3,bf,ns,agc,src），在每个产音频
 * 的节点后面插一个 wavtap 观察者，把该节点的输出原样落成 WAV；链尾再挂一个
 * 终端 tap 收整条链的最终输出。跑完打印各节点统计，--check 时对"形状类事实"
 * 做轻断言（深度指标在 tests/audio_front 里断言）。
 *
 * 为什么直接操作 pipeline 而不复用 host_sim 的 agent 驱动：host_sim 是 Phase 1
 * 的骨架仿真（链上全是 mock 节点），它的语义要冻结；这里要的是"真算法 + 能寻址
 * 到每一个中间节点"，两者目标不同，硬凑在一起两边都会被拖住。
 *
 * 麦克风接不上时的两条路径：
 *   合成场景 —— --gen-scene 造一段回声路径已知的信号，能验证"有参考时确实在消"；
 *   真实录音 —— assets/audio/microphone_array 的 CH0/CH1（无播放回采），
 *               AEC 只能走"缺参考 → 静音顶替"的降级路径，验证的是不崩、其余
 *               节点照常工作。
 */
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#endif
#include <sys/stat.h>

#include "core/algorithm.h"
#include "core/config/config.h"
#include "core/pipeline/pipeline.h"
#include "dsp/aura_dsp_adapter.h"
#include "front_probe.h"
#include "metrics.h"
#include "synth_scene.h"
#include "utils/logger/logger.h"
#include "wav_io.h"
#include "wavtap.h"

#define TAG "audio_debug"

typedef struct opts {
    const char *in_path;
    const char *ref_path;
    const char *config_path;
    const char *out_dir;
    const char *gen_scene; /* 非 NULL = 只造场景，然后退出 */
    uint32_t    max_frames;
    bool        check;
    bool        profile; /* 打印帧级能量剖面（排查"哪一段被吃了"用） */
} opts_t;

static void usage(const char *argv0)
{
    printf("用法:\n");
    printf("  %s --gen-scene <prefix>\n", argv0);
    printf("  %s --in <mic.wav> [--ref <ref.wav>] --config <conf> --out-dir <dir>\n", argv0);
    printf("     [--check] [--max-frames N]\n\n");
    printf("  --gen-scene  造合成回声场景（<prefix>_mic.wav / <prefix>_ref.wav）后退出\n");
    printf("  --ref        播放回采；缺省 = 无参考（真实录音场景，AEC 走静音顶替）\n");
    printf("  --check      对形状/健康度做轻断言，失败返回非 0\n");
}

/* 帧级剖面的开关。放全局是因为它由 main 解析、在 report_synth_metrics 里用，
 * 而那条调用链上再插一个参数只会让签名变长而不增加信息。 */
static bool g_profile = false;

/* 建目录的实现挪到 wav_io.c（wav_ensure_dir）：CTest 那边也要自己准备落盘目录，
 * 两边各写一份的话，Windows/Linux 的差异迟早只在其中一边被修。 */

static void dump_dsp_stats(aura_node_t *n)
{
    aura_dsp_stats_t s;
    if (aura_dsp_get_stats(n, &s) != AURA_OK) {
        return;
    }
    printf("  %-10s 帧 in=%llu out=%llu  样本 in=%llu  错误=%llu  参考缺失=%llu", n->name,
           (unsigned long long)s.frames_in, (unsigned long long)s.frames_out,
           (unsigned long long)s.samples_in, (unsigned long long)s.process_errors,
           (unsigned long long)s.ref_missed);
    if (s.ref_lead_max || s.ref_stale) {
        printf("  参考领先峰值=%llu 样本  参考丢弃=%llu 样本", (unsigned long long)s.ref_lead_max,
               (unsigned long long)s.ref_stale);
    }
    printf("  标志 ↑%llu ↓%llu\n", (unsigned long long)s.flag_rise, (unsigned long long)s.flag_fall);
}

static void dump_tap_stats(aura_node_t *n, bool with_wav)
{
    wavtap_stats_t t;
    if (wavtap_get_stats(n, &t) != AURA_OK) {
        return;
    }
    printf("  %-10s 帧 in=%llu  样本=%llu", n->name, (unsigned long long)t.frames_in,
           (unsigned long long)t.samples_total);
    if (t.frames_in > 0) {
        printf("  %uch/%uHz→(末帧) %uch/%uHz", t.channels, t.rate, t.last_channels, t.last_rate);
    }
    printf("  峰值=%d 削波=%llu", t.peak, (unsigned long long)t.clipped);
    if (with_wav) {
        printf("  落盘=%llu 帧", (unsigned long long)t.frames_written);
        if (t.write_errors > 0) {
            printf(" (写失败 %llu)", (unsigned long long)t.write_errors);
        }
    }
    printf("\n");
}

/* 一个探针的统计快照。放文件作用域是因为它很大（每个 wavtap_stats_t 都带
 * 4096 个 double 的逐帧能量，16 个就是 500 多 KB），放栈上跑在 Windows 默认
 * 1MB 主线程栈上只剩一点点余量。 */
typedef struct tap_entry {
    const char    *name;
    wavtap_stats_t st;
} tap_entry_t;

static void collect_taps(aura_node_t **nodes, uint32_t count, const aura_probe_tap_t *taps,
                         uint32_t ntap, tap_entry_t *t, uint32_t *out_n)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < ntap && n < AURA_PROBE_MAX_TAPS; i++) {
        aura_node_t *node = aura_probe_find(nodes, count, taps[i].tap);
        if (node != NULL && wavtap_get_stats(node, &t[n].st) == AURA_OK) {
            t[n].name = taps[i].tap;
            n++;
        }
    }
    *out_n = n;
}

/* 帧级能量剖面。**对任何输入都成立**（不像下面那些合成场景窗口），所以真实录音、
 * 甚至 TrickRoom 自带的测试数据都能直接喂进来比 —— 排查"回声到底消掉没有"时，
 * 最有力的对照就是拿引擎自己的数据跑一遍。 */
static void report_profile(const wav_data_t *mic, const tap_entry_t *t, uint32_t n, uint32_t fc)
{
    if (!g_profile || fc == 0 || mic->frames < fc) {
        return;
    }
    const uint32_t   BUCKET  = 25; /* 25 帧 = 250 ms @10ms 帧 */
    const uint32_t   nframes = mic->frames / fc;
    const tap_entry_t *pick[8];
    const char        *label[8];
    uint32_t           npick = 0;
    /* 按探针登记顺序取，但把 SRC 排到最后并标注时间轴不同：它的帧号仍是"第几帧"，
     * 窗口取值依旧成立，只是每帧代表 20ms 而不是 10ms。 */
    static const char *prefer[] = { "aec3_out", "bf_out", "ns_out", "agc_out", "out", "src_out" };
    for (uint32_t k = 0; k < sizeof(prefer) / sizeof(prefer[0]) && npick < 8; k++) {
        for (uint32_t i = 0; i < n; i++) {
            if (strcmp(t[i].name, prefer[k]) == 0) {
                pick[npick]  = &t[i];
                label[npick] = prefer[k];
                npick++;
                break;
            }
        }
    }
    if (npick == 0) {
        return;
    }
    printf("\n== 能量剖面（每格 %u ms，dB 相对同窗口的近端输入；-- = 无数据）==\n",
           BUCKET * 10);
    printf("  格起(s) ");
    for (uint32_t c = 0; c < npick; c++) {
        printf("%9s", label[c]);
    }
    printf("\n");
    for (uint32_t f = 0; f + BUCKET <= nframes; f += BUCKET) {
        const double e_in = aura_metrics_energy(mic->samples, mic->frames, mic->channels, f * fc,
                                                (f + BUCKET) * fc);
        printf("  %6.2f  ", (double)f * 10.0 / 1000.0);
        for (uint32_t c = 0; c < npick; c++) {
            if (pick[c]->st.energy_frames < f + BUCKET) {
                printf("%9s", "--");
                continue;
            }
            printf("%9.1f", aura_metrics_db(aura_metrics_frames_mean(pick[c]->st.frame_energy,
                                                                     pick[c]->st.energy_frames, f,
                                                                     f + BUCKET),
                                            e_in));
        }
        printf("\n");
    }
}

/* 合成场景的客观指标。**只在输入确实是合成场景形状时才打印** ——
 * 真实录音上这些窗口没有对应含义，打印出来只会误导。
 * 这里只报数，不下结论：阈值断言在 tests/audio_front 里（那里有失败重跑与
 * 落盘对比）。工具给人看数，测试给机器判对错。 */
static void report_synth_metrics(const wav_data_t *mic, const tap_entry_t *t, uint32_t n)
{
    /* 形状对不上就安静跳过，不用报错（--in 本来就可以喂别的）。 */
    if (mic->channels != SYNTH_CHANNELS || mic->sample_rate != SYNTH_RATE ||
        mic->frames != SYNTH_SAMPLES) {
        return;
    }
    const uint32_t fc = SYNTH_RATE * SYNTH_FRAME_MS / 1000u; /* 每帧每通道样本数 */

    /* 两个窗口各自的输入能量（近端 2 路）。tap 的逐帧能量与输入帧一一对应
     * （aec3/bf/ns/agc 都是 16k、定长帧，一进一出），所以窗口下标可以直接对。
     * SRC 的输出在另一条时间轴上（20ms 一帧），**不参与**这两个窗口。 */
    const double e_mic_echo = aura_metrics_energy(mic->samples, mic->frames, mic->channels,
                                                  SYNTH_FRAME_ERLE_0 * fc, SYNTH_FRAME_ERLE_1 * fc);
    const double e_mic_near = aura_metrics_energy(mic->samples, mic->frames, mic->channels,
                                                  SYNTH_FRAME_NEARFID_0 * fc,
                                                  SYNTH_FRAME_NEARFID_1 * fc);

    const wavtap_stats_t *st_aec = NULL, *st_bf = NULL, *st_ns = NULL, *st_agc = NULL,
                         *st_src = NULL;
    for (uint32_t i = 0; i < n; i++) {
        if (strcmp(t[i].name, "aec3_out") == 0) {
            st_aec = &t[i].st;
        } else if (strcmp(t[i].name, "bf_out") == 0) {
            st_bf = &t[i].st;
        } else if (strcmp(t[i].name, "ns_out") == 0) {
            st_ns = &t[i].st;
        } else if (strcmp(t[i].name, "agc_out") == 0) {
            st_agc = &t[i].st;
        } else if (strcmp(t[i].name, "src_out") == 0) {
            st_src = &t[i].st;
        }
    }

    printf("\n== 合成场景指标（帧号 = 时间×100，段定义见 synth_scene.h）==\n");
    if (st_aec != NULL) {
        const double e_echo = aura_metrics_frames_mean(st_aec->frame_energy, st_aec->energy_frames,
                                                       SYNTH_FRAME_ERLE_0, SYNTH_FRAME_ERLE_1);
        const double e_near = aura_metrics_frames_mean(st_aec->frame_energy, st_aec->energy_frames,
                                                       SYNTH_FRAME_NEARFID_0,
                                                       SYNTH_FRAME_NEARFID_1);
        printf("  ERLE        纯回声段 [%u,%u)  = %6.1f dB（越高越好，合成线性回声应 ≥ 10）\n",
               SYNTH_FRAME_ERLE_0, SYNTH_FRAME_ERLE_1, aura_metrics_db(e_mic_echo, e_echo));
        printf("  近端保真    AEC 输出/输入 [%u,%u) = %6.1f dB（≈0 = 没有回声时近端没被吃掉）\n",
               SYNTH_FRAME_NEARFID_0, SYNTH_FRAME_NEARFID_1,
               aura_metrics_db(e_near, e_mic_near));
    }
    if (st_bf != NULL) {
        const double e_bf_near = aura_metrics_frames_mean(st_bf->frame_energy, st_bf->energy_frames,
                                                          SYNTH_FRAME_NEARFID_0,
                                                          SYNTH_FRAME_NEARFID_1);
        printf("  BF 保形     近端段能量比 = %.3f（1.0 = 与输入单通道等能量，只判有没有崩）\n",
               aura_metrics_ratio(e_bf_near, e_mic_near));
    }
    if (st_ns != NULL && st_agc != NULL) {
        const double e_ns  = aura_metrics_frames_mean(st_ns->frame_energy, st_ns->energy_frames, 0,
                                                      st_ns->energy_frames);
        const double e_agc = aura_metrics_frames_mean(st_agc->frame_energy, st_agc->energy_frames,
                                                      0, st_agc->energy_frames);
        printf("  AGC         峰值=%d 削波=%llu ｜ AGC/NS 能量比 = %.3f\n", st_agc->peak,
               (unsigned long long)st_agc->clipped, aura_metrics_ratio(e_agc, e_ns));
    }
    if (st_src != NULL && st_agc != NULL && st_agc->samples_total > 0) {
        printf("  SRC         末帧 %uch/%uHz，样本数比 = %.4f（16k→8k 应 ≈0.5）\n",
               st_src->last_channels, st_src->last_rate,
               aura_metrics_ratio((double)st_src->samples_total,
                                  (double)st_agc->samples_total));
    }

    /* 分段增益表。上面那两个汇总量只回答"整段下来是多少"，回答不了
     * "这个损失是全程均匀的，还是只在某一类段里发生的" —— 而后者才是排查的
     * 分岔口：全程掉电平是增益结构问题，只在双讲段掉是被当成回声压掉了。
     * 数值 = 10·log10(该 tap 的段能量 / 近端输入的段能量)，0 dB = 没动。 */
    static const struct {
        const char *name;
        uint32_t    a, b;
    } SEGS[] = {
        { "近端单讲", SYNTH_FRAME_SPEECH_A_0, SYNTH_FRAME_SPEECH_A_1 },
        { "静音    ", SYNTH_FRAME_SILENCE_0, SYNTH_FRAME_SILENCE_1 },
        { "纯回声  ", SYNTH_FRAME_ECHO_0, SYNTH_FRAME_ECHO_1 },
        { "双讲    ", SYNTH_FRAME_DT_0, SYNTH_FRAME_DT_SOLO_0 },
        { "近端尾  ", SYNTH_FRAME_DT_SOLO_0, SYNTH_FRAME_TOTAL },
    };
    /* 注意这里**没有 src_out**：SRC 跑在 8k 的另一条时间轴上，帧号对不上，
     * 拿它的帧能量按 16k 的窗口取只会得到一句无意义的数。 */
    const struct {
        const char          *label;
        const wavtap_stats_t *st;
    } cols[] = {
        { "aec3_out", st_aec }, { "bf_out", st_bf }, { "ns_out", st_ns }, { "agc_out", st_agc },
    };
    printf("\n== 分段增益（dB，相对近端输入；0 = 没动，负 = 被压掉）==\n");
    printf("  段        窗口        ");
    for (uint32_t c = 0; c < 4; c++) {
        printf("%9s", cols[c].label);
    }
    printf("\n");
    for (uint32_t s = 0; s < sizeof(SEGS) / sizeof(SEGS[0]); s++) {
        const double e_in = aura_metrics_energy(mic->samples, mic->frames, mic->channels,
                                                SEGS[s].a * fc, SEGS[s].b * fc);
        printf("  %s [%3u,%3u) ", SEGS[s].name, SEGS[s].a, SEGS[s].b);
        for (uint32_t c = 0; c < 4; c++) {
            if (cols[c].st == NULL) {
                printf("%9s", "-");
                continue;
            }
            const double e = aura_metrics_frames_mean(cols[c].st->frame_energy,
                                                      cols[c].st->energy_frames, SEGS[s].a,
                                                      SEGS[s].b);
            printf("%9.1f", aura_metrics_db(e, e_in));
        }
        printf("\n");
    }
}

static int run(opts_t *o)
{
    /* ---- 注册：链上的算法必须先于配置解析进注册表，否则 chain 里的名字
     * 会在装配期报"未注册"，而真正的原因是忘了调注册。---- */
    aura_err_t rc = aura_dsp_trickroom_register();
    if (rc != AURA_OK) {
        fprintf(stderr, "TrickRoom 注册失败: %s\n", aura_strerror(rc));
        return 2;
    }
    rc = wavtap_register();
    if (rc != AURA_OK) {
        fprintf(stderr, "wavtap 注册失败: %s\n", aura_strerror(rc));
        return 2;
    }
    wavtap_set_out_dir(o->out_dir);

    /* ---- 配置 ---- */
    aura_config_t cfg;
    aura_config_default(&cfg);
    if (o->config_path != NULL) {
        rc = aura_config_load_file(&cfg, o->config_path);
        if (rc != AURA_OK) {
            fprintf(stderr, "读配置 %s 失败: %s\n", o->config_path, aura_strerror(rc));
            return 2;
        }
    }
    if (cfg.channels == 0 || cfg.sample_rate == 0 || cfg.frame_ms != 10) {
        fprintf(stderr, "配置不合用：sample_rate=%u frame_ms=%u channels=%u"
                        "（TrickRoom 全族要求 frame_ms==10）\n",
                cfg.sample_rate, cfg.frame_ms, cfg.channels);
        return 2;
    }

    aura_chain_t chain;
    aura_chain_init(&chain);
    rc = aura_chain_from_config(&cfg, &chain);
    if (rc != AURA_OK) {
        fprintf(stderr, "链装配（配置解析）失败: %s\n", aura_strerror(rc));
        return 2;
    }
    if (chain.count == 0) {
        fprintf(stderr, "配置里没有 chain = ... 一行，无事可做\n");
        return 2;
    }
    printf("chain: %s（%u 个链元素）\n", cfg.chain, chain.count);

    aura_probe_tap_t taps[AURA_PROBE_MAX_TAPS];
    aura_chain_t     probe;
    uint32_t         ntap = 0;
    rc = aura_probe_chain(&chain, &probe, taps, &ntap);
    if (rc != AURA_OK) {
        fprintf(stderr, "探针链接线失败: %s\n", aura_strerror(rc));
        return 2;
    }

    /* ---- 输入 ---- */
    wav_data_t mic;
    rc = wav_read_ex(o->in_path, &mic);
    if (rc != AURA_OK) {
        fprintf(stderr, "读 %s 失败: %s\n", o->in_path, aura_strerror(rc));
        return 2;
    }
    if (mic.channels != cfg.channels) {
        fprintf(stderr, "输入 %u 路，而配置声明 %u 路 —— 链上的通道数由配置定，"
                        "不匹配就没法比\n",
                mic.channels, cfg.channels);
        wav_free(&mic);
        return 2;
    }
    if (mic.sample_rate != cfg.sample_rate) {
        fprintf(stderr, "警告：输入 %u Hz，配置 %u Hz —— 工具不做重采样，"
                        "指标会按错误的时间轴解释\n",
                mic.sample_rate, cfg.sample_rate);
    }

    wav_data_t ref;
    bool       have_ref = false;
    if (o->ref_path != NULL) {
        rc = wav_read_ex(o->ref_path, &ref);
        if (rc != AURA_OK) {
            fprintf(stderr, "读参考 %s 失败: %s\n", o->ref_path, aura_strerror(rc));
            wav_free(&mic);
            return 2;
        }
        /* 参考流的通道数必须与近端一致：TrickRoom 的 AEC 按 capture 通道数
         * 交错读取 far 缓冲（audio_engine_aec.cpp 的 stream_config_），
         * 喂错通道数不会报错，只会把延迟估计带偏。 */
        if (ref.channels != mic.channels) {
            fprintf(stderr, "参考 %u 路、近端 %u 路：AEC 要求两者通道数一致\n", ref.channels,
                    mic.channels);
            wav_free(&mic);
            wav_free(&ref);
            return 2;
        }
        have_ref = true;
    }

    /* ---- pipeline ---- */
    aura_pipeline_cfg_t pc;
    aura_pipeline_config_default(&pc);
    pc.sample_rate = cfg.sample_rate;
    pc.frame_ms    = cfg.frame_ms;
    pc.channels    = cfg.channels;
    /* 扇出成倍消耗帧池：探针链上同一帧同时在途"链长 × 扇出"份，
     * 默认 16 块会以"帧池耗尽"的形式丢帧（表现为 frames_dropped，不是报错），
     * 而丢帧会让所有指标悄悄偏掉。32 是这条链实测不会丢的值。 */
    pc.frame_pool_blocks = 32;

    aura_pipeline_t *p = aura_pipeline_create(&pc);
    if (p == NULL) {
        fprintf(stderr, "pipeline 创建失败\n");
        wav_free(&mic);
        if (have_ref) {
            wav_free(&ref);
        }
        return 2;
    }

    aura_node_t *nodes[AURA_CHAIN_MAX_LINKS];
    uint32_t     ncount = 0;
    rc = aura_chain_build(p, &probe, nodes, &ncount);
    if (rc != AURA_OK) {
        fprintf(stderr, "链构建失败: %s\n", aura_strerror(rc));
        aura_pipeline_destroy(p);
        wav_free(&mic);
        if (have_ref) {
            wav_free(&ref);
        }
        return 2;
    }

    rc = aura_pipeline_start(p);
    if (rc != AURA_OK) {
        fprintf(stderr, "pipeline 启动失败: %s\n", aura_strerror(rc));
        aura_pipeline_stop(p);
        aura_pipeline_destroy(p);
        aura_chain_destroy_nodes(nodes, ncount);
        wav_free(&mic);
        if (have_ref) {
            wav_free(&ref);
        }
        return 2;
    }

    /* ---- 喂数据 ---- */
    const uint32_t fc    = cfg.sample_rate * cfg.frame_ms / 1000u; /* 每帧每通道样本数 */
    uint32_t       total = mic.frames / fc;
    if (have_ref) {
        const uint32_t ref_frames = ref.frames / fc;
        if (ref_frames < total) {
            total = ref_frames;
        }
    }
    if (o->max_frames > 0 && o->max_frames < total) {
        total = o->max_frames;
    }
    printf("喂入 %u 帧（%.2f s，%u 路，参考%s）\n", total, (double)total * cfg.frame_ms / 1000.0,
           mic.channels, have_ref ? "有" : "无（AEC 走静音顶替）");

    /* 喂帧节奏（尤其"参考领先近端一帧"）在 front_probe.c 里，与 tests/audio_front
     * 共用同一份 —— 差一帧不报错，只会让 ERLE 静默归零。 */
    aura_probe_input_t in;
    memset(&in, 0, sizeof(in));
    in.mic          = mic.samples;
    in.mic_channels = mic.channels;
    in.ref          = have_ref ? ref.samples : NULL;
    in.ref_channels = have_ref ? ref.channels : 0u;
    in.frames       = total;
    in.frame_count  = fc;
    in.frame_ms     = cfg.frame_ms;
    rc              = aura_probe_feed(p, &in);
    if (rc != AURA_OK) {
        fprintf(stderr, "喂数据失败: %s\n", aura_strerror(rc));
    }

    aura_pipeline_wait_drained(p, 10000);
    /* stop → 各节点 deinit → wavtap 回填 WAV 头。必须先 stop 再统计落盘数据。 */
    aura_pipeline_stop(p);

    /* ---- 汇总 ---- */
    aura_pipeline_stats_t ps;
    aura_pipeline_stats(p, &ps);
    printf("\n== pipeline ==\n");
    printf("  帧 in=%llu out=%llu dropped=%llu node_errors=%llu | 参考 in=%llu dropped=%llu\n",
           (unsigned long long)ps.frames_in, (unsigned long long)ps.frames_out,
           (unsigned long long)ps.frames_dropped, (unsigned long long)ps.node_errors,
           (unsigned long long)ps.ref_in, (unsigned long long)ps.ref_dropped);

    printf("\n== 节点 ==\n");
    for (uint32_t i = 0; i < ncount; i++) {
        if (nodes[i]->caps.produces_audio) {
            dump_dsp_stats(nodes[i]);
        }
    }
    printf("\n== 落盘 ==\n");
    for (uint32_t i = 0; i < ncount; i++) {
        if (!nodes[i]->caps.produces_audio) {
            dump_tap_stats(nodes[i], o->out_dir != NULL);
        }
    }

    static tap_entry_t tap_st[AURA_PROBE_MAX_TAPS]; /* 500KB 级，别放栈上 */
    uint32_t           ntap_st = 0;
    collect_taps(nodes, ncount, taps, ntap, tap_st, &ntap_st);
    report_synth_metrics(&mic, tap_st, ntap_st);
    report_profile(&mic, tap_st, ntap_st, fc);

    /* ---- 轻断言 ---- */
    int fails = 0;
    if (o->check) {
        printf("\n== 检查 ==\n");
        aura_node_t *out = aura_probe_find(nodes, ncount, "out");
        wavtap_stats_t out_st;
        const bool have_out = (out != NULL) && (wavtap_get_stats(out, &out_st) == AURA_OK);

#define CK(cond, ...)                                    \
    do {                                                 \
        const bool c_ = (cond);                          \
        printf("%s", c_ ? "  [ok]   " : "  [FAIL] ");    \
        printf(__VA_ARGS__);                             \
        printf("\n");                                    \
        if (!c_) {                                       \
            fails++;                                     \
        }                                                \
    } while (0)

        CK(ps.node_errors == 0, "无节点错误（node_errors=%llu）",
           (unsigned long long)ps.node_errors);
        /* 丢帧会让所有指标悄悄偏掉，它必须是一条硬失败而不是警告。 */
        CK(ps.frames_dropped == 0, "无帧丢弃（frames_dropped=%llu）",
           (unsigned long long)ps.frames_dropped);
        CK(have_out && out_st.frames_in == total, "终端 out 收到全部 %u 帧（实际 %llu）", total,
           have_out ? (unsigned long long)out_st.frames_in : 0ull);
        if (o->out_dir != NULL) {
            CK(have_out && out_st.frames_written == out_st.frames_in, "out.wav 每帧都落了盘");
        }

        for (uint32_t i = 0; i < ntap; i++) {
            aura_node_t   *node = aura_probe_find(nodes, ncount,taps[i].tap);
            wavtap_stats_t st;
            if (node == NULL || wavtap_get_stats(node, &st) != AURA_OK) {
                CK(false, "%s 存在且是 wavtap", taps[i].tap);
                continue;
            }
            CK(st.frames_in > 0, "%s 有帧经过（%llu）", taps[i].tap,
               (unsigned long long)st.frames_in);
            aura_dsp_stats_t ds;
            aura_node_t     *prod = aura_probe_find(nodes, ncount,taps[i].node);
            if (prod != NULL && aura_dsp_get_stats(prod, &ds) == AURA_OK) {
                CK(ds.process_errors == 0, "%s 无处理错误（%llu）", taps[i].node,
                   (unsigned long long)ds.process_errors);
            }
            /* 形状类事实：算法声明它会变，就真的得变 —— 这类错误（比如 SRC 的
             * out_sample_rate 没生效）不会报错，只会让下游拿到错误时基的数据。 */
            if (taps[i].declared_out_rate != 0) {
                CK(st.last_rate == taps[i].declared_out_rate,
                   "%s 末帧采样率 = 声明的 %u（实际 %u）", taps[i].algo,
                   taps[i].declared_out_rate, st.last_rate);
            }
            if (taps[i].changes_channels) {
                CK(st.last_channels == 1, "%s 把通道数收到了 1（实际 %u）", taps[i].algo,
                   st.last_channels);
            }
        }
        if (have_ref) {
            /* 按能力找 AEC，而不是按"链上第一个" —— 链序是配置定的，
             * 拿位置当身份会在改配置时静默指向别的节点。 */
            aura_node_t *aec = NULL;
            for (uint32_t i = 0; i < ncount; i++) {
                if (nodes[i]->caps.consumes_ref_audio) {
                    aec = nodes[i];
                    break;
                }
            }
            aura_dsp_stats_t ds;
            if (aec != NULL && aura_dsp_get_stats(aec, &ds) == AURA_OK) {
                CK(ds.ref_missed == 0, "AEC 参考全程不缺（ref_missed=%llu）",
                   (unsigned long long)ds.ref_missed);
            }
        }
#undef CK
        printf("  → %s\n", (fails == 0) ? "通过" : "有失败项");
    }

    /* ---- 收尾（顺序：先停/销 pipeline，再释放它建出来的节点）---- */
    aura_pipeline_destroy(p);
    aura_chain_destroy_nodes(nodes, ncount);
    wav_free(&mic);
    if (have_ref) {
        wav_free(&ref);
    }
    return (fails == 0) ? 0 : 1;
}

/* 把 path 里最后一级目录建出来（--gen-scene 的 prefix 可能带目录，CTest 就带着）。
 * 只建一级，与 --out-dir 同一口径：工具不去猜调用方想要多深的目录树。 */
static void ensure_parent_dir(const char *path)
{
    const char *slash  = strrchr(path, '/');
    const char *bslash = strrchr(path, '\\');
    if (bslash != NULL && (slash == NULL || bslash > slash)) {
        slash = bslash;
    }
    if (slash == NULL || slash == path) {
        return; /* 不带目录，或就在根下 */
    }
    char         dir[512];
    const size_t n = (size_t)(slash - path);
    if (n >= sizeof(dir)) {
        return;
    }
    memcpy(dir, path, n);
    dir[n] = '\0';
    (void)wav_ensure_dir(dir);
}

int main(int argc, char **argv)
{
    opts_t o;
    memset(&o, 0, sizeof(o));

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--gen-scene") == 0 && i + 1 < argc) {
            o.gen_scene = argv[++i];
        } else if (strcmp(a, "--in") == 0 && i + 1 < argc) {
            o.in_path = argv[++i];
        } else if (strcmp(a, "--ref") == 0 && i + 1 < argc) {
            o.ref_path = argv[++i];
        } else if (strcmp(a, "--config") == 0 && i + 1 < argc) {
            o.config_path = argv[++i];
        } else if (strcmp(a, "--out-dir") == 0 && i + 1 < argc) {
            o.out_dir = argv[++i];
        } else if (strcmp(a, "--max-frames") == 0 && i + 1 < argc) {
            o.max_frames = (uint32_t)strtoul(argv[++i], NULL, 0);
        } else if (strcmp(a, "--check") == 0) {
            o.check = true;
        } else if (strcmp(a, "--profile") == 0) {
            o.profile = true;
        } else if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "未知参数: %s\n", a);
            usage(argv[0]);
            return 2;
        }
    }

    aura_log_init();
    aura_log_set_level(AURA_LOG_LVL_WARN); /* 工具的输出以 printf 为准，日志只留警告以上 */
    g_profile = o.profile;

    if (o.gen_scene != NULL) {
        ensure_parent_dir(o.gen_scene);
        synth_scene_t sc;
        aura_err_t    rc = synth_scene_build(&sc, 20261003u);
        if (rc == AURA_OK) {
            rc = synth_scene_write(o.gen_scene, &sc);
        }
        if (rc != AURA_OK) {
            fprintf(stderr, "造场景失败: %s\n", aura_strerror(rc));
            synth_scene_free(&sc);
            return 2;
        }
        printf("已生成 %s_mic.wav / %s_ref.wav（%.1f s @%u Hz，%u 路）\n", o.gen_scene, o.gen_scene,
               (double)sc.frames / (double)sc.sample_rate, sc.sample_rate, sc.channels);
        synth_scene_free(&sc);
        return 0;
    }

    if (o.in_path == NULL || o.config_path == NULL) {
        usage(argv[0]);
        return 2;
    }
    if (o.out_dir != NULL && wav_ensure_dir(o.out_dir) != AURA_OK) {
        fprintf(stderr, "创建输出目录 %s 失败\n", o.out_dir);
        return 2;
    }
    return run(&o);
}
