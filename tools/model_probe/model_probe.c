/* model_probe — 模型探针（Phase 3 起：换模型/新算法接入前的第一件事）
 *
 * 干什么：把 .mnn 打开，**不打一行算法代码**，先把文件的"事实"打出来：
 *   - 能不能加载（MODULE / SESSION 各试一遍，哪个成走哪个）
 *   - 输入输出张量：名字、形状、dtype（流式模型的状态张量形状是硬编码错误的来源）
 *   - metadata 全量键值（zipformer 的超参全在这儿：T / decode_chunk_len /
 *     encoder_dims / left_context_len / vocab_size ...，缺一个就得回退硬编码）
 *   - 空跑一次前向（全零输入），确认"加载得了"不等于"跑得动"
 *
 * 为什么要有这个工具：ONNX → MNN 转换后，**元数据丢失、dtype 被改写、动态维被
 * 固化** 都可能发生，而且都是静默的 —— 直接照 sherpa 参考实现写引擎，跑出来是
 * 错的结果而不是报错。先探针后写码，把"模型实际长什么样"和"我以为它长什么样"
 * 对齐，能省掉一整轮"接上了但效果不对"的排查。
 *
 * 用法：
 *   model_probe <model.mnn> [--form module|session] [--run] [--only-meta]
 *   model_probe --all <dir>        # 把一个目录下所有 .mnn 全探一遍
 *
 * 退出码：0 = 至少一个形态加载成功；1 = 全都加载不了（脚本里可直接当断言用）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "inference/inference.h"

/* ------------------------------------------------------------------ 打印 */

#if defined(_WIN32)
#include <windows.h>
#endif

/* 把控制台切到 UTF-8：metadata 里可能有中文（模型描述），Windows 默认 GBK 会乱码。 */
static void console_utf8(void)
{
#if defined(_WIN32)
    SetConsoleOutputCP(65001);
#endif
}

static const char *base_name(const char *path)
{
    const char *s = path;
    for (const char *p = path; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') {
            s = p + 1;
        }
    }
    return s;
}

/* 形状打成 "[1, 64, 1, 256]" 这样的人读形式。 */
static void print_shape(const aura_tensor_info_t *ti, char *out, size_t cap)
{
    size_t   off = 0;
    int32_t  n   = ti->dim_count;
    if (n <= 0) {
        snprintf(out, cap, "(scalar)");
        return;
    }
    off += (size_t)snprintf(out + off, cap - off, "[");
    for (int32_t i = 0; i < n && off < cap; i++) {
        off += (size_t)snprintf(out + off, cap - off, "%s%d", (i == 0) ? "" : ", ", ti->dims[i]);
    }
    snprintf(out + off, cap - off, "]");
}

static void probe_tensors(const aura_infer_model_t *m)
{
    int32_t in_n  = aura_model_input_count(m);
    int32_t out_n = aura_model_output_count(m);
    printf("  tensors: %d in / %d out\n", in_n, out_n);

    for (int32_t i = 0; i < in_n; i++) {
        aura_tensor_info_t ti;
        if (aura_model_input_info(m, i, &ti) != AURA_OK) {
            printf("    in[%d]  <info failed>\n", i);
            continue;
        }
        char shape[128];
        print_shape(&ti, shape, sizeof(shape));
        printf("    in [%2d] %-32s %-5s %-20s %llu B\n", i, ti.name, aura_dtype_name(ti.dtype),
               shape, (unsigned long long)ti.bytes);
    }
    for (int32_t i = 0; i < out_n; i++) {
        aura_tensor_info_t ti;
        if (aura_model_output_info(m, i, &ti) != AURA_OK) {
            printf("    out[%d] <info failed>\n", i);
            continue;
        }
        char shape[128];
        print_shape(&ti, shape, sizeof(shape));
        printf("    out[%2d] %-32s %-5s %-20s %llu B\n", i, ti.name, aura_dtype_name(ti.dtype),
               shape, (unsigned long long)ti.bytes);
    }
}

static void probe_meta(const aura_infer_model_t *m)
{
    int32_t n = aura_model_meta_count(m);
    if (n == 0) {
        printf("  metadata: (none —— 形态不支持，或转换时丢失)\n");
        return;
    }
    printf("  metadata: %d keys\n", n);
    for (int32_t i = 0; i < n; i++) {
        char key[128];
        char val[512];
        if (aura_model_meta_at(m, i, key, sizeof(key), val, sizeof(val)) != AURA_OK) {
            printf("    <key %d unreadable>\n", i);
            continue;
        }
        /* zipformer 的长向量（encoder_dims 等）原样打，别截断 —— 就是要看全。 */
        printf("    %-28s = %s\n", key, val);
    }
}

/* 一次"真"前向：把每个输入都写满零再跑。零输入算出来的数是垃圾，但这条路径
 * 恰好是算法层要走的全部动作 —— resize 动态维 → write → run → read 输出。
 * 测的是"写进去的路通不通"，而不是"结果对不对"（后者要真特征，是算法测试的事）。
 *
 * 动态维（-1）按 1 兜底：流式模型的状态张量只有 batch 维是真动态，且实际恒为 1。 */
static int probe_run(aura_infer_model_t *m)
{
    int32_t in_n  = aura_model_input_count(m);
    int32_t out_n = aura_model_output_count(m);

    /* 输出里若还有未定的动态维，run 之后形状仍可能不落地 —— 但仍值得跑：
     * 跑通了说明后端 kernel 齐全，跑不通是转换/后端问题，与调用方无关。 */
    for (int32_t i = 0; i < out_n; i++) {
        aura_tensor_info_t ti;
        if (aura_model_output_info(m, i, &ti) == AURA_OK && ti.dims[0] < 0) {
            printf("  run: 注意 —— 输出 %s 含动态维，形状要跑完才定\n", ti.name);
            break;
        }
    }

    int32_t resized = 0;
    int32_t written = 0;
    for (int32_t i = 0; i < in_n; i++) {
        aura_tensor_info_t ti;
        if (aura_model_input_info(m, i, &ti) != AURA_OK) {
            printf("  run: FAILED —— 输入 %d 取不到信息\n", i);
            return -1;
        }
        int32_t dims[AURA_TENSOR_MAX_DIMS];
        bool    dyn = false;
        for (int32_t d = 0; d < ti.dim_count; d++) {
            dims[d] = ti.dims[d];
            if (dims[d] < 0) {
                dims[d] = 1;
                dyn     = true;
            }
        }
        if (dyn) {
            aura_err_t rrc = aura_tensor_resize(aura_model_input(m, ti.name), dims, ti.dim_count);
            if (rrc != AURA_OK) {
                printf("  run: FAILED —— resize 输入 %s 失败 (%s)\n", ti.name, aura_strerror(rrc));
                return -1;
            }
            resized++;
        }
        aura_tensor_t *t = aura_model_input(m, ti.name);
        uint64_t       n = aura_tensor_bytes(t);
        if (n == 0 || n > (64u << 20)) {
            printf("  run: FAILED —— 输入 %s 字节数异常 (%llu)\n", ti.name, (unsigned long long)n);
            return -1;
        }
        void *zero = calloc(1, (size_t)n);
        if (zero == NULL) {
            printf("  run: FAILED —— 分配 %llu B 失败\n", (unsigned long long)n);
            return -1;
        }
        aura_err_t wrc = aura_tensor_write(t, zero, n);
        free(zero);
        if (wrc != AURA_OK) {
            printf("  run: FAILED —— 写输入 %s 失败 (%s)\n", ti.name, aura_strerror(wrc));
            return -1;
        }
        written++;
    }
    printf("  run: 写输入 %d 个（其中 %d 个做过 resize）\n", written, resized);

    aura_err_t rc = aura_model_run(m);
    if (rc != AURA_OK) {
        printf("  run: FAILED (%s) —— 加载得了但跑不动\n", aura_strerror(rc));
        return -1;
    }
    printf("  run: ok\n");

    /* run 之后再看一眼输出：加载时的输出形状常常是动态的（打出来是 (scalar)），
     * 跑过一遍才知道实际是多少 —— 分配输出缓冲、算 subsampling 都靠它。 */
    printf("  output shapes after run:\n");
    for (int32_t i = 0; i < out_n; i++) {
        aura_tensor_info_t ti;
        if (aura_model_output_info(m, i, &ti) != AURA_OK) {
            continue;
        }
        char shape[128];
        print_shape(&ti, shape, sizeof(shape));
        printf("    out[%2d] %-32s %-5s %-20s %llu B\n", i, ti.name, aura_dtype_name(ti.dtype),
               shape, (unsigned long long)ti.bytes);
        /* 顺带验一次回读：读不出数就等于算法层拿不到结果。只读第 0 个，
         * 39 个状态全读一遍是几百 KB 的噪声，对"路通不通"没有增量信息。 */
        if (i == 0 && ti.bytes > 0 && ti.bytes < (64u << 20)) {
            void *buf = malloc((size_t)ti.bytes);
            if (buf != NULL) {
                aura_err_t rrc = aura_tensor_read(aura_model_output(m, ti.name), buf, ti.bytes);
                printf("    -> 回读 %s: %s\n", ti.name, (rrc == AURA_OK) ? "ok" : aura_strerror(rrc));
                free(buf);
            }
        }
    }
    return 0;
}

/* 探一个模型：两种形态各试。返回加载成功的形态数。 */
static int probe_one(const char *path, aura_infer_form_t forced, bool do_run, bool only_meta)
{
    printf("== %s\n", path);

    /* --form 指定了就只试那一个，否则 MODULE 优先：metadata 只在 MODULE 上，
     * 而 zipformer 一族没有 metadata 就得回退硬编码 —— 所以先看 MODULE 行不行。 */
    aura_infer_form_t forms[2];
    int               nform = 0;
    if (forced != (aura_infer_form_t)-1) {
        forms[nform++] = forced;
    } else {
        forms[nform++] = AURA_INFER_FORM_MODULE;
        forms[nform++] = AURA_INFER_FORM_SESSION;
    }

    int ok = 0;
    for (int i = 0; i < nform; i++) {
        aura_infer_model_t *m = aura_infer_load(path, forms[i]);
        if (m == NULL) {
            printf("  [%s] load FAILED\n", aura_infer_form_name(forms[i]));
            continue;
        }
        aura_model_info_t info;
        if (aura_model_get_info(m, &info) == AURA_OK) {
            printf("  [%s] load ok —— 权重 %.2f MB, 加载 %.0f ms\n", aura_infer_form_name(forms[i]),
                   (double)info.weight_bytes / (1024.0 * 1024.0), (double)info.load_us / 1000.0);
        } else {
            printf("  [%s] load ok (info unavailable)\n", aura_infer_form_name(forms[i]));
        }
        if (!only_meta) {
            probe_tensors(m);
        }
        probe_meta(m);
        if (do_run && !probe_run(m)) {
            /* probe_run 返回 0 = 跑通或跳过；-1 = 跑挂了（但它自己打了原因） */
        }
        aura_infer_unload(m);
        ok++;
        /* 默认形态探完就够 —— 探针是给人看的，两种都打会淹掉结论。 */
        if (forced == (aura_infer_form_t)-1) {
            break;
        }
    }
    printf("\n");
    return ok;
}

#if defined(_WIN32)
#include <windows.h>
/* 目录扫描用 Win32，不为一个调试工具引入 dirent 兼容层。 */
static int probe_dir(const char *dir, aura_infer_form_t forced, bool do_run, bool only_meta)
{
    char   pattern[512];
    snprintf(pattern, sizeof(pattern), "%s\\*.mnn", dir);
    WIN32_FIND_DATAA fd;
    HANDLE           h = FindFirstFileA(pattern, &fd);
    if (h == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "model_probe: 目录里没有 .mnn: %s\n", dir);
        return 0;
    }
    int total = 0;
    do {
        char path[768];
        snprintf(path, sizeof(path), "%s\\%s", dir, fd.cFileName);
        total += probe_one(path, forced, do_run, only_meta);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return total;
}
#else
#include <dirent.h>
static int probe_dir(const char *dir, aura_infer_form_t forced, bool do_run, bool only_meta)
{
    DIR *d = opendir(dir);
    if (d == NULL) {
        fprintf(stderr, "model_probe: 打不开目录 %s\n", dir);
        return 0;
    }
    int            total = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t len = strlen(e->d_name);
        if (len < 5 || strcmp(e->d_name + len - 4, ".mnn") != 0) {
            continue;
        }
        char path[768];
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        total += probe_one(path, forced, do_run, only_meta);
    }
    closedir(d);
    return total;
}
#endif

static void usage(const char *argv0)
{
    printf("用法:\n"
           "  %s <model.mnn> [--form module|session] [--run] [--only-meta]\n"
           "  %s --all <dir>  [--form module|session] [--run] [--only-meta]\n"
           "\n"
           "  --run        加载后空跑一次前向（默认只加载 + 打印）\n"
           "  --only-meta  不打印张量清单\n"
           "  --form X     只试指定形态（默认 MODULE 不行再试 SESSION）\n",
           argv0, argv0);
}

int main(int argc, char **argv)
{
    console_utf8();

    const char       *path  = NULL;
    const char       *dir   = NULL;
    bool              do_run = false;
    bool              only_meta = false;
    aura_infer_form_t forced = (aura_infer_form_t)-1;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "--run") == 0) {
            do_run = true;
        } else if (strcmp(a, "--only-meta") == 0) {
            only_meta = true;
        } else if (strcmp(a, "--all") == 0 && i + 1 < argc) {
            dir = argv[++i];
        } else if (strcmp(a, "--form") == 0 && i + 1 < argc) {
            const char *f = argv[++i];
            if (strcmp(f, "module") == 0) {
                forced = AURA_INFER_FORM_MODULE;
            } else if (strcmp(f, "session") == 0) {
                forced = AURA_INFER_FORM_SESSION;
            } else {
                fprintf(stderr, "model_probe: 未知形态 '%s'\n", f);
                return 2;
            }
        } else if (a[0] == '-') {
            fprintf(stderr, "model_probe: 未知参数 '%s'\n", a);
            usage(argv[0]);
            return 2;
        } else {
            path = a;
        }
    }

    if (path == NULL && dir == NULL) {
        usage(argv[0]);
        return 2;
    }

    aura_infer_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.backend = AURA_INFER_BACKEND_CPU;
    cfg.threads = 2; /* 探针不需要跑道数；板端核少时线程开满反而更慢 */
    aura_err_t rc = aura_infer_init(&cfg);
    if (rc != AURA_OK) {
        fprintf(stderr, "model_probe: 引擎初始化失败: %s\n", aura_strerror(rc));
        return 1;
    }
    printf("MNN 引擎: %s\n\n", aura_infer_version());

    int ok = (dir != NULL) ? probe_dir(dir, forced, do_run, only_meta)
                           : probe_one(path, forced, do_run, only_meta);

    aura_infer_deinit();
    printf("%s\n", (ok > 0) ? "探针结论: 至少一个形态可用" : "探针结论: 全部形态都加载不了");
    return (ok > 0) ? 0 : 1;
}
