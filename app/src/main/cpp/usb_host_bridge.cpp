#include "usb_host_bridge.h"
#include <android/log.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>
#include "libuac.h"

#define USB_HOST_TAG "AudioProcessUsbHost"
#define USB_HOST_LOGE(...) __android_log_print(ANDROID_LOG_ERROR, USB_HOST_TAG, __VA_ARGS__)
#define USB_HOST_LOGI(...) __android_log_print(ANDROID_LOG_INFO, USB_HOST_TAG, __VA_ARGS__)

namespace {
static uint64_t now_us() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
static void update_max(std::atomic<uint64_t> &target, uint64_t value) {
    uint64_t old = target.load(std::memory_order_relaxed);
    while (old < value && !target.compare_exchange_weak(old, value,
            std::memory_order_relaxed, std::memory_order_relaxed)) {}
}

class UsbHostAudio {
public:
    UsbHostAudio(int fd, int processingRate, int inputRate, int inputBitDepth,
                 int outputRate, int outputBitDepth, bool in, bool out, int outputMaxBufferMs,
                 int processingFrames, int inputBurstPackets, int outputBurstPackets)
        : processingRate_(processingRate), inputRate_(inputRate), inputBitDepth_(inputBitDepth),
          outputRate_(outputRate), outputBitDepth_(outputBitDepth),
          inputBurstPackets_(std::clamp(inputBurstPackets, 1, 128)),
          outputBurstPackets_(std::clamp(outputBurstPackets, 1, 128)), inputRing_(kRingFrames * 2),
          outputRing_(kRingFrames * 2) {
        processingFrames_ = std::max(1, processingFrames);
        outputMaxBufferMs_.store(std::clamp(outputMaxBufferMs, 5, 200), std::memory_order_relaxed);
        context_ = uac::uac_context::create();
        device_ = context_->wrap(fd);
        USB_HOST_LOGI("USB device opened vid=0x%04x pid=0x%04x name=%s",
                      device_->get_device()->get_vid(), device_->get_device()->get_pid(),
                      device_->get_name().c_str());
        if (in) startInput(inputRate_);
        if (out) startOutput(outputRate_);
        if ((in && !inputStream_) || (out && !outputStream_)) throw std::runtime_error("requested USB audio route/format not found");
        USB_HOST_LOGI("USB Host audio started input=%d output=%d, processing=%d Hz, input=%d/%d-bit output=%d/%d-bit, output buffer target=%u/max=%u frames, burst=%d/%d",
                      inputStream_ != nullptr, outputStream_ != nullptr, processingRate_,
                      inputRate_, inputBitDepth_, outputRate_, outputBitDepth_,
                      outputPrerollFrames_.load(), outputMaxPrerollFrames_.load(),
                      inputBurstPackets_, outputBurstPackets_);
    }
    ~UsbHostAudio() {
        stopping_.store(true, std::memory_order_release);
        outputStream_.reset(); inputStream_.reset(); device_.reset(); context_.reset();
    }

    int read(float *dst, int frames) {
        if (!dst || frames <= 0) return 0;
        uint32_t write = inputWrite_.load(std::memory_order_acquire);
        uint32_t read = inputRead_.load(std::memory_order_relaxed);
        uint32_t available = write - read;
        if (available > kRingFrames) return 0;
        const int sourceRate = inputSampleRate_ > 0 ? (int)inputSampleRate_ : inputRate_;
        uint32_t maxFrames = (uint32_t)((uint64_t)std::max(1, sourceRate) * (uint32_t)inputMaxBufferMs_.load(std::memory_order_relaxed) / 1000u);
        if (maxFrames < (uint32_t)frames) maxFrames = (uint32_t)frames;
        if (available > maxFrames) {
            inputRead_.store(write, std::memory_order_release);
            inputBufferClears_.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
        const double step = (double)sourceRate / (double)std::max(1, processingRate_);
        int produced = 0;
        while (produced < frames) {
            const double position = inputResamplePhase_ + (double)produced * step;
            const uint32_t index = (uint32_t)position;
            if (index + 1u >= available) break;
            const float frac = (float)(position - (double)index);
            const uint32_t p0 = (read + index) & kRingMask;
            const uint32_t p1 = (read + index + 1u) & kRingMask;
            dst[produced * 2] = inputRing_[p0 * 2] + (inputRing_[p1 * 2] - inputRing_[p0 * 2]) * frac;
            dst[produced * 2 + 1] = inputRing_[p0 * 2 + 1] + (inputRing_[p1 * 2 + 1] - inputRing_[p0 * 2 + 1]) * frac;
            ++produced;
        }
        const double consumedPosition = inputResamplePhase_ + (double)produced * step;
        const uint32_t consumed = (uint32_t)consumedPosition;
        inputResamplePhase_ = consumedPosition - (double)consumed;
        if (consumed > 0) inputRead_.store(read + consumed, std::memory_order_release);
        return produced;
    }

    int write(const float *src, int frames) {
        if (!src || frames <= 0) return 0;
        if ((uint32_t)frames > kRingFrames) {
            outputRingOverruns_.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
        uint32_t write;
        while (!stopping_.load(std::memory_order_acquire)) {
            write = outputWrite_.load(std::memory_order_relaxed);
            uint32_t read = outputRead_.load(std::memory_order_acquire);
            uint32_t queued = write - read;
            uint32_t limit = outputMaxPrerollFrames_.load(std::memory_order_acquire);
            if (limit == 0 || limit > kRingFrames) limit = kRingFrames;
            if (queued <= limit && (uint32_t)frames <= limit - queued) break;
            std::this_thread::sleep_for(std::chrono::microseconds(250));
        }
        if (stopping_.load(std::memory_order_acquire)) return 0;
        uint32_t offset = write & kRingMask, count = (uint32_t)frames;
        uint32_t first = std::min(count, kRingFrames - offset);
        std::memcpy(outputRing_.data() + (size_t)offset * 2u, src, (size_t)first * 2u * sizeof(float));
        if (first < count) std::memcpy(outputRing_.data(), src + (size_t)first * 2u, (size_t)(count - first) * 2u * sizeof(float));
        outputHasData_.store(true, std::memory_order_relaxed);
        outputWrite_.store(write + count, std::memory_order_release);
        return frames;
    }

    void configureOutputBuffer(int maxBufferMs) {
        outputMaxBufferMs_.store(std::clamp(maxBufferMs, 5, 200), std::memory_order_relaxed);
        updateOutputBufferFrames();
        outputPrimed_.store(false, std::memory_order_relaxed);
        outputUnderruns_.store(0, std::memory_order_relaxed);
        USB_HOST_LOGI("USB output buffer configured target=%u/max=%u frames", outputPrerollFrames_.load(), outputMaxPrerollFrames_.load());
    }

    void configureInputBuffer(int maxBufferMs) {
        inputMaxBufferMs_.store(std::clamp(maxBufferMs, 5, 200), std::memory_order_relaxed);
    }

    bool setVolumePercent(int percent) {
        if (!outputRoute_) return false;
        if (!volumeRangeValid_) {
            if (!device_->get_feature_master_volume_range(*outputRoute_, &volumeMin_, &volumeMax_, &volumeRes_)) {
                USB_HOST_LOGE("USB output volume control is not available");
                return false;
            }
            volumeRangeValid_ = true;
            USB_HOST_LOGI("USB output volume range min=%d max=%d res=%d (1/256 dB)",
                          volumeMin_, volumeMax_, volumeRes_);
        }
        const int p = std::clamp(percent, 0, 100);
        // UAC Volume reserves 0x8000 as the mandatory digital-silence code;
        // it must not be confused with the reported MIN attribute.
        int64_t value = -32768;
        if (p > 0) {
            const int64_t span = (int64_t)volumeMax_ - (int64_t)volumeMin_;
            value = (int64_t)volumeMin_ + (span * p) / 100;
            if (volumeRes_ > 0) value = volumeMin_ + ((value - volumeMin_ + volumeRes_ / 2) / volumeRes_) * volumeRes_;
            value = std::clamp<int64_t>(value, volumeMin_, volumeMax_);
        }
        const int32_t raw = static_cast<int32_t>(value);
        if (volumeRawValid_ && raw == volumeRaw_) return true;
        const bool ok = device_->set_feature_master_volume(*outputRoute_, raw);
        if (!ok) USB_HOST_LOGE("USB output volume SET_CUR failed percent=%d", p);
        else {
            volumeRaw_ = raw;
            volumeRawValid_ = true;
            USB_HOST_LOGI("USB output volume SET_CUR success percent=%d raw=%d", p, raw);
        }
        return ok;
    }

    usb_host_audio_stats_t stats() const {
        usb_host_audio_stats_t result{};
        if (inputStream_) { auto s = inputStream_->get_streaming_stats(); result.input_packet_errors=s.packet_errors; result.input_empty_packets=s.empty_packets; result.input_transfer_errors=s.transfer_errors; }
        if (outputStream_) result.output_transfer_errors = outputStream_->get_streaming_stats().transfer_errors;
        result.input_ring_overruns=inputRingOverruns_.load(); result.input_buffer_clears=inputBufferClears_.load(); result.output_low_water_events=outputUnderruns_.load();
        result.input_callback_max_us=inputCallbackMaxUs_.load(); result.output_callback_max_us=outputCallbackMaxUs_.load(); result.output_ring_overruns=outputRingOverruns_.load();
        result.input_sample_rate=inputSampleRate_; result.input_bit_resolution=inputBitResolution_; result.input_channels=inputChannels_;
        result.output_sample_rate=outputSampleRate_; result.output_bit_resolution=outputBitResolution_; result.output_channels=outputChannels_;
        return result;
    }

private:
    static constexpr uint32_t kRingFrames=131072u, kRingMask=kRingFrames-1u;
    static float decode(const uint8_t *s, int n) {
        if (n==2) return (float)(int16_t)(s[0]|(s[1]<<8))/32768.0f;
        if (n==3) { int32_t v=s[0]|(s[1]<<8)|(s[2]<<16); if(v&0x00800000)v|=~0x00ffffff; return (float)v/8388608.0f; }
        if (n==4) { int32_t v=(int32_t)(s[0]|(s[1]<<8)|(s[2]<<16)|(s[3]<<24)); return (float)v/2147483648.0f; }
        return 0.0f;
    }
    static void encode(uint8_t *s, float x, int n) {
        x=std::max(-1.0f,std::min(1.0f,x));
        if(n==2){int32_t v=(int32_t)(x*32767.0f);s[0]=(uint8_t)v;s[1]=(uint8_t)(v>>8);}
        else if(n==3){int32_t v=(int32_t)(x*8388607.0f);s[0]=(uint8_t)v;s[1]=(uint8_t)(v>>8);s[2]=(uint8_t)(v>>16);}
        else if(n==4){int64_t v=(int64_t)(x*2147483647.0f);s[0]=(uint8_t)v;s[1]=(uint8_t)(v>>8);s[2]=(uint8_t)(v>>16);s[3]=(uint8_t)(v>>24);}
        else std::memset(s,0,(size_t)n);
    }
    void onInput(uint8_t *data,uint len,int bytes,int ch){
        uint64_t begin=now_us(); if(stopping_.load(std::memory_order_acquire))return; int stride=bytes*ch; if(stride<=0)return;
        uint32_t count=(uint32_t)(len/(uint)stride),write=inputWrite_.load(std::memory_order_relaxed),read=inputRead_.load(std::memory_order_acquire),queued=write-read;
        if(queued>kRingFrames||count>kRingFrames-queued){inputRingOverruns_.fetch_add(1);update_max(inputCallbackMaxUs_,now_us()-begin);return;}
        uint32_t offset=write&kRingMask;
        for(uint32_t i=0;i<count;++i){const uint8_t*f=data+(size_t)i*stride;uint32_t p=(offset+i)&kRingMask;inputRing_[2u*p]=decode(f,bytes);inputRing_[2u*p+1u]=ch>1?decode(f+bytes,bytes):inputRing_[2u*p];}
        inputWrite_.store(write+count,std::memory_order_release);update_max(inputCallbackMaxUs_,now_us()-begin);
    }
    void onOutput(uint8_t *data,uint len,int bytes,int ch){
        uint64_t begin=now_us(); if(stopping_.load(std::memory_order_acquire)){std::memset(data,0,len);return;} int stride=bytes*ch;if(stride<=0)return;uint32_t count=(uint32_t)(len/(uint)stride);
        uint32_t write=outputWrite_.load(std::memory_order_acquire),read=outputRead_.load(std::memory_order_relaxed),available=write-read;
        const double step = (double)std::max(1, processingRate_) /
                            (double)std::max(1u, outputSampleRate_ > 0 ? outputSampleRate_ : (uint32_t)outputRate_);
        const double endPosition = outputResamplePhase_ + (double)count * step;
        const uint32_t sourceNeeded = (uint32_t)endPosition;
        if(available>kRingFrames){outputRead_.store(write,std::memory_order_release);outputPrimed_.store(false);outputBufferClears_.fetch_add(1);std::memset(data,0,len);update_max(outputCallbackMaxUs_,now_us()-begin);return;}
        if(!outputPrimed_.load(std::memory_order_relaxed)){uint32_t prefill=outputPrerollFrames_.load(std::memory_order_acquire);if(available<prefill){std::memset(data,0,len);update_max(outputCallbackMaxUs_,now_us()-begin);return;}outputPrimed_.store(true);}
        const uint32_t lastIndex = count > 0
                ? (uint32_t)(outputResamplePhase_ + (double)(count - 1u) * step) : 0u;
        if (available < sourceNeeded || available <= lastIndex + 1u) {
            std::memset(data,0,len); outputUnderruns_.fetch_add(1); outputPrimed_.store(false);
            update_max(outputCallbackMaxUs_,now_us()-begin); return;
        }
        uint32_t offset=read&kRingMask;
        for(uint32_t i=0;i<count;++i){
            const double position = outputResamplePhase_ + (double)i * step;
            const uint32_t sourceOffset = (uint32_t)position;
            const float frac = (float)(position - (double)sourceOffset);
            const uint32_t p0=(offset+sourceOffset)&kRingMask;
            const uint32_t p1=(offset+sourceOffset+1u)&kRingMask;
            const float left = outputRing_[2u*p0] + (outputRing_[2u*p1] - outputRing_[2u*p0]) * frac;
            const float right = outputRing_[2u*p0+1u] + (outputRing_[2u*p1+1u] - outputRing_[2u*p0+1u]) * frac;
            uint8_t*f=data+(size_t)i*stride;encode(f,left,bytes);if(ch>1)encode(f+bytes,right,bytes);for(int c=2;c<ch;++c)encode(f+c*bytes,0.0f,bytes);
        }
        const uint32_t consumed = (uint32_t)endPosition;
        outputResamplePhase_=endPosition-(double)consumed;
        outputRead_.store(read+sourceNeeded,std::memory_order_release);update_max(outputCallbackMaxUs_,now_us()-begin);
    }
    void startInput(int rate){
        auto routes=device_->get_device()->query_audio_routes(uac::UAC_TERMINAL_ANY,uac::UAC_TERMINAL_USB_STREAMING);
        if(routes.empty()){ USB_HOST_LOGE("USB input: no audio route found"); return; }
        const auto&si=device_->get_device()->get_stream_interface(routes.front().get());
        auto cfg=findCompatibleConfig(si, 2, rate, inputBitDepth_);
        if(!cfg) cfg=findCompatibleConfig(si, 1, rate, inputBitDepth_);
        if(!cfg) { USB_HOST_LOGE("USB input: no compatible PCM format requested=%d Hz/%d-bit", rate, inputBitDepth_); return; }
        if(!cfg)return;
        int bytes=cfg->bSubframeSize,ch=cfg->bChannelCount;
        inputStream_=device_->start_streaming(si,*cfg,[this,bytes,ch](uint8_t*d,uint n){onInput(d,n,bytes,ch);},inputBurstPackets_);
        inputSampleRate_=cfg->tSampleRate;inputBitResolution_=cfg->bBitResolution;inputChannels_=cfg->bChannelCount;
        USB_HOST_LOGI("USB input actual format=%u Hz/%u-bit/%u ch, subframe=%u B",inputSampleRate_,inputBitResolution_,inputChannels_,cfg->bSubframeSize);
    }
    void startOutput(int rate){
        auto routes=device_->get_device()->query_audio_routes(uac::UAC_TERMINAL_USB_STREAMING,uac::UAC_TERMINAL_ANY);
        if(routes.empty()){ USB_HOST_LOGE("USB output: no audio route found"); return; }
        outputRoute_ = &routes.front().get();
        const auto&si=device_->get_device()->get_stream_interface(routes.front().get());
        auto cfg=findCompatibleConfig(si, 2, rate, outputBitDepth_);
        if(!cfg) cfg=findCompatibleConfig(si, 1, rate, outputBitDepth_);
        if(!cfg) { USB_HOST_LOGE("USB output: no compatible PCM format requested=%d Hz/%d-bit", rate, outputBitDepth_); return; }
        int bytes=cfg->bSubframeSize,ch=cfg->bChannelCount;
        outputSampleRate_=cfg->tSampleRate;
        outputBitResolution_=cfg->bBitResolution;
        outputChannels_=cfg->bChannelCount;
        const uint32_t packetIntervalUs = cfg->highSpeed
                ? (125u << std::max(0, (int)cfg->bInterval - 1))
                : (1000u << std::max(0, (int)cfg->bInterval - 1));
        const uint32_t packetFrames = packetIntervalUs > 0
                ? (uint32_t)(((uint64_t)(outputSampleRate_ > 0 ? outputSampleRate_ : (uint32_t)outputRate_) * packetIntervalUs + 999999u) / 1000000u) : 1u;
        outputTransferFrames_=outputBurstPackets_*packetFrames;
        outputResamplePhase_ = 0;
        USB_HOST_LOGI("USB output timing packetFrames<=%u intervalUs=%u endpointCapacity=%u bytes",
                      packetFrames, packetIntervalUs, cfg->wMaxPacketSize);
        updateOutputBufferFrames();
        outputStream_=device_->start_streaming(si,*cfg,[this,bytes,ch](uint8_t*d,uint n){onOutput(d,n,bytes,ch);},outputBurstPackets_);
        USB_HOST_LOGI("USB output actual format=%u Hz/%u-bit/%u ch, subframe=%u B",outputSampleRate_,outputBitResolution_,outputChannels_,cfg->bSubframeSize);
    }
    static std::unique_ptr<const uac::uac_audio_config_uncompressed> findCompatibleConfig(
            const uac::uac_stream_if& si, int channels, int requestedRate, int requestedBits) {
        std::vector<uint32_t> supportedRates = si.get_sample_rates(uac::UAC_FORMAT_DATA_PCM);
        std::vector<uint32_t> rates{(uint32_t)requestedRate};
        const int bits[] = { requestedBits, 24, 32, 16 };
        for (uint32_t r : rates) for (int b : bits) {
            if (r <= 0 || b <= 0) continue;
            auto cfg = si.query_config_uncompressed(uac::UAC_FORMAT_DATA_PCM,
                                                     (uint8_t)channels, r, (uint8_t)b);
            if (!cfg && channels == 2)
                cfg = si.query_config_uncompressed(uac::UAC_FORMAT_DATA_PCM, 0, r, (uint8_t)b);
            if (cfg) {
                if (b != requestedBits)
                    USB_HOST_LOGI("USB format fallback: requested=%d/%d, selected=%d/%d ch=%d",
                                  requestedRate, requestedBits, r, b, channels);
                return cfg;
            }
        }
        if (!supportedRates.empty()) {
            std::sort(supportedRates.begin(), supportedRates.end(), [requestedRate](uint32_t a, uint32_t b) {
                const uint32_t da = a > (uint32_t)requestedRate ? a - (uint32_t)requestedRate : (uint32_t)requestedRate - a;
                const uint32_t db = b > (uint32_t)requestedRate ? b - (uint32_t)requestedRate : (uint32_t)requestedRate - b;
                return da < db;
            });
            for (uint32_t r : supportedRates) for (int b : bits) {
                auto cfg = si.query_config_uncompressed(uac::UAC_FORMAT_DATA_PCM,
                                                         (uint8_t)channels, r, (uint8_t)b);
                if (!cfg && channels == 2)
                    cfg = si.query_config_uncompressed(uac::UAC_FORMAT_DATA_PCM, 0, r, (uint8_t)b);
                if (cfg) {
                    USB_HOST_LOGI("USB format fallback: requested=%d/%d, selected=%u/%d ch=%d (resampling enabled)",
                                  requestedRate, requestedBits, r, b, channels);
                    return cfg;
                }
            }
            USB_HOST_LOGE("USB format unsupported at %d Hz; device reports %zu sample rates",
                          requestedRate, supportedRates.size());
            for (uint32_t supported : supportedRates)
                USB_HOST_LOGI("USB supported sample rate: %u Hz", supported);
        }
        return nullptr;
    }
    void updateOutputBufferFrames(){if(outputTransferFrames_<=0)return;int maxMs=std::clamp(outputMaxBufferMs_.load(),5,200);uint32_t maxFrames=std::max<uint32_t>({(uint32_t)outputTransferFrames_,(uint32_t)processingFrames_,(uint32_t)((processingRate_*maxMs+999)/1000)});maxFrames=std::min<uint32_t>(maxFrames,kRingFrames);uint32_t prefill=std::max<uint32_t>({maxFrames/2u,(uint32_t)outputTransferFrames_,(uint32_t)processingFrames_});if(prefill>maxFrames)prefill=maxFrames;outputPrerollFrames_.store(prefill);outputMaxPrerollFrames_.store(maxFrames);}
    std::shared_ptr<uac::uac_context> context_;std::shared_ptr<uac::uac_device_handle> device_;std::shared_ptr<uac::uac_stream_handle> inputStream_,outputStream_;const uac::uac_audio_route *outputRoute_=nullptr;bool volumeRangeValid_=false,volumeRawValid_=false;int32_t volumeMin_=0,volumeMax_=0,volumeRes_=0,volumeRaw_=0;std::atomic<bool> stopping_{false};int processingRate_=48000,inputRate_=48000,inputBitDepth_=16,outputRate_=48000,outputBitDepth_=16,inputBurstPackets_=8,outputBurstPackets_=8,outputTransferFrames_=0,processingFrames_=256;double inputResamplePhase_=0.0,outputResamplePhase_=0.0;std::vector<float> inputRing_,outputRing_;std::atomic<uint32_t> inputRead_{0},inputWrite_{0},outputRead_{0},outputWrite_{0};std::atomic<int> inputMaxBufferMs_{20},outputMaxBufferMs_{50};std::atomic<uint32_t> outputPrerollFrames_{1},outputMaxPrerollFrames_{1};std::atomic<bool> outputPrimed_{false},outputHasData_{false};std::atomic<uint64_t> inputRingOverruns_{0},inputBufferClears_{0},outputRingOverruns_{0},outputUnderruns_{0},outputBufferClears_{0},inputCallbackMaxUs_{0},outputCallbackMaxUs_{0};uint32_t inputSampleRate_=0,outputSampleRate_=0;uint8_t inputBitResolution_=0,inputChannels_=0,outputBitResolution_=0,outputChannels_=0;
};
}
extern "C" usb_host_audio_t usb_host_audio_start(int fd,int processingRate,int inputRate,int inputBitDepth,int outputRate,int outputBitDepth,int in,int out,int maxMs,int processingFrames,int inputBurst,int outputBurst){try{return new UsbHostAudio(fd,processingRate,inputRate,inputBitDepth,outputRate,outputBitDepth,in!=0,out!=0,maxMs,processingFrames,inputBurst,outputBurst);}catch(const std::exception&e){USB_HOST_LOGE("USB Host audio start failed: %s",e.what());return nullptr;}catch(...){USB_HOST_LOGE("USB Host audio start failed");return nullptr;}}
extern "C" int usb_host_audio_read(usb_host_audio_t a,float*d,int n){return a?static_cast<UsbHostAudio*>(a)->read(d,n):0;}
extern "C" int usb_host_audio_write(usb_host_audio_t a,const float*d,int n){return a?static_cast<UsbHostAudio*>(a)->write(d,n):0;}
extern "C" void usb_host_audio_configure_output_buffer(usb_host_audio_t a,int maxMs){if(a)static_cast<UsbHostAudio*>(a)->configureOutputBuffer(maxMs);}
extern "C" void usb_host_audio_configure_input_buffer(usb_host_audio_t a,int maxMs){if(a)static_cast<UsbHostAudio*>(a)->configureInputBuffer(maxMs);}
extern "C" int usb_host_audio_set_volume(usb_host_audio_t a,int percent){return a&&static_cast<UsbHostAudio*>(a)->setVolumePercent(percent)?1:0;}
extern "C" void usb_host_audio_get_stats(usb_host_audio_t a,usb_host_audio_stats_t*stats){if(!stats)return;*stats=a?static_cast<UsbHostAudio*>(a)->stats():usb_host_audio_stats_t{};}
extern "C" void usb_host_audio_stop(usb_host_audio_t a){delete static_cast<UsbHostAudio*>(a);}
