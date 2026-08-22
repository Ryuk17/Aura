D1: ASR 模型
使用https://github.com/alibaba/MNN/tree/master/apps/frameworks/sherpa-mnn，模型路径https://huggingface.co/taobao-mnn/sherpa-mnn-streaming-zipformer-bilingual-zh-en-2023-02-20;

D2:TTS 模型
https://github.com/wangzhaode/mnn-tts

D3:VAD模型
参考https://github.com/azri57806-design/YinyueMobile/blob/236b75d0be44c00d047ef5364ad761b6b5c65d6e/MNN/apps/frameworks/sherpa-mnn/sherpa-mnn/csrc/silero-vad-model.h

D4: NLU模型
直接不要NLU模型


D5: 回声消除
使用https://github.com/Ryuk17/TrickRoom/tree/master/src/audio_engine/audio_processing/acoustic_echo_cancellation_mobile

D6：事件总线
为何依赖boost

D7: 知识库
先用md文档形式

D8:
不用考虑量化，MNN模型已有，https://huggingface.co/taobao-mnn/Qwen3.5-0.8B-MNN

D9：记忆持久化
先用md文档形式，后续可能用embedding模型抽取


其他
1）需要RAG就需要加入emebdding模型,https://huggingface.co/taobao-mnn/Qwen3-Embedding-0.6B-MNN
2）引入轮次检测，可以配置使用轮次检测还是vad，https://huggingface.co/pipecat-ai/smart-turn-v3/tree/main
3）本项目不需要考虑模型转换过程，会直接使用MNN模型
4）树莓派算力有限，目前只是评估，后续可能会在树莓派上加算力卡


