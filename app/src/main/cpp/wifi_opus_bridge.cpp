#include "wifi_opus_bridge.h"

#include <algorithm>
#include <memory>
#include <vector>

#include <opus.h>

namespace {
class WifiOpusCodec {
public:
    WifiOpusCodec(int sampleRate, int bitrate, int application, int frameSize)
        : sampleRate_(sampleRate), frameSize_(frameSize) {
        int error = OPUS_OK;
        encoder_ = opus_encoder_create(sampleRate_, 2, application, &error);
        if (!encoder_ || error != OPUS_OK) return;
        if (opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(bitrate)) != OPUS_OK ||
            opus_encoder_ctl(encoder_, OPUS_SET_COMPLEXITY(5)) != OPUS_OK ||
            opus_encoder_ctl(encoder_, OPUS_SET_SIGNAL(OPUS_AUTO)) != OPUS_OK) return;
        decoder_ = opus_decoder_create(sampleRate_, 2, &error);
        if (!decoder_ || error != OPUS_OK) return;
        pcm_.resize(static_cast<size_t>(frameSize_) * 2u);
        valid_ = true;
    }

    ~WifiOpusCodec() {
        if (decoder_) opus_decoder_destroy(decoder_);
        if (encoder_) opus_encoder_destroy(encoder_);
    }

    bool valid() const { return valid_; }
    int frameLength() const { return valid_ ? frameSize_ : 0; }
    int sampleRate() const { return sampleRate_; }

    int encode(const float *stereo, int frames, uint8_t *output, int capacity) {
        if (!valid_ || !stereo || frames != frameSize_ || !output || capacity <= 0) return -1;
        return opus_encode_float(encoder_, stereo, frames, output, capacity);
    }

    int decode(const uint8_t *input, int inputSize, float *stereo, int capacityFrames) {
        if (!valid_ || !input || inputSize <= 0 || !stereo || capacityFrames <= 0) return -1;
        if (static_cast<int>(pcm_.size() / 2u) < capacityFrames)
            pcm_.resize(static_cast<size_t>(capacityFrames) * 2u);
        int frames = opus_decode_float(decoder_, input, inputSize, pcm_.data(), capacityFrames, 0);
        if (frames < 0) return frames;
        std::copy(pcm_.begin(), pcm_.begin() + static_cast<size_t>(frames) * 2u, stereo);
        return frames;
    }

private:
    OpusEncoder *encoder_ = nullptr;
    OpusDecoder *decoder_ = nullptr;
    int sampleRate_;
    int frameSize_;
    std::vector<float> pcm_;
    bool valid_ = false;
};
}

extern "C" wifi_opus_t wifi_opus_create(int sample_rate, int bitrate_bps, int application,
                                         int frame_size) {
    std::unique_ptr<WifiOpusCodec> codec(new WifiOpusCodec(sample_rate, bitrate_bps,
                                                            application, frame_size));
    return codec->valid() ? codec.release() : nullptr;
}

extern "C" void wifi_opus_destroy(wifi_opus_t codec) {
    delete static_cast<WifiOpusCodec *>(codec);
}

extern "C" int wifi_opus_frame_length(wifi_opus_t codec) {
    return codec ? static_cast<WifiOpusCodec *>(codec)->frameLength() : 0;
}

extern "C" int wifi_opus_sample_rate(wifi_opus_t codec) {
    return codec ? static_cast<WifiOpusCodec *>(codec)->sampleRate() : 0;
}

extern "C" int wifi_opus_encode(wifi_opus_t codec, const float *stereo, int frames,
                                uint8_t *output, int output_capacity) {
    return codec ? static_cast<WifiOpusCodec *>(codec)->encode(stereo, frames, output, output_capacity) : -1;
}

extern "C" int wifi_opus_decode(wifi_opus_t codec, const uint8_t *input, int input_size,
                                float *stereo, int output_capacity_frames) {
    return codec ? static_cast<WifiOpusCodec *>(codec)->decode(input, input_size, stereo, output_capacity_frames) : -1;
}
