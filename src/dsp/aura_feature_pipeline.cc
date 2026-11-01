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

// Aura: 取自 WeNet 的 runtime/core/frontend/feature_pipeline.cc，改动见 .h 顶部
// 的说明（日志 / 配置字段 / include 路径），外加这里的一处：帧栅格支持
// snip_edges=false（训练侧口径）。
#include "dsp/aura_feature_pipeline.h"

#include <algorithm>
#include <utility>

namespace aura {

FeaturePipeline::FeaturePipeline(const FeaturePipelineConfig& config)
    : config_(config),
      feature_dim_(config.num_bins),
      fbank_(config.num_bins, config.sample_rate, config.frame_length,
             config.frame_shift, config.low_freq, config.pre_emphasis,
             config.scale_input_to_unit, config.log_floor, config.log_base,
             config.window_type, config.mel_type, config.norm_type,
             config.high_freq),
      num_frames_(0),
      input_finished_(false) {
  // kaldi 的取窗起点是 frame*shift + shift/2 - length/2，减去 length/2 之后
  // 落在 0 左边的那一段就是需要镜像补出的点数。400/160 时是 120。
  prefix_len_ = config_.snip_edges
                    ? 0
                    : (config_.frame_length - config_.frame_shift) / 2;
}

void FeaturePipeline::AcceptWaveform(const float* pcm, const int size) {
  std::vector<std::vector<float>> feats;
  std::vector<float> waves;
  waves.insert(waves.end(), remained_wav_.begin(), remained_wav_.end());
  waves.insert(waves.end(), pcm, pcm + size);

  // Aura: snip_edges=false —— 第 0 帧的窗从 -120 开始，负数下标按 kaldi 的规则
  // **镜像**（wave[-1] = wave[0]、wave[-2] = wave[1]……），不是补零。
  //
  // 关键在于：镜像补完之后，剩下的切帧就退化成 snip_edges=true 的老算法 ——
  // 帧栅格整体平移 120 个采样点后完全重合。所以这里只插一次前缀，下面的
  // Compute/remainder 逻辑一个字都不用改。
  if (!prefix_done_) {
    if ((int)waves.size() < prefix_len_) {
      // 还没攒够 120 个真实采样，镜像无从谈起；先存着，下次再来。
      remained_wav_ = std::move(waves);
      finish_condition_.notify_one();
      return;
    }
    if (prefix_len_ > 0) {
      std::vector<float> mirror(prefix_len_);
      for (int i = 0; i < prefix_len_; ++i) {
        mirror[i] = waves[prefix_len_ - 1 - i];
      }
      waves.insert(waves.begin(), mirror.begin(), mirror.end());
    }
    prefix_done_ = true;
  }

  int num_frames = fbank_.Compute(waves, &feats);
  feature_queue_.Push(std::move(feats));
  num_frames_ += num_frames;

  int left_samples = waves.size() - config_.frame_shift * num_frames;
  remained_wav_.resize(left_samples);
  std::copy(waves.begin() + config_.frame_shift * num_frames, waves.end(),
            remained_wav_.begin());
  // We are still adding wave, notify input is not finished
  finish_condition_.notify_one();
}

void FeaturePipeline::AcceptWaveform(const int16_t* pcm, const int size) {
  auto* float_pcm = new float[size];
  // Aura: size 为负时上游直接越界；这里按空输入处理（AcceptWaveform(0 个采样)
  // 是合法调用，pcm 侧的处理调用方自己负责）。
  for (int i = 0; i < size; i++) {
    float_pcm[i] = static_cast<float>(pcm[i]);
  }
  this->AcceptWaveform(float_pcm, size);
  delete[] float_pcm;
}

void FeaturePipeline::set_input_finished() {
  // Aura: 上游是 CHECK（abort）。重复置位本身无害，打一条警告就够了 ——
  // 板端为一个调用顺序问题崩掉整条链路不划算。
  if (input_finished_) {
    AURA_LOGW("dsp.feature", "set_input_finished() 被重复调用");
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    input_finished_ = true;
  }
  finish_condition_.notify_one();
}

bool FeaturePipeline::ReadOne(std::vector<float>* feat) {
  if (!feature_queue_.Empty()) {
    *feat = std::move(feature_queue_.Pop());
    return true;
  } else {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!input_finished_) {
      // This will release the lock and wait for notify_one()
      // from AcceptWaveform() or set_input_finished()
      finish_condition_.wait(lock);
      if (!feature_queue_.Empty()) {
        *feat = std::move(feature_queue_.Pop());
        return true;
      }
    }
    // 走到这里 input_finished_ 必然为真（循环只可能因此退出），上游那句 CHECK
    // 在 Aura 里没有额外信息量，去掉。
    // Double check queue.empty, see issue#893 for detailed discussions.
    if (!feature_queue_.Empty()) {
      *feat = std::move(feature_queue_.Pop());
      return true;
    } else {
      return false;
    }
  }
}

bool FeaturePipeline::Read(int num_frames,
                           std::vector<std::vector<float>>* feats) {
  feats->clear();
  // Aura: Size() 是 size_t，num_frames 是 int；num_frames <= 0 时上游行为未定义，
  // 这里直接返回 false 不碰队列（Pop 里 0 或负数会退化成阻塞等一条）。
  if (num_frames <= 0) {
    return false;
  }
  const size_t want = static_cast<size_t>(num_frames);
  if (feature_queue_.Size() >= want) {
    *feats = std::move(feature_queue_.Pop(want));
    return true;
  } else {
    std::unique_lock<std::mutex> lock(mutex_);
    while (!input_finished_) {
      // This will release the lock and wait for notify_one()
      // from AcceptWaveform() or set_input_finished()
      finish_condition_.wait(lock);
      if (feature_queue_.Size() >= want) {
        *feats = std::move(feature_queue_.Pop(want));
        return true;
      }
    }
    // 同上：循环退出即 input_finished_ 为真。
    // Double check queue.empty, see issue#893 for detailed discussions.
    if (feature_queue_.Size() >= want) {
      *feats = std::move(feature_queue_.Pop(want));
      return true;
    } else {
      *feats = std::move(feature_queue_.Pop(feature_queue_.Size()));
      return false;
    }
  }
}

void FeaturePipeline::Reset() {
  input_finished_ = false;
  num_frames_ = 0;
  remained_wav_.clear();
  // Aura: 复位要连"开头镜像补过了"一起清掉，否则下一句话的第 0 帧会按上一句
  // 的栅格切，听不出来但特征整体错位 120 个采样点。
  prefix_done_ = false;
  feature_queue_.Clear();
}

}  // namespace aura
