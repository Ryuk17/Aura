# 模型下载清单

模型权重**不随仓库分发**，克隆后按下面命令下载。
依赖：[hfd.sh](hfd.sh)（HuggingFace 下载脚本）；推荐 `aria2c`（无则加 `--tool wget`）。

| 用途 | 本地目录 | 上游仓库（HuggingFace） | 约大小 |
|------|----------|--------------------------|--------|
| ASR | [asr/sherpa-mnn-streaming-zipformer-bilingual-zh-en-2023-02-20](asr/sherpa-mnn-streaming-zipformer-bilingual-zh-en-2023-02-20) | `taobao-mnn/sherpa-mnn-streaming-zipformer-bilingual-zh-en-2023-02-20` | 282 MB |
| Embedding | [embedding/Qwen3-Embedding-0.6B-MNN](embedding/Qwen3-Embedding-0.6B-MNN) | `taobao-mnn/Qwen3-Embedding-0.6B-MNN` | 361 MB |
| LLM | [llm/Qwen3.5-0.8B-MNN](llm/Qwen3.5-0.8B-MNN) | `taobao-mnn/Qwen3.5-0.8B-MNN` | 523 MB |
| TTS | [tts/Kokoro-82M](tts/Kokoro-82M) | `hexgrad/Kokoro-82M` | 347 MB |

## 一键下载

```bash
cd models

# 国内网络建议走镜像；直连可省略
export HF_ENDPOINT=https://alpha.hf-mirror.com

bash hfd.sh taobao-mnn/sherpa-mnn-streaming-zipformer-bilingual-zh-en-2023-02-20 \
    --tool aria2c --local-dir asr/sherpa-mnn-streaming-zipformer-bilingual-zh-en-2023-02-20
bash hfd.sh taobao-mnn/Qwen3-Embedding-0.6B-MNN \
    --tool aria2c --local-dir embedding/Qwen3-Embedding-0.6B-MNN
bash hfd.sh taobao-mnn/Qwen3.5-0.8B-MNN \
    --tool aria2c --local-dir llm/Qwen3.5-0.8B-MNN
bash hfd.sh hexgrad/Kokoro-82M \
    --tool aria2c --local-dir tts/Kokoro-82M
```

需要认证的模型加 `--hf_token <token>`（下载命令会记录在各模型的 `.hfd/` 元数据中）。
