#include "usb_host_bridge.h"
#include <android/log.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <vector>
#include "libuac.h"

#define USB_HOST_TAG "PulseForgeUsbHost"
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
    UsbHostAudio(int fd, int rate, int bitDepth, bool in, bool out, int outputMinBufferMs,
                 int outputMaxBufferMs, int inputBurstPackets, int outputBurstPackets)
        : rate_(rate), bitDepth_(bitDepth), inputBurstPackets_(std::clamp(inputBurstPackets, 1, 16)),
          outputBurstPackets_(std::clamp(outputBurstPackets, 1, 16)), inputRing_(kRingFrames * 2),
          outputRing_(kRingFrames * 2), outputMinBufferMs_(outputMinBufferMs),
          outputMaxBufferMs_(outputMaxBufferMs) {
        context_ = uac::uac_context::create();
        device_ = context_->wrap(fd);
        if (in) startInput(rate);
        if (out) startOutput(rate);
        if ((in && !inputStream_) || (out && !outputStream_)) throw std::runtime_error("requested USB audio route/format not found");
        USB_HOST_LOGI("USB Host audio started input=%d output=%d, requested=%d Hz/%d-bit, buffer=%d/%d/%d ms, burst=%d/%d",
                      inputStream_ != nullptr, outputStream_ != nullptr, rate_, bitDepth_,
                      outputMinBufferMs_.load(), outputMinBufferMs_.load() + (outputMaxBufferMs_.load() - outputMinBufferMs_.load()) / 2,
                      outputMaxBufferMs_.load(), inputBurstPackets_, outputBurstPackets_);
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
        uint32_t maxFrames = (uint32_t)((uint64_t)std::max(1, rate_) * (uint32_t)inputMaxBufferMs_.load(std::memory_order_relaxed) / 1000u);
        if (maxFrames < (uint32_t)frames) maxFrames = (uint32_t)frames;
        if (available > maxFrames) {
            inputRead_.store(write, std::memory_order_release);
            inputBufferClears_.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
        uint32_t n = std::min<uint32_t>((uint32_t)frames, available);
        uint32_t offset = read & kRingMask;
        uint32_t first = std::min(n, kRingFrames - offset);
        std::memcpy(dst, inputRing_.data() + (size_t)offset * 2u, (size_t)first * 2u * sizeof(float));
        if (first < n) std::memcpy(dst + (size_t)first * 2u, inputRing_.data(), (size_t)(n - first) * 2u * sizeof(float));
        inputRead_.store(read + n, std::memory_order_release);
        return (int)n;
    }

    int write(const float *src, int frames) {
        if (!src || frames <= 0) return 0;
        uint32_t write = outputWrite_.load(std::memory_order_relaxed);
        uint32_t read = outputRead_.load(std::memory_order_acquire);
        uint32_t queued = write - read;
        if (queued > kRingFrames || (uint32_t)frames > kRingFrames - queued) {
            outputRingOverruns_.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
        uint32_t offset = write & kRingMask, count = (uint32_t)frames;
        uint32_t first = std::min(count, kRingFrames - offset);
        std::memcpy(outputRing_.data() + (size_t)offset * 2u, src, (size_t)first * 2u * sizeof(float));
        if (first < count) std::memcpy(outputRing_.data(), src + (size_t)first * 2u, (size_t)(count - first) * 2u * sizeof(float));
        outputHasData_.store(true, std::memory_order_relaxed);
        outputWrite_.store(write + count, std::memory_order_release);
        return frames;
    }

    void configureOutputBuffer(int minBufferMs, int maxBufferMs) {
        outputMinBufferMs_.store(std::clamp(minBufferMs, 8, 200), std::memory_order_relaxed);
        outputMaxBufferMs_.store(std::clamp(maxBufferMs, 8, 500), std::memory_order_relaxed);
        int minMs = outputMinBufferMs_.load(), maxMs = outputMaxBufferMs_.load();
        if (maxMs < minMs) { maxMs = minMs; outputMaxBufferMs_.store(maxMs); }
        updateOutputBufferFrames();
        outputPrimed_.store(false, std::memory_order_relaxed);
        outputUnderruns_.store(0, std::memory_order_relaxed);
        USB_HOST_LOGI("USB output buffer configured min=%d, target=%u, max=%u frames", minMs, outputPrerollFrames_.load(), outputMaxPrerollFrames_.load());
    }

    void configureInputBuffer(int maxBufferMs) {
        inputMaxBufferMs_.store(std::clamp(maxBufferMs, 5, 200), std::memory_order_relaxed);
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
        uint32_t write=outputWrite_.load(std::memory_order_acquire),read=outputRead_.load(std::memory_order_relaxed),available=write-read,maxFrames=outputMaxPrerollFrames_.load(std::memory_order_acquire);
        if(available>kRingFrames||available>maxFrames){outputRead_.store(write,std::memory_order_release);outputPrimed_.store(false);outputBufferClears_.fetch_add(1);std::memset(data,0,len);update_max(outputCallbackMaxUs_,now_us()-begin);return;}
        if(!outputPrimed_.load(std::memory_order_relaxed)){uint32_t prefill=outputPrerollFrames_.load(std::memory_order_acquire);if(available<prefill){std::memset(data,0,len);update_max(outputCallbackMaxUs_,now_us()-begin);return;}outputPrimed_.store(true);}
        uint32_t take=std::min(count,available),offset=read&kRingMask;
        for(uint32_t i=0;i<take;++i){uint32_t p=(offset+i)&kRingMask;uint8_t*f=data+(size_t)i*stride;encode(f,outputRing_[2u*p],bytes);if(ch>1)encode(f+bytes,outputRing_[2u*p+1u],bytes);for(int c=2;c<ch;++c)encode(f+c*bytes,0.0f,bytes);}
        if(take<count){std::memset(data+(size_t)take*stride,0,(size_t)(count-take)*stride);outputUnderruns_.fetch_add(1);outputPrimed_.store(false);}
        outputRead_.store(read+take,std::memory_order_release);update_max(outputCallbackMaxUs_,now_us()-begin);
    }
    void startInput(int rate){
        auto routes=device_->get_device()->query_audio_routes(uac::UAC_TERMINAL_ANY,uac::UAC_TERMINAL_USB_STREAMING);
        if(routes.empty())return;
        const auto&si=device_->get_device()->get_stream_interface(routes.front().get());
        auto cfg=si.query_config_uncompressed(uac::UAC_FORMAT_DATA_PCM,2,(uint32_t)rate,(uint8_t)bitDepth_);
        if(!cfg){
            cfg=si.query_config_uncompressed(uac::UAC_FORMAT_DATA_PCM,1,(uint32_t)rate,(uint8_t)bitDepth_);
            if(cfg)USB_HOST_LOGI("USB input stereo format unavailable; falling back to mono");
        }
        if(!cfg)return;
        int bytes=cfg->bSubframeSize,ch=cfg->bChannelCount;
        inputStream_=device_->start_streaming(si,*cfg,[this,bytes,ch](uint8_t*d,uint n){onInput(d,n,bytes,ch);},inputBurstPackets_);
        inputSampleRate_=cfg->tSampleRate;inputBitResolution_=cfg->bBitResolution;inputChannels_=cfg->bChannelCount;
        USB_HOST_LOGI("USB input actual format=%u Hz/%u-bit/%u ch, subframe=%u B",inputSampleRate_,inputBitResolution_,inputChannels_,cfg->bSubframeSize);
    }
    void startOutput(int rate){
        auto routes=device_->get_device()->query_audio_routes(uac::UAC_TERMINAL_USB_STREAMING,uac::UAC_TERMINAL_ANY);
        if(routes.empty())return;
        const auto&si=device_->get_device()->get_stream_interface(routes.front().get());
        auto cfg=si.query_config_uncompressed(uac::UAC_FORMAT_DATA_PCM,2,(uint32_t)rate,(uint8_t)bitDepth_);
        if(!cfg){
            cfg=si.query_config_uncompressed(uac::UAC_FORMAT_DATA_PCM,1,(uint32_t)rate,(uint8_t)bitDepth_);
            if(cfg)USB_HOST_LOGI("USB output stereo format unavailable; falling back to mono");
        }
        if(!cfg)return;
        int bytes=cfg->bSubframeSize,ch=cfg->bChannelCount;
        outputTransferFrames_=(outputBurstPackets_*cfg->wMaxPacketSize)/std::max(1,bytes*ch);
        updateOutputBufferFrames();
        outputStream_=device_->start_streaming(si,*cfg,[this,bytes,ch](uint8_t*d,uint n){onOutput(d,n,bytes,ch);},outputBurstPackets_);
        outputSampleRate_=cfg->tSampleRate;outputBitResolution_=cfg->bBitResolution;outputChannels_=cfg->bChannelCount;
        USB_HOST_LOGI("USB output actual format=%u Hz/%u-bit/%u ch, subframe=%u B",outputSampleRate_,outputBitResolution_,outputChannels_,cfg->bSubframeSize);
    }
    void updateOutputBufferFrames(){if(outputTransferFrames_<=0)return;int minMs=std::clamp(outputMinBufferMs_.load(),8,200),maxMs=std::clamp(outputMaxBufferMs_.load(),8,500);if(maxMs<minMs)maxMs=minMs;uint32_t minFrames=std::max<uint32_t>((uint32_t)outputTransferFrames_,(uint32_t)((rate_*minMs+999)/1000)),maxFrames=std::max<uint32_t>(minFrames,(uint32_t)((rate_*maxMs+999)/1000));maxFrames=std::min<uint32_t>(maxFrames,kRingFrames);outputMinFrames_.store(minFrames);outputPrerollFrames_.store(minFrames+(maxFrames-minFrames)/2);outputMaxPrerollFrames_.store(maxFrames);}
    std::shared_ptr<uac::uac_context> context_;std::shared_ptr<uac::uac_device_handle> device_;std::shared_ptr<uac::uac_stream_handle> inputStream_,outputStream_;std::atomic<bool> stopping_{false};int rate_=48000,bitDepth_=16,inputBurstPackets_=8,outputBurstPackets_=8,outputTransferFrames_=0;std::vector<float> inputRing_,outputRing_;std::atomic<uint32_t> inputRead_{0},inputWrite_{0},outputRead_{0},outputWrite_{0};std::atomic<int> inputMaxBufferMs_{20},outputMinBufferMs_,outputMaxBufferMs_;std::atomic<uint32_t> outputMinFrames_{1},outputPrerollFrames_{1},outputMaxPrerollFrames_{1};std::atomic<bool> outputPrimed_{false},outputHasData_{false};std::atomic<uint64_t> inputRingOverruns_{0},inputBufferClears_{0},outputRingOverruns_{0},outputUnderruns_{0},outputBufferClears_{0},inputCallbackMaxUs_{0},outputCallbackMaxUs_{0};uint32_t inputSampleRate_=0,outputSampleRate_=0;uint8_t inputBitResolution_=0,inputChannels_=0,outputBitResolution_=0,outputChannels_=0;
};
}
extern "C" usb_host_audio_t usb_host_audio_start(int fd,int rate,int bitDepth,int in,int out,int minMs,int maxMs,int inputBurst,int outputBurst){try{return new UsbHostAudio(fd,rate,bitDepth,in!=0,out!=0,minMs,maxMs,inputBurst,outputBurst);}catch(const std::exception&e){USB_HOST_LOGE("USB Host audio start failed: %s",e.what());return nullptr;}catch(...){USB_HOST_LOGE("USB Host audio start failed");return nullptr;}}
extern "C" int usb_host_audio_read(usb_host_audio_t a,float*d,int n){return a?static_cast<UsbHostAudio*>(a)->read(d,n):0;}
extern "C" int usb_host_audio_write(usb_host_audio_t a,const float*d,int n){return a?static_cast<UsbHostAudio*>(a)->write(d,n):0;}
extern "C" void usb_host_audio_configure_output_buffer(usb_host_audio_t a,int minMs,int maxMs){if(a)static_cast<UsbHostAudio*>(a)->configureOutputBuffer(minMs,maxMs);}
extern "C" void usb_host_audio_configure_input_buffer(usb_host_audio_t a,int maxMs){if(a)static_cast<UsbHostAudio*>(a)->configureInputBuffer(maxMs);}
extern "C" void usb_host_audio_get_stats(usb_host_audio_t a,usb_host_audio_stats_t*stats){if(!stats)return;*stats=a?static_cast<UsbHostAudio*>(a)->stats():usb_host_audio_stats_t{};}
extern "C" void usb_host_audio_stop(usb_host_audio_t a){delete static_cast<UsbHostAudio*>(a);}
