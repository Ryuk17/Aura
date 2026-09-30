/* inference/MNN 单元测试：跑通第一个模型（silero VAD v5）
 *
 * 这是 Phase 1 "MNN 封装打通，跑通第一个模型" 的验收用例：
 *   - 加载 models/vad/silero_vad.mnn/silero_vad.mnn（1.5MB，int8）
 *   - 验证 I/O 张量枚举（input(1,-1) / state(2,-1,128) / sr()）
 *   - 动态形状 resize → 喂静音 → 前向 → 概率在 [0,1] → 状态张量被更新
 *
 * **必须走 AURA_INFER_FORM_MODULE**：该模型算子表里有 If 子图，Session 路径
 * 的 resizeSession 会在形状推导阶段失败（输出恒为 0 形状）。详见
 * engine_mnn.cpp 文件头；官方 sherpa-mnn 的 SileroVadModel 同样只用 Module。
 *
 * 输入口径照官方 SileroVadModel（silero-vad-model.cc）：
 *   - 顺序 input / state / sr，sr 是 int32 标量
 *   - 窗口 = 512 + 64 overlap = 576（v5 固定，见 CheckV5）
 *   - state (2,1,128) 手工喂回上一次的 stateN，递归由调用方维持
 *
 * 模型文件不入 git（models/download.sh 下载）。找不到模型时本测试跳过（返回 0），
 * 不当作失败 —— 出口验收（host_sim）不依赖本测试，但 CI 有模型时必须全绿。
 */
#include "aura_test.h"

#include <stdio.h>

#include "inference/inference.h"

static char g_vad_path[512];

static int vad_model_exists(void)
{
    if (g_vad_path[0] != '\0') {
        return 1;
    }
    /* 模型目录由编译期注入（见 tests/CMakeLists.txt） */
    const char *root =
#ifdef AURA_HOST_SIM_SOURCE_DIR
        AURA_HOST_SIM_SOURCE_DIR;
#else
        ".";
#endif
    snprintf(g_vad_path, sizeof(g_vad_path), "%s/models/vad/silero_vad.mnn/silero_vad.mnn", root);
    FILE *fp = fopen(g_vad_path, "rb");
    if (fp == NULL) {
        snprintf(g_vad_path, sizeof(g_vad_path), "%s", "");
        return 0;
    }
    fclose(fp);
    return 1;
}

static void test_engine_init(void)
{
    aura_infer_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.backend       = AURA_INFER_BACKEND_CPU;
    cfg.threads       = 2;
    cfg.precision_low = true;
    AURA_ASSERT(aura_infer_init(&cfg) == AURA_OK);
    AURA_ASSERT(aura_infer_ready());
    AURA_CHECK(strlen(aura_infer_version()) > 0);
    AURA_ASSERT(aura_infer_init(&cfg) == AURA_ERR_EXIST); /* 重复初始化拒绝 */
}

static void test_load_vad_and_run(void)
{
    if (!vad_model_exists()) {
        printf("    SKIP: VAD model not found (%s)\n", "models/vad/silero_vad.mnn");
        return;
    }
    AURA_ASSERT(aura_infer_ready());

    aura_infer_model_t *m = aura_infer_load(g_vad_path, AURA_INFER_FORM_MODULE);
    AURA_ASSERT(m != NULL);

    AURA_ASSERT_EQ(aura_model_input_count(m), 3);  /* input / state / sr */
    AURA_ASSERT_EQ(aura_model_output_count(m), 2); /* output / stateN */

    /* 输入枚举：验证名字与形状（model_requirement.md 2.5 实测口径） */
    aura_tensor_info_t info;
    AURA_ASSERT(aura_model_input_info(m, 0, &info) == AURA_OK);
    AURA_ASSERT_STREQ(info.name, "input");
    AURA_ASSERT_EQ(info.dim_count, 2);
    AURA_ASSERT_EQ(info.dims[0], 1);
    AURA_ASSERT_EQ(info.dims[1], -1); /* 声明里是动态 T 维：bytes=0，必须先 resize */
    AURA_ASSERT_EQ(info.bytes, 0);

    /* input: resize 到 576 样本（官方 v5 口径：512 窗口 + 64 overlap） */
    aura_tensor_t *in = aura_model_input(m, "input");
    AURA_ASSERT(in != NULL);
    int32_t shape[2] = {1, 576};
    AURA_ASSERT(aura_tensor_resize(in, shape, 2) == AURA_OK);

    float samples[576];
    memset(samples, 0, sizeof(samples)); /* 静音输入 */
    uint64_t nbytes = aura_tensor_bytes(in);
    AURA_ASSERT_EQ(nbytes, sizeof(samples));
    AURA_ASSERT(aura_tensor_write(in, samples, nbytes) == AURA_OK);

    /* sr: int32 标量。声明形状为空（dim_count=0，元素数按 1 算），直接可写。 */
    aura_tensor_t *sr = aura_model_input(m, "sr");
    AURA_ASSERT(sr != NULL);
    AURA_ASSERT_EQ(aura_tensor_dtype(sr), AURA_DTYPE_S32);
    AURA_ASSERT_EQ(aura_tensor_bytes(sr), sizeof(int32_t));
    AURA_ASSERT(aura_tensor_write(sr, &(int32_t){16000}, sizeof(int32_t)) == AURA_OK);

    /* state: 声明为 (2,-1,128)，batch 维动态 → 显式定形后才能写 */
    aura_tensor_t *state = aura_model_input(m, "state");
    AURA_ASSERT(state != NULL);
    AURA_ASSERT_EQ(aura_tensor_bytes(state), 0);
    int32_t st_shape[3] = {2, 1, 128};
    AURA_ASSERT(aura_tensor_resize(state, st_shape, 3) == AURA_OK);
    float zero_state[2 * 1 * 128];
    memset(zero_state, 0, sizeof(zero_state));
    AURA_ASSERT_EQ(aura_tensor_bytes(state), sizeof(zero_state));
    AURA_ASSERT(aura_tensor_write(state, zero_state, sizeof(zero_state)) == AURA_OK);

    /* 第一次前向：静音 → 概率应接近 0 且在 [0,1] 内 */
    AURA_ASSERT(aura_model_run(m) == AURA_OK);
    aura_tensor_t *out = aura_model_output(m, "output");
    AURA_ASSERT(out != NULL);
    float prob = 0.f;
    AURA_ASSERT(aura_tensor_read(out, &prob, aura_tensor_bytes(out)) == AURA_OK);
    AURA_CHECK(prob >= 0.0f && prob <= 1.0f);
    AURA_CHECK(prob < 0.5f); /* 静音不应判为语音 */

    /* stateN 应可读且形状正确 */
    aura_tensor_t *state_n = aura_model_output(m, "stateN");
    AURA_ASSERT(state_n != NULL);
    float new_state[2 * 1 * 128];
    AURA_ASSERT_EQ(aura_tensor_bytes(state_n), sizeof(new_state));
    AURA_ASSERT(aura_tensor_read(state_n, new_state, sizeof(new_state)) == AURA_OK);

    /* 第二次前向：状态应随递归变化（非全零） */
    AURA_ASSERT(aura_tensor_write(state, new_state, sizeof(new_state)) == AURA_OK);
    AURA_ASSERT(aura_model_run(m) == AURA_OK);
    AURA_ASSERT(aura_tensor_read(state_n, new_state, sizeof(new_state)) == AURA_OK);
    int nonzero = 0;
    for (int i = 0; i < 2 * 128; i++) {
        if (new_state[i] != 0.0f) {
            nonzero++;
        }
    }
    AURA_CHECK(nonzero > 0);

    /* 模型信息：权重/运行时内存账（memory_budget.md 复核依据） */
    aura_model_info_t minfo;
    AURA_ASSERT(aura_model_get_info(m, &minfo) == AURA_OK);
    AURA_CHECK(minfo.weight_bytes > 0);
    printf("    vad model: weight=%llu KB runtime=%llu KB load=%llu ms\n",
           (unsigned long long)(minfo.weight_bytes >> 10),
           (unsigned long long)(minfo.runtime_bytes >> 10),
           (unsigned long long)(minfo.load_us / 1000));

    uint64_t cur = 0, peak = 0;
    AURA_ASSERT(aura_infer_memory_stats(&cur, &peak) == AURA_OK);
    printf("    mnn memory: current=%llu KB peak=%llu KB\n", (unsigned long long)(cur >> 10),
           (unsigned long long)(peak >> 10));

    AURA_ASSERT(aura_infer_unload(m) == AURA_OK);
}

static void test_helpers(void)
{
    AURA_ASSERT_STREQ(aura_infer_form_name(AURA_INFER_FORM_SESSION), "session");
    AURA_ASSERT_STREQ(aura_dtype_name(AURA_DTYPE_F32), "f32");
    AURA_ASSERT_EQ(aura_dtype_size(AURA_DTYPE_F32), 4);
    AURA_ASSERT_EQ(aura_dtype_size(AURA_DTYPE_S8), 1);

    aura_tensor_info_t info;
    memset(&info, 0, sizeof(info));
    info.dim_count = 3;
    info.dims[0] = 2;
    info.dims[1] = 3;
    info.dims[2] = 4;
    AURA_ASSERT_EQ(aura_tensor_info_elements(&info), 24);
    info.dims[1] = -1; /* 动态维 */
    AURA_ASSERT_EQ(aura_tensor_info_elements(&info), 0);
}

static void test_engine_teardown(void)
{
    aura_infer_deinit();
    AURA_ASSERT(!aura_infer_ready());
}

AURA_TEST(test_engine_init);
AURA_TEST(test_load_vad_and_run);
AURA_TEST(test_helpers);
AURA_TEST(test_engine_teardown);

AURA_TEST_MAIN();
