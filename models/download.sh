apt update && apt install aria2 -y

wget https://hf-mirror.com/hfd/hfd.sh 

chmod +x hfd.sh

export HF_ENDPOINT=https://hf-mirror.com

./hfd.sh taobao-mnn/sherpa-mnn-streaming-zipformer-bilingual-zh-en-2023-02-20 --local-dir sherpa-mnn-streaming-zipformer-bilingual-zh-en-2023-02-20

./hfd.sh hexgrad/Kokoro-82M --local-dir tts/Kokoro-82M

./hfd.sh taobao-mnn/Qwen3.5-0.8B-MNN --local-dir llm/Qwen3.5-0.8B-MNN

./hfd.sh taobao-mnn/Qwen3-Embedding-0.6B-MNN --local-dir embedding/Qwen3-Embedding-0.6B-MNN

./hfd.sh Jeryuk/smart-turn-v3.2-gpu.mnn --local-dir turn/smart-turn-v3.2-gpu.mnn

./hfd.sh Jeryuk/silero_vad.mnn --local-dir vad/silero_vad.mnn

