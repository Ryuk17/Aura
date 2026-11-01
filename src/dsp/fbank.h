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
//
// ---------------------------------------------------------------------------
// 本文件取自 WeNet 的 runtime/core/frontend/fbank.h。Aura 侧只改了四件事，
// 每处都带 `Aura:` 注释，方便日后与上游对照：
//
//   1. FFT 换成 NE10（dsp/aura_fft.h）—— 与 TrickRoom 的 AEC3 用同一个库、同一份
//      源码（cmake/neon_fft.cmake），免得语音前端出现"两套 FFT 算出的特征对不上"
//      这种最难查的偏差。上游那套 Ooura 的表（bitrev_/sintbl_）随之删掉。
//   2. CHECK 宏换成项目 logger —— 上游的 utils/log.h 不在本仓库。
//   3. high_freq 从硬编码改成构造参数 —— 训练侧（sherpa/kaldi）用的是 7600Hz
//      而不是 Nyquist，这个值不改会让高档 mel 滤波器整体偏移。
//   4. Compute() 拆出单帧入口 ComputeFrame()，并把每帧的临时 vector 提成成员 ——
//      流式调用方（KWS/ASR）每次只要一帧，且运行期不该有分配。
//
// 帧切分（snip_edges=false）**不在本文件里**：这里只管"给一帧定长信号 → 一帧
// 特征"，取窗与反射是流式封装（dsp/aura_fbank.cpp）的事。上游的 Compute() 走的是
// snip_edges=true，只保留给离线对照用。
// ---------------------------------------------------------------------------

#ifndef AURA_DSP_FBANK_H_
#define AURA_DSP_FBANK_H_

#include <cmath>
#include <cstring>
#include <limits>
#include <random>
#include <stdexcept>
#include <utility>
#include <vector>

#include "dsp/aura_fft.h"
#include "utils/logger/logger.h"

namespace aura {

// This code is based on kaldi Fbank implementation, please see
// https://github.com/kaldi-asr/kaldi/blob/master/src/feat/feature-fbank.cc

static const int kS16AbsMax = 1 << 15;

// Aura: 上游从 utils/log.h 拿 M_2PI，这里自带。
static const double k2Pi = 6.283185307179586476925286766559005;

// Aura: 上游用 utils/log.h 的 CHECK/CHECK_GE（失败即 abort）。换成打一条明确
// 错误后返回 —— 这两处断言只可能在 mel 配置明显写错时触发，板端为一个可以继续
// 观测的配置错误崩掉整条流水线不划算。两个调用点都在 void 函数里，故用 return。
#define AURA_FBANK_CHECK(cond, ...)        \
  do {                                     \
    if (!(cond)) {                         \
      AURA_LOGE("dsp.fbank", __VA_ARGS__); \
      return;                              \
    }                                      \
  } while (0)

enum class WindowType {
  kPovey = 0,
  kHanning,
};

enum class MelType {
  kHTK = 0,
  kSlaney,
};

enum class NormalizationType {
  kKaldi = 0,
  kWhisper,
};

enum class LogBase {
  kBaseE = 0,
  kBase10,
};

class Fbank {
 public:
  // Aura: 新增 high_freq 参数，语义跟 kaldi 的 FrameExtractionOptions 一致：
  //   <= 0 → Nyquist + high_freq（所以 -400 就是 7600Hz）
  //   == 0 → 正好 Nyquist（= 上游原本的硬编码行为）
  //    > 0 → 就是它本身
  // 其余参数与上游默认值逐字保持一致。
  Fbank(int num_bins, int sample_rate, int frame_length, int frame_shift,
        float low_freq = 20, bool pre_emphasis = true,
        bool scale_input_to_unit = false,
        float log_floor = std::numeric_limits<float>::epsilon(),
        LogBase log_base = LogBase::kBaseE,
        WindowType window_type = WindowType::kPovey,
        MelType mel_type = MelType::kHTK,
        NormalizationType norm_type = NormalizationType::kKaldi,
        float high_freq = 0)
      : num_bins_(num_bins),
        sample_rate_(sample_rate),
        frame_length_(frame_length),
        frame_shift_(frame_shift),
        use_log_(true),
        remove_dc_offset_(true),
        // Aura: 初始化顺序与成员声明顺序保持一致（见类尾），否则 -Wreorder。
        pre_emphasis_(pre_emphasis),
        scale_input_to_unit_(scale_input_to_unit),
        low_freq_(low_freq),
        log_floor_(log_floor),
        high_freq_(high_freq),
        log_base_(log_base),
        norm_type_(norm_type),
        generator_(0),
        distribution_(0, 1.0),
        dither_(0.0) {
    // Aura: kaldi 的 high_freq 语义（见上面的注释）。
    if (high_freq_ <= 0.0f) {
      high_freq_ += static_cast<float>(sample_rate_) / 2.0f;
    }
    if (high_freq_ <= low_freq_ ||
        high_freq_ > static_cast<float>(sample_rate_) / 2.0f) {
      AURA_LOGE("dsp.fbank",
                "high_freq=%.1f 不在 (%.1f, %d/2] 内，退回 Nyquist",
                high_freq_, low_freq_, sample_rate_);
      high_freq_ = static_cast<float>(sample_rate_) / 2.0f;
    }

    fft_points_ = UpperPowerOfTwo(frame_length_);

    // Aura: 上游在这里生成 Ooura 的位反转表与三角函数表；换成 NE10 后由
    // aura_fft_create 内部准备旋转因子，这里只需要一个句柄。
    fft_ = aura_fft_create(fft_points_);
    if (fft_ == NULL) {
      AURA_LOGE("dsp.fbank", "FFT 初始化失败 (points=%d)", fft_points_);
    }

    // Aura: 每帧复用的临时缓冲，构造时分配一次，运行期不再分配。
    frame_.resize(frame_length_);
    padded_.resize(fft_points_);
    power_.resize(fft_points_ / 2);

    InitMelFilters(mel_type);
    InitWindow(window_type);
  }

  // Aura: 上游没有析构函数（表都在 vector 里）；NE10 的 cfg 要手工释放。
  ~Fbank() { aura_fft_destroy(fft_); }
  Fbank(const Fbank &) = delete;
  Fbank &operator=(const Fbank &) = delete;

  void InitMelFilters(MelType mel_type) {
    int num_fft_bins = fft_points_ / 2;
    float fft_bin_width = static_cast<float>(sample_rate_) / fft_points_;
    float mel_low_freq = MelScale(low_freq_, mel_type);
    float mel_high_freq = MelScale(high_freq_, mel_type);
    float mel_freq_delta = (mel_high_freq - mel_low_freq) / (num_bins_ + 1);
    bins_.resize(num_bins_);
    center_freqs_.resize(num_bins_);

    for (int bin = 0; bin < num_bins_; ++bin) {
      float left_mel = mel_low_freq + bin * mel_freq_delta,
            center_mel = mel_low_freq + (bin + 1) * mel_freq_delta,
            right_mel = mel_low_freq + (bin + 2) * mel_freq_delta;
      center_freqs_[bin] = InverseMelScale(center_mel, mel_type);
      std::vector<float> this_bin(num_fft_bins);
      int first_index = -1, last_index = -1;
      for (int i = 0; i < num_fft_bins; ++i) {
        float freq = (fft_bin_width * i);  // Center frequency of this fft
        // bin.
        float mel = MelScale(freq, mel_type);
        if (mel > left_mel && mel < right_mel) {
          // Aura: 初始化成 0 —— 两个分支（HTK/Slaney）都会赋值，但 GCC 证不出
          // 这一点，留着未初始化会带着 -Wmaybe-uninitialized 一起编。
          float weight = 0.0f;
          if (mel_type == MelType::kHTK) {
            if (mel <= center_mel)
              weight = (mel - left_mel) / (center_mel - left_mel);
            else if (mel > center_mel)
              weight = (right_mel - mel) / (right_mel - center_mel);
          } else if (mel_type == MelType::kSlaney) {
            if (mel <= center_mel) {
              weight = (InverseMelScale(mel, mel_type) -
                        InverseMelScale(left_mel, mel_type)) /
                       (InverseMelScale(center_mel, mel_type) -
                        InverseMelScale(left_mel, mel_type));
              weight *= 2.0 / (InverseMelScale(right_mel, mel_type) -
                               InverseMelScale(left_mel, mel_type));
            } else if (mel > center_mel) {
              weight = (InverseMelScale(right_mel, mel_type) -
                        InverseMelScale(mel, mel_type)) /
                       (InverseMelScale(right_mel, mel_type) -
                        InverseMelScale(center_mel, mel_type));
              weight *= 2.0 / (InverseMelScale(right_mel, mel_type) -
                               InverseMelScale(left_mel, mel_type));
            }
          }
          this_bin[i] = weight;
          if (first_index == -1) first_index = i;
          last_index = i;
        }
      }
      AURA_FBANK_CHECK(first_index != -1 && last_index >= first_index,
                       "mel 滤波器 %d 没落到任何 FFT bin 上（num_bins=%d, "
                       "low=%.1f, high=%.1f）：特征会全 0，检查配置",
                       bin, num_bins_, low_freq_, high_freq_);
      bins_[bin].first = first_index;
      int size = last_index + 1 - first_index;
      bins_[bin].second.resize(size);
      for (int i = 0; i < size; ++i) {
        bins_[bin].second[i] = this_bin[first_index + i];
      }
    }
  }

  void InitWindow(WindowType window_type) {
    window_.resize(frame_length_);
    if (window_type == WindowType::kPovey) {
      // povey window
      double a = k2Pi / (frame_length_ - 1);
      for (int i = 0; i < frame_length_; ++i)
        window_[i] = std::pow(0.5 - 0.5 * std::cos(a * i), 0.85);
    } else if (window_type == WindowType::kHanning) {
      // periodic hanning window
      double a = k2Pi / (frame_length_);
      for (int i = 0; i < frame_length_; ++i)
        window_[i] = 0.5 * (1.0 - std::cos(i * a));
    }
  }

  void set_use_log(bool use_log) { use_log_ = use_log; }

  void set_remove_dc_offset(bool remove_dc_offset) {
    remove_dc_offset_ = remove_dc_offset;
  }

  void set_dither(float dither) { dither_ = dither; }

  int num_bins() const { return num_bins_; }

  // Aura: 构造期资源是否就绪（目前只有 FFT）。失败不抛异常，由调用方查。
  bool ok() const { return fft_ != nullptr; }

  static inline float InverseMelScale(float mel_freq,
                                      MelType mel_type = MelType::kHTK) {
    if (mel_type == MelType::kHTK) {
      return 700.0f * (expf(mel_freq / 1127.0f) - 1.0f);
    } else if (mel_type == MelType::kSlaney) {
      float f_min = 0.0;
      float f_sp = 200.0f / 3.0f;
      float min_log_hz = 1000.0;
      float freq = f_min + f_sp * mel_freq;
      float min_log_mel = (min_log_hz - f_min) / f_sp;
      float logstep = logf(6.4) / 27.0f;
      if (mel_freq >= min_log_mel) {
        return min_log_hz * expf(logstep * (mel_freq - min_log_mel));
      } else {
        return freq;
      }
    } else {
      throw std::invalid_argument("Unsupported mel type!");
    }
  }

  static inline float MelScale(float freq, MelType mel_type = MelType::kHTK) {
    if (mel_type == MelType::kHTK) {
      return 1127.0f * logf(1.0f + freq / 700.0f);
    } else if (mel_type == MelType::kSlaney) {
      float f_min = 0.0;
      float f_sp = 200.0f / 3.0f;
      float min_log_hz = 1000.0;
      float mel = (freq - f_min) / f_sp;
      float min_log_mel = (min_log_hz - f_min) / f_sp;
      float logstep = logf(6.4) / 27.0f;
      if (freq >= min_log_hz) {
        return min_log_mel + logf(freq / min_log_hz) / logstep;
      } else {
        return mel;
      }
    } else {
      throw std::invalid_argument("Unsupported mel type!");
    }
  }

  static int UpperPowerOfTwo(int n) {
    return static_cast<int>(std::pow(2, std::ceil(std::log(n) / std::log(2))));
  }

  // pre emphasis
  void PreEmphasis(float coeff, std::vector<float>* data) const {
    if (coeff == 0.0) return;
    for (int i = data->size() - 1; i > 0; i--)
      (*data)[i] -= coeff * (*data)[i - 1];
    (*data)[0] -= coeff * (*data)[0];
  }

  // Apply window on data in place
  void ApplyWindow(std::vector<float>* data) const {
    // Aura: %llu + 强转 —— MinGW 的 msvcrt printf 不认识 %zu（logger 直接走
    // vsnprintf），用了会在运行时打出乱码而不是数字。
    AURA_FBANK_CHECK(data->size() >= window_.size(),
                     "加窗输入 %llu 短于窗长 %llu",
                     (unsigned long long)data->size(),
                     (unsigned long long)window_.size());
    for (size_t i = 0; i < window_.size(); ++i) {
      (*data)[i] *= window_[i];
    }
  }

  void WhisperNorm(std::vector<std::vector<float>>* feat,
                   float max_mel_engery) {
    int num_frames = feat->size();
    for (int i = 0; i < num_frames; ++i) {
      for (int j = 0; j < num_bins_; ++j) {
        float energy = (*feat)[i][j];
        if (energy < max_mel_engery - 8) energy = max_mel_engery - 8;
        energy = (energy + 4.0) / 4.0;
        (*feat)[i][j] = energy;
      }
    }
  }

  // Aura: 上游 Compute() 的循环体，抽成"一帧进、一帧出"。
  // frame 必须是 frame_length_ 个采样（调用方保证，不在这里再切一次窗）；
  // out 写 num_bins_ 个 float。返回 num_bins_，便于链式书写。
  int ComputeFrame(const float* frame, float* out) {
    std::memcpy(frame_.data(), frame, sizeof(float) * frame_length_);

    if (scale_input_to_unit_) {
      for (int j = 0; j < frame_length_; ++j) {
        frame_[j] = frame_[j] / kS16AbsMax;
      }
    }

    // optional add noise
    if (dither_ != 0.0) {
      for (int j = 0; j < frame_length_; ++j)
        frame_[j] += dither_ * distribution_(generator_);
    }
    // optinal remove dc offset
    if (remove_dc_offset_) {
      float mean = 0.0;
      for (int j = 0; j < frame_length_; ++j) mean += frame_[j];
      mean /= frame_length_;
      for (int j = 0; j < frame_length_; ++j) frame_[j] -= mean;
    }

    if (pre_emphasis_) {
      PreEmphasis(0.97, &frame_);
    }
    ApplyWindow(&frame_);

    // Aura: 补零到 fft_points_ 后交给 NE10。上游在这里还 memset 了一个
    // fft_img（Ooura 用双数组），NE10 的复数输出是它自己的事，不需要。
    std::memset(padded_.data() + frame_length_, 0,
                sizeof(float) * (fft_points_ - frame_length_));
    std::memcpy(padded_.data(), frame_.data(),
                sizeof(float) * frame_length_);

    // Aura: 功率谱 [DC, bin1 .. bin(nfft/2-1)]，不含 Nyquist —— 与 kaldi 的
    // num_fft_bins = nfft/2 一致。
    if (aura_fft_power(fft_, padded_.data(), power_.data()) != AURA_OK) {
      AURA_LOGE("dsp.fbank", "FFT 失败");
      for (int j = 0; j < num_bins_; ++j) out[j] = log_floor_;
      return num_bins_;
    }

    // cepstral coefficients, triangle filter array
    for (int j = 0; j < num_bins_; ++j) {
      float mel_energy = 0.0;
      int s = bins_[j].first;
      for (size_t k = 0; k < bins_[j].second.size(); ++k) {
        mel_energy += bins_[j].second[k] * power_[s + k];
      }
      // optional use log
      if (use_log_) {
        if (mel_energy < log_floor_) mel_energy = log_floor_;

        if (log_base_ == LogBase::kBaseE)
          mel_energy = logf(mel_energy);
        else if (log_base_ == LogBase::kBase10)
          mel_energy = log10(mel_energy);
      }
      out[j] = mel_energy;
    }
    return num_bins_;
  }

  // Compute fbank feat, return num frames
  // Aura: 离线整段入口，保留用于对照测试。**帧切分是 snip_edges=true**
  // （上游行为，1 + (N-L)/S）；训练侧要的 snip_edges=false 在
  // dsp/aura_fbank.cpp 里做，两者帧栅格差 120 个采样点，不要混用。
  int Compute(const std::vector<float>& wave,
              std::vector<std::vector<float>>* feat) {
    int num_samples = wave.size();

    if (num_samples < frame_length_) return 0;
    int num_frames = 1 + ((num_samples - frame_length_) / frame_shift_);
    feat->resize(num_frames);

    float max_mel_engery = std::numeric_limits<float>::min();

    for (int i = 0; i < num_frames; ++i) {
      (*feat)[i].resize(num_bins_);
      ComputeFrame(wave.data() + i * frame_shift_, (*feat)[i].data());
    }
    if (norm_type_ == NormalizationType::kWhisper) {
      for (int i = 0; i < num_frames; ++i) {
        for (int j = 0; j < num_bins_; ++j) {
          if (max_mel_engery < (*feat)[i][j]) max_mel_engery = (*feat)[i][j];
        }
      }
      WhisperNorm(feat, max_mel_engery);
    }

    return num_frames;
  }

 private:
  int num_bins_;
  int sample_rate_;
  int frame_length_, frame_shift_;
  int fft_points_;
  bool use_log_;
  bool remove_dc_offset_;
  bool pre_emphasis_;
  bool scale_input_to_unit_;
  float low_freq_;
  float log_floor_;
  float high_freq_;
  LogBase log_base_;
  NormalizationType norm_type_;

  std::vector<float> center_freqs_;
  std::vector<std::pair<int, std::vector<float>>> bins_;
  std::vector<float> window_;
  std::default_random_engine generator_;
  std::normal_distribution<float> distribution_;
  float dither_;

  // Aura: 替换上游的 Ooura 查表（bitrev_ / sintbl_）与每帧临时 vector。
  aura_fft_t* fft_ = nullptr;
  std::vector<float> frame_;   // 加窗/去直流/预加重的工作区（frame_length_ 个）
  std::vector<float> padded_;  // 补零后的 FFT 输入（fft_points_ 个）
  std::vector<float> power_;   // 功率谱（fft_points_/2 个）
};

}  // namespace aura

#undef AURA_FBANK_CHECK

#endif  // AURA_DSP_FBANK_H_
