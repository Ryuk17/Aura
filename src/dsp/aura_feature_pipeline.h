// Copyright (c) 2017 Personal (Binbin Zhang)
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

// ---------------------------------------------------------------------------
// Aura: 取自 WeNet 的 runtime/core/frontend/feature_pipeline.{h,cc}，改动如下
// （每处带 `Aura:` 注释）：
//   1. 日志换成 Aura 的 logger（utils/logger/logger.h）—— 上游的 utils/log.h
//      是一套基于 ostream 的 LOG(INFO)<<... 宏，本仓库没有，也不打算再引一套。
//   2. 配置加 high_freq 与 snip_edges 两个字段 —— 训练侧（sherpa/kaldi）用的是
//      high_freq=7600、snip_edges=false，两个默认值都与上游不同。
//   3. include 路径改成 Aura 的（dsp/fbank.h）。
// ---------------------------------------------------------------------------
#ifndef AURA_DSP_AURA_FEATURE_PIPELINE_H_
#define AURA_DSP_AURA_FEATURE_PIPELINE_H_

#include <limits>
#include <mutex>
#include <queue>
#include <string>
#include <vector>

#include "dsp/fbank.h"
#include "utils/blocking_queue.h"
#include "utils/logger/logger.h"

namespace aura {

enum class FeatureType {
  kKaldi = 0,
  kWhisper,
};

struct FeaturePipelineConfig {
  int num_bins;
  int sample_rate;
  int frame_length;
  int frame_shift;
  float low_freq;
  // Aura: 语义同 kaldi —— <= 0 表示 Nyquist + high_freq。**注意默认 0 是上游
  // WeNet 的口径（正好 Nyquist 8000Hz），而 Phase 3 的三个模型都是 kaldi/sherpa
  // 训出来的，那边是 -400（7600Hz）。接线时显式写 -400，别指望默认值。
  float high_freq;
  bool pre_emphasis;
  bool scale_input_to_unit;
  float log_floor;
  LogBase log_base;
  WindowType window_type;
  MelType mel_type;
  NormalizationType norm_type;
  // Aura: 帧栅格。true = kaldi 的 snip_edges=true（也是上游 WeNet 的行为），
  // false = 训练侧的 snip_edges=false（开头 120 个采样点按镜像补出第 0 帧）。
  // **三个模型的输入都要 false**；留 true 只是为了和 Fbank::Compute() 的离线
  // 栅格对齐，方便对照测试。
  bool snip_edges;

  FeaturePipelineConfig(int num_bins, int sample_rate,
                        FeatureType feat_type = FeatureType::kKaldi)
      : num_bins(num_bins),                  // 80 dim fbank
        sample_rate(sample_rate) {           // 16k sample rate
    frame_length = sample_rate / 1000 * 25;  // frame length 25ms
    frame_shift = sample_rate / 1000 * 10;   // frame shift 10ms
    if (feat_type == FeatureType::kKaldi) {
      low_freq = 20.0;
      high_freq = 0.0f;
      pre_emphasis = true;
      log_floor = std::numeric_limits<float>::epsilon();
      log_base = LogBase::kBaseE;
      window_type = WindowType::kPovey;
      mel_type = MelType::kHTK;
      norm_type = NormalizationType::kKaldi;
      scale_input_to_unit = false;
      snip_edges = true;
    } else if (feat_type == FeatureType::kWhisper) {
      low_freq = 0.0;
      high_freq = 0.0f;
      pre_emphasis = false;
      log_floor = 1e-10;
      log_base = LogBase::kBase10;
      window_type = WindowType::kHanning;
      mel_type = MelType::kSlaney;
      scale_input_to_unit = true;
      norm_type = NormalizationType::kWhisper;
      snip_edges = true;
    }
  }

  // Aura: 训练侧那一套（sherpa/kaldi）的参数，Phase 3 的模型全用它。
  // 与 kKaldi 的差别只有两处：high_freq=-400、snip_edges=false，正好是两边
  // 唯一的两个口径差。写在构造函数里而不是散在调用点，免得漏一个。
  static FeaturePipelineConfig TrainingDefaults(int num_bins, int sample_rate) {
    FeaturePipelineConfig c(num_bins, sample_rate, FeatureType::kKaldi);
    c.high_freq = -400.0f;
    c.snip_edges = false;
    return c;
  }

  void Info() const {
    AURA_LOGI("dsp.feature",
              "feature pipeline config: num_bins=%d frame_length=%d frame_shift=%d "
              "low_freq=%.1f high_freq=%.1f preemphasis=%d log_floor=%g log_base=%d "
              "window_type=%d mel_type=%d norm_type=%d snip_edges=%d",
              num_bins, frame_length, frame_shift, low_freq, high_freq,
              (int)pre_emphasis, (double)log_floor, (int)log_base, (int)window_type,
              (int)mel_type, (int)norm_type, (int)snip_edges);
  }
};

// Typically, FeaturePipeline is used in two threads: one thread A calls
// AcceptWaveform() to add raw wav data and set_input_finished() to notice
// the end of input wav, another thread B (decoder thread) calls Read() to
// consume features.So a BlockingQueue is used to make this class thread safe.

// The Read() is designed as a blocking method when there is no feature
// in feature_queue_ and the input is not finished.

// See bin/decoder_main.cc, websocket/websocket_server.cc and
// decoder/torch_asr_decoder.cc for usage

class FeaturePipeline {
 public:
  explicit FeaturePipeline(const FeaturePipelineConfig& config);

  // The feature extraction is done in AcceptWaveform().
  void AcceptWaveform(const float* pcm, const int size);
  void AcceptWaveform(const int16_t* pcm, const int size);

  // Current extracted frames number.
  int num_frames() const { return num_frames_; }
  int feature_dim() const { return feature_dim_; }
  const FeaturePipelineConfig& config() const { return config_; }

  // The caller should call this method when speech input is end.
  // Never call AcceptWaveform() after calling set_input_finished() !
  void set_input_finished();
  bool input_finished() const { return input_finished_; }

  // Return False if input is finished and no feature could be read.
  // Return True if a feature is read.
  // This function is a blocking method. It will block the thread when
  // there is no feature in feature_queue_ and the input is not finished.
  bool ReadOne(std::vector<float>* feat);

  // Aura: 非阻塞取一帧（队列空返回 false）。pipeline 的 process() 里禁止阻塞，
  // 而 ReadOne 在"队列空且输入未结束"时会一直等（那是给解码线程的语义）。
  bool TryPopOne(std::vector<float>* feat) { return feature_queue_.TryPop(feat); }

  // Read #num_frames frame features.
  // Return False if less than #num_frames features are read and the
  // input is finished.
  // Return True if #num_frames features are read.
  // This function is a blocking method when there is no feature
  // in feature_queue_ and the input is not finished.
  bool Read(int num_frames, std::vector<std::vector<float>>* feats);

  void Reset();
  bool IsLastFrame(int frame) const {
    return input_finished_ && (frame == num_frames_ - 1);
  }

  int NumQueuedFrames() const { return feature_queue_.Size(); }

 private:
  // Aura: 上游是引用成员（调用方保证 config 比 pipeline 活得长）—— 我们的
  // aura_feature_create 传的是栈上临时对象，引用悬垂后 frame_shift 读到垃圾：
  // 余量逻辑不收缩 → 特征帧数三角式增长（50 音频帧产 201 fbank 帧）。
  // 改成值成员，pipeline 自己持有一份。
  FeaturePipelineConfig config_;
  int feature_dim_;
  Fbank fbank_;

  BlockingQueue<std::vector<float>> feature_queue_;
  int num_frames_;
  bool input_finished_;

  // The feature extraction is done in AcceptWaveform().
  // This waveform sample points are consumed by frame size.
  // The residual waveform sample points after framing are
  // kept to be used in next AcceptWaveform() calling.
  std::vector<float> remained_wav_;

  // Aura: snip_edges=false 时，开头的反射前缀是否已经补进 remained_wav_。
  // 补法见 aura_feature_pipeline.cc 的 AcceptWaveform()。
  bool prefix_done_ = false;
  int prefix_len_ = 0;

  // Used to block the Read when there is no feature in feature_queue_
  // and the input is not finished.
  mutable std::mutex mutex_;
  std::condition_variable finish_condition_;
};

}  // namespace aura

#endif  // FRONTEND_FEATURE_PIPELINE_H_
