// WASAPI 采集/播放实现（共享模式 + 自动格式转换到 16k mono）
#include "wasapi_device.h"

#include "log.h"

#ifndef _WIN32
#error "wasapi_device.cc is Windows-only"
#endif

#define WIN32_LEAN_AND_MEAN
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <windows.h>

#include <cstring>

namespace aura::platform {

namespace {
constexpr const char* kTag = "wasapi";

// 16 kHz / 16 bit / mono 格式描述（请求格式，系统自动转换）
WAVEFORMATEX kRequestFormat = {
    WAVE_FORMAT_PCM, 1, 16000, 32000, 2, 16, 0};

constexpr REFERENCE_TIME kBufferDuration = 100000;  // 10ms

// 复用 IAudioClient 初始化逻辑
bool InitAudioClient(IAudioClient** out_client, IMMDeviceEnumerator* enumerator,
                     EDataFlow flow, bool event_callback) {
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    HRESULT hr = enumerator->GetDefaultAudioEndpoint(flow, eConsole, &device);
    if (FAILED(hr)) {
        ALOG_ERROR(kTag, "GetDefaultAudioEndpoint failed: 0x%08x", static_cast<unsigned>(hr));
        return false;
    }
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(&client));
    device->Release();
    if (FAILED(hr)) {
        ALOG_ERROR(kTag, "Activate(IAudioClient) failed: 0x%08x", static_cast<unsigned>(hr));
        return false;
    }

    DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM;  // 允许系统转换到请求格式
    if (event_callback) flags |= AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kBufferDuration, 0,
                            &kRequestFormat, nullptr);
    if (FAILED(hr)) {
        ALOG_ERROR(kTag, "IAudioClient::Initialize failed: 0x%08x", static_cast<unsigned>(hr));
        client->Release();
        return false;
    }
    *out_client = client;
    return true;
}

}  // namespace

// ---------------- 采集 ----------------

WasapiCaptureDevice::WasapiCaptureDevice() = default;
WasapiCaptureDevice::~WasapiCaptureDevice() { Stop(); }

bool WasapiCaptureDevice::Start(SampleCallback cb) {
    if (capturing_.load()) return true;
    cb_ = std::move(cb);
    if (!cb_) {
        ALOG_ERROR(kTag, "capture Start without callback");
        return false;
    }

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    coinit_state_ = reinterpret_cast<void*>(static_cast<uintptr_t>(hr));

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator),
                          reinterpret_cast<void**>(&enumerator_));
    if (FAILED(hr)) {
        ALOG_ERROR(kTag, "CoCreateInstance(MMDeviceEnumerator) failed: 0x%08x",
                   static_cast<unsigned>(hr));
        return false;
    }
    if (!InitAudioClient(&audio_client_, enumerator_, eCapture, /*event_callback=*/true)) {
        return false;
    }
    HRESULT hr2 = audio_client_->GetService(__uuidof(IAudioCaptureClient),
                                            reinterpret_cast<void**>(&capture_client_));
    if (FAILED(hr2)) {
        ALOG_ERROR(kTag, "GetService(IAudioCaptureClient) failed: 0x%08x",
                   static_cast<unsigned>(hr2));
        return false;
    }

    event_handle_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (FAILED(audio_client_->SetEventHandle(static_cast<HANDLE>(event_handle_)))) {
        ALOG_ERROR(kTag, "SetEventHandle failed");
        return false;
    }
    if (FAILED(audio_client_->Start())) {
        ALOG_ERROR(kTag, "IAudioClient::Start (capture) failed");
        return false;
    }

    stop_requested_ = false;
    capturing_ = true;
    thread_ = std::thread(&WasapiCaptureDevice::CaptureLoop, this);
    ALOG_INFO(kTag, "capture started (16 kHz/16 bit/mono)");
    return true;
}

void WasapiCaptureDevice::Stop() {
    if (!capturing_.load()) return;
    stop_requested_ = true;
    if (event_handle_) {
        SetEvent(static_cast<HANDLE>(event_handle_));  // 唤醒采集线程
    }
    if (thread_.joinable()) thread_.join();
    capturing_ = false;

    if (audio_client_) {
        audio_client_->Stop();
        audio_client_->Release();
        audio_client_ = nullptr;
    }
    if (capture_client_) {
        capture_client_->Release();
        capture_client_ = nullptr;
    }
    if (enumerator_) {
        enumerator_->Release();
        enumerator_ = nullptr;
    }
    if (event_handle_) {
        CloseHandle(static_cast<HANDLE>(event_handle_));
        event_handle_ = nullptr;
    }
    if (coinit_state_) {
        CoUninitialize();
        coinit_state_ = nullptr;
    }
    ALOG_INFO(kTag, "capture stopped");
}

void WasapiCaptureDevice::CaptureLoop() {
    // 提升音频线程优先级
    HANDLE task = AvSetMmThreadCharacteristicsW(L"Audio", nullptr);

    while (!stop_requested_.load()) {
        DWORD wait = WaitForSingleObject(static_cast<HANDLE>(event_handle_), 200);
        if (wait == WAIT_OBJECT_0) {
            UINT32 packet = 0;
            while (SUCCEEDED(capture_client_->GetNextPacketSize(&packet)) && packet > 0) {
                BYTE* data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                HRESULT hr = capture_client_->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
                if (SUCCEEDED(hr)) {
                    if (data && frames > 0 && !(flags & AUDCLNT_BUFFERFLAGS_SILENT)) {
                        cb_(reinterpret_cast<const int16_t*>(data), frames);
                    }
                    capture_client_->ReleaseBuffer(frames);
                }
                if (SUCCEEDED(capture_client_->GetNextPacketSize(&packet)) && packet == 0) break;
            }
        }
    }

    if (task) AvRevertMmThreadCharacteristics(task);
}

// ---------------- 播放 ----------------

WasapiPlaybackDevice::WasapiPlaybackDevice() = default;
WasapiPlaybackDevice::~WasapiPlaybackDevice() { Stop(); }

bool WasapiPlaybackDevice::Start() {
    if (playing_.load()) return true;

    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator),
                          reinterpret_cast<void**>(&enumerator_));
    if (FAILED(hr)) {
        ALOG_ERROR(kTag, "CoCreateInstance(MMDeviceEnumerator) failed: 0x%08x",
                   static_cast<unsigned>(hr));
        return false;
    }
    if (!InitAudioClient(&audio_client_, enumerator_, eRender, /*event_callback=*/true)) {
        return false;
    }
    HRESULT hr2 = audio_client_->GetService(__uuidof(IAudioRenderClient),
                                            reinterpret_cast<void**>(&render_client_));
    if (FAILED(hr2)) {
        ALOG_ERROR(kTag, "GetService(IAudioRenderClient) failed: 0x%08x",
                   static_cast<unsigned>(hr2));
        return false;
    }
    audio_client_->GetBufferSize(&buffer_frames_);

    event_handle_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
    if (FAILED(audio_client_->SetEventHandle(static_cast<HANDLE>(event_handle_)))) {
        ALOG_ERROR(kTag, "SetEventHandle failed");
        return false;
    }

    // 预填静音，避免启动时的引擎欠载
    BYTE* data = nullptr;
    if (SUCCEEDED(render_client_->GetBuffer(buffer_frames_, &data))) {
        memset(data, 0, buffer_frames_ * 2);
        render_client_->ReleaseBuffer(buffer_frames_, AUDCLNT_BUFFERFLAGS_SILENT);
    }
    if (FAILED(audio_client_->Start())) {
        ALOG_ERROR(kTag, "IAudioClient::Start (render) failed");
        return false;
    }

    stop_requested_ = false;
    playing_ = true;
    thread_ = std::thread(&WasapiPlaybackDevice::PlaybackLoop, this);
    ALOG_INFO(kTag, "playback started (16 kHz/16 bit/mono, buffer=%u frames)", buffer_frames_);
    return true;
}

void WasapiPlaybackDevice::Stop() {
    if (!playing_.load()) return;
    stop_requested_ = true;
    if (event_handle_) {
        SetEvent(static_cast<HANDLE>(event_handle_));
    }
    if (thread_.joinable()) thread_.join();
    playing_ = false;

    {
        std::lock_guard<std::mutex> lock(fifo_mutex_);
        fifo_.clear();
    }
    if (audio_client_) {
        audio_client_->Stop();
        audio_client_->Release();
        audio_client_ = nullptr;
    }
    if (render_client_) {
        render_client_->Release();
        render_client_ = nullptr;
    }
    if (enumerator_) {
        enumerator_->Release();
        enumerator_ = nullptr;
    }
    if (event_handle_) {
        CloseHandle(static_cast<HANDLE>(event_handle_));
        event_handle_ = nullptr;
    }
    ALOG_INFO(kTag, "playback stopped");
}

void WasapiPlaybackDevice::Push(const int16_t* data, size_t frames) {
    std::lock_guard<std::mutex> lock(fifo_mutex_);
    fifo_.insert(fifo_.end(), data, data + frames);
}

void WasapiPlaybackDevice::Clear() {
    std::lock_guard<std::mutex> lock(fifo_mutex_);
    fifo_.clear();
}

void WasapiPlaybackDevice::PlaybackLoop() {
    HANDLE task = AvSetMmThreadCharacteristicsW(L"Audio", nullptr);

    while (!stop_requested_.load()) {
        DWORD wait = WaitForSingleObject(static_cast<HANDLE>(event_handle_), 200);
        if (wait != WAIT_OBJECT_0) continue;

        UINT32 padding = 0;
        audio_client_->GetCurrentPadding(&padding);
        UINT32 available = buffer_frames_ - padding;
        if (available > 0) {
            BYTE* data = nullptr;
            if (SUCCEEDED(render_client_->GetBuffer(available, &data))) {
                {
                    std::lock_guard<std::mutex> lock(fifo_mutex_);
                    size_t take = std::min(static_cast<size_t>(available),
                                           static_cast<size_t>(fifo_.size()));
                    if (take > 0) {
                        memcpy(data, fifo_.data(), take * 2);
                        fifo_.erase(fifo_.begin(), fifo_.begin() + take);
                    }
                    if (take < available) {
                        memset(data + take * 2, 0, (available - take) * 2);
                    }
                }
                render_client_->ReleaseBuffer(available, 0);
            }
        }
    }

    if (task) AvRevertMmThreadCharacteristics(task);
}

}  // namespace aura::platform
