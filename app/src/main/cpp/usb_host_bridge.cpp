#include "usb_host_bridge.h"
#include <android/log.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <vector>
#include "libuac.h"

#define USB_HOST_TAG "PulseForgeUsbHost"
#define USB_HOST_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, USB_HOST_TAG, __VA_ARGS__)
#define USB_HOST_LOGI(...) __android_log_print(ANDROID_LOG_INFO, USB_HOST_TAG, __VA_ARGS__)

namespace {
class UsbHostAudio {
public:
    UsbHostAudio(int fd, int rate, bool in, bool out, int outputMinBufferMs,
                 int outputMaxBufferMs, int inputBurstPackets, int outputBurstPackets)
        : inputRing_(kRingFrames * 2), outputRing_(kRingFrames * 2), rate_(rate),
          outputMinBufferMs_(outputMinBufferMs), outputMaxBufferMs_(outputMaxBufferMs),
          inputBurstPackets_(std::clamp(inputBurstPackets, 1, 16)),
          outputBurstPackets_(std::clamp(outputBurstPackets, 1, 16)) {
        context_ = uac::uac_context::create();
        device_ = context_->wrap(fd);
        if (in) startInput(rate);
        if (out) startOutput(rate);
        if ((in && !inputStream_) || (out && !outputStream_))
            throw std::runtime_error("requested USB audio route/format not found");
        USB_HOST_LOGI("USB Host audio started input=%d output=%d, buffer=%d/%d/%d ms, burst=%d/%d",
                      inputStream_ != nullptr, outputStream_ != nullptr,
                      outputMinBufferMs_,
                      outputMinBufferMs_ + (outputMaxBufferMs_ - outputMinBufferMs_) / 2,
                      outputMaxBufferMs_, inputBurstPackets_,
                      outputBurstPackets_);
    }
    ~UsbHostAudio() {
        // Stream callbacks capture this object. Stop and drain both streams
        // while the rings and mutexes are still alive; implicit member
        // destruction would otherwise destroy those callback targets first.
        stopping_.store(true, std::memory_order_release);
        outputStream_.reset();
        inputStream_.reset();
        device_.reset();
        context_.reset();
    }
    int read(float *dst, int frames) {
        if (!dst || frames <= 0) return 0;
        std::unique_lock<std::mutex> lock(inputMutex_);
        inputCv_.wait_for(lock, std::chrono::milliseconds(20),
                          [this] { return inputCount_ > 0; });
        int n = std::min(frames, inputCount_);
        for (int i = 0; i < n; ++i) { int p = (inputRead_ + i) % kRingFrames; dst[2*i] = inputRing_[2*p]; dst[2*i+1] = inputRing_[2*p+1]; }
        inputRead_ = (inputRead_ + n) % kRingFrames; inputCount_ -= n;
        return n;
    }
    int write(const float *src, int frames) {
        if (!src || frames <= 0) return 0;
        std::lock_guard<std::mutex> lock(outputMutex_);
        outputHasData_ = true;
        for (int i = 0; i < frames; ++i) {
            if (outputCount_ == kRingFrames) {
                outputRead_ = (outputRead_ + 1) % kRingFrames;
                --outputCount_;
            }
            int p = outputWrite_;
            outputWrite_ = (outputWrite_ + 1) % kRingFrames;
            outputRing_[2*p] = src[2*i];
            outputRing_[2*p+1] = src[2*i+1];
            ++outputCount_;
        }
        return frames;
    }
    void configureOutputBuffer(int minBufferMs, int maxBufferMs) {
        std::lock_guard<std::mutex> lock(outputMutex_);
        outputMinBufferMs_ = std::clamp(minBufferMs, 8, 200);
        outputMaxBufferMs_ = std::clamp(maxBufferMs, 8, 500);
        if (outputMaxBufferMs_ < outputMinBufferMs_)
            outputMaxBufferMs_ = outputMinBufferMs_;
        updateOutputBufferFrames();
        while (outputCount_ > outputMaxPrerollFrames_) {
            outputRead_ = (outputRead_ + 1) % kRingFrames;
            --outputCount_;
        }
        outputPrimed_ = false;
        outputUnderruns_ = 0;
        USB_HOST_LOGI("USB output buffer configured min=%d, target=%d, max=%d frames",
                      outputMinFrames_, outputPrerollFrames_, outputMaxPrerollFrames_);
    }
private:
    static constexpr int kRingFrames = 96000;
    static float decode(const uint8_t *s, int n) {
        if (n == 2) return (float)(int16_t)(s[0] | (s[1] << 8)) / 32768.0f;
        if (n == 3) { int32_t v = s[0] | (s[1] << 8) | (s[2] << 16); if (v & 0x00800000) v |= ~0x00ffffff; return (float)v / 8388608.0f; }
        if (n == 4) { int32_t v = (int32_t)(s[0] | (s[1] << 8) | (s[2] << 16) | (s[3] << 24)); return (float)v / 2147483648.0f; }
        return 0.0f;
    }
    static void encode(uint8_t *s, float x, int n) {
        x = std::max(-1.0f, std::min(1.0f, x));
        if (n == 2) { int32_t v = (int32_t)(x * 32767.0f); s[0] = (uint8_t)v; s[1] = (uint8_t)(v >> 8); }
        else if (n == 3) { int32_t v = (int32_t)(x * 8388607.0f); s[0] = (uint8_t)v; s[1] = (uint8_t)(v >> 8); s[2] = (uint8_t)(v >> 16); }
        else if (n == 4) { int64_t v = (int64_t)(x * 2147483647.0f); s[0] = (uint8_t)v; s[1] = (uint8_t)(v >> 8); s[2] = (uint8_t)(v >> 16); s[3] = (uint8_t)(v >> 24); }
        else std::memset(s, 0, (size_t)n);
    }
    void onInput(uint8_t *data, uint len, int bytes, int ch) {
        if (stopping_.load(std::memory_order_acquire)) return;
        int stride = bytes * ch; if (stride <= 0) return; int count = (int)(len / (uint)stride);
        {
            std::lock_guard<std::mutex> lock(inputMutex_);
            for (int i = 0; i < count; ++i) { const uint8_t *f = data + i * stride; float l = decode(f, bytes), r = ch > 1 ? decode(f + bytes, bytes) : l; if (inputCount_ == kRingFrames) { inputRead_ = (inputRead_ + 1) % kRingFrames; --inputCount_; } int p = inputWrite_; inputWrite_ = (inputWrite_ + 1) % kRingFrames; inputRing_[2*p] = l; inputRing_[2*p+1] = r; ++inputCount_; }
        }
        inputCv_.notify_one();
    }
    void onOutput(uint8_t *data, uint len, int bytes, int ch) {
        if (stopping_.load(std::memory_order_acquire)) { std::memset(data, 0, len); return; }
        int stride = bytes * ch; if (stride <= 0) return; std::lock_guard<std::mutex> lock(outputMutex_); int count = (int)(len / (uint)stride);
        if (outputCount_ >= outputMaxPrerollFrames_) {
            const int drop = outputCount_ - outputPrerollFrames_;
            outputRead_ = (outputRead_ + drop) % kRingFrames;
            outputCount_ -= drop;
        }
        if (!outputPrimed_) {
            if (outputCount_ >= outputPrerollFrames_) outputPrimed_ = true;
            else { std::memset(data, 0, len); return; }
        }
        if (outputCount_ < outputMinFrames_ || outputCount_ < count) {
            if (outputHasData_) {
                ++outputUnderruns_;
                if (outputUnderruns_ == 1 || outputUnderruns_ % 10 == 0)
                    USB_HOST_LOGI("USB output below minimum count=%llu, buffered=%d, min=%d, target=%d frames",
                                  (unsigned long long)outputUnderruns_, outputCount_,
                                  outputMinFrames_, outputPrerollFrames_);
            }
            outputPrimed_ = false;
            std::memset(data, 0, len);
            return;
        }
        for (int i = 0; i < count; ++i) { int p = outputRead_; outputRead_ = (outputRead_ + 1) % kRingFrames; float l = outputRing_[2*p], r = outputRing_[2*p+1]; --outputCount_; uint8_t *f = data + i * stride; encode(f, l, bytes); if (ch > 1) encode(f + bytes, r, bytes); for (int c = 2; c < ch; ++c) encode(f + c * bytes, 0, bytes); }
        int used = count * stride; if (used < (int)len) std::memset(data + used, 0, len - used);
    }
    void startInput(int rate) {
        auto routes = device_->get_device()->query_audio_routes(uac::UAC_TERMINAL_ANY, uac::UAC_TERMINAL_USB_STREAMING); if (routes.empty()) return;
        const auto &si = device_->get_device()->get_stream_interface(routes.front().get()); auto cfg = si.query_config_uncompressed(uac::UAC_FORMAT_DATA_PCM, 2, (uint32_t)rate); if (!cfg) return;
        int bytes = cfg->bSubframeSize, ch = cfg->bChannelCount; inputStream_ = device_->start_streaming(si, *cfg, [this, bytes, ch](uint8_t *d, uint n) { onInput(d, n, bytes, ch); }, inputBurstPackets_);
    }
    void startOutput(int rate) {
        auto routes = device_->get_device()->query_audio_routes(uac::UAC_TERMINAL_USB_STREAMING, uac::UAC_TERMINAL_ANY); if (routes.empty()) return;
        const auto &si = device_->get_device()->get_stream_interface(routes.front().get()); auto cfg = si.query_config_uncompressed(uac::UAC_FORMAT_DATA_PCM, 2, (uint32_t)rate); if (!cfg) return;
        int bytes = cfg->bSubframeSize, ch = cfg->bChannelCount;
        outputTransferFrames_ = (outputBurstPackets_ * cfg->wMaxPacketSize) / std::max(1, bytes * ch);
        updateOutputBufferFrames();
        outputStream_ = device_->start_streaming(si, *cfg, [this, bytes, ch](uint8_t *d, uint n) { onOutput(d, n, bytes, ch); }, outputBurstPackets_);
    }
    void updateOutputBufferFrames() {
        if (outputTransferFrames_ <= 0) return;
        outputMinBufferMs_ = std::clamp(outputMinBufferMs_, 8, 200);
        outputMaxBufferMs_ = std::clamp(outputMaxBufferMs_, 8, 500);
        if (outputMaxBufferMs_ < outputMinBufferMs_)
            outputMaxBufferMs_ = outputMinBufferMs_;
        const int configuredMinFrames = (rate_ * outputMinBufferMs_ + 999) / 1000;
        const int configuredMaxFrames = (rate_ * outputMaxBufferMs_ + 999) / 1000;
        outputMinFrames_ = std::max(outputTransferFrames_, configuredMinFrames);
        outputMaxPrerollFrames_ = std::max(outputMinFrames_, configuredMaxFrames);
        outputMaxPrerollFrames_ = std::min(outputMaxPrerollFrames_, kRingFrames);
        outputPrerollFrames_ = outputMinFrames_ +
                               (outputMaxPrerollFrames_ - outputMinFrames_) / 2;
    }
    std::shared_ptr<uac::uac_context> context_; std::shared_ptr<uac::uac_device_handle> device_; std::shared_ptr<uac::uac_stream_handle> inputStream_, outputStream_;
    std::atomic<bool> stopping_{false};
    std::mutex inputMutex_, outputMutex_; std::condition_variable inputCv_; std::vector<float> inputRing_, outputRing_; int inputRead_=0,inputWrite_=0,inputCount_=0,outputRead_=0,outputWrite_=0,outputCount_=0;
    int rate_=48000, outputMinBufferMs_=16, outputMaxBufferMs_=50;
    int inputBurstPackets_=8, outputBurstPackets_=8;
    int outputTransferFrames_=0, outputMinFrames_=1, outputPrerollFrames_=1, outputMaxPrerollFrames_=1;
    bool outputPrimed_=false, outputHasData_=false;
    uint64_t outputUnderruns_=0;
};
}
extern "C" usb_host_audio_t usb_host_audio_start(int fd, int rate, int in, int out, int minMs, int maxMs, int inputBurst, int outputBurst) { try { return new UsbHostAudio(fd, rate, in != 0, out != 0, minMs, maxMs, inputBurst, outputBurst); } catch (const std::exception &e) { USB_HOST_LOGE("USB Host audio start failed: %s", e.what()); return nullptr; } catch (...) { USB_HOST_LOGE("USB Host audio start failed"); return nullptr; } }
extern "C" int usb_host_audio_read(usb_host_audio_t a, float *d, int n) { return a ? static_cast<UsbHostAudio *>(a)->read(d, n) : 0; }
extern "C" int usb_host_audio_write(usb_host_audio_t a, const float *d, int n) { return a ? static_cast<UsbHostAudio *>(a)->write(d, n) : 0; }
extern "C" void usb_host_audio_configure_output_buffer(usb_host_audio_t a, int minMs, int maxMs) { if (a) static_cast<UsbHostAudio *>(a)->configureOutputBuffer(minMs, maxMs); }
extern "C" void usb_host_audio_stop(usb_host_audio_t a) { delete static_cast<UsbHostAudio *>(a); }
