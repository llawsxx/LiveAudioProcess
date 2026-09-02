#include "wifi_aac_bridge.h"

#include <algorithm>
#include <memory>
#include <vector>

#include "aacdecoder_lib.h"
#include "aacenc_lib.h"

namespace {
class WifiAacCodec {
public:
    WifiAacCodec(int sampleRate, int bitrate) {
        if (aacEncOpen(&encoder_, 0, 2) != AACENC_OK) return;
        if (aacEncoder_SetParam(encoder_, AACENC_AOT, AOT_AAC_LC) != AACENC_OK ||
            aacEncoder_SetParam(encoder_, AACENC_SAMPLERATE, sampleRate) != AACENC_OK ||
            aacEncoder_SetParam(encoder_, AACENC_CHANNELMODE, MODE_2) != AACENC_OK ||
            aacEncoder_SetParam(encoder_, AACENC_CHANNELORDER, 1) != AACENC_OK ||
            aacEncoder_SetParam(encoder_, AACENC_BITRATE, bitrate) != AACENC_OK ||
            aacEncoder_SetParam(encoder_, AACENC_TRANSMUX, TT_MP4_ADTS) != AACENC_OK ||
            aacEncoder_SetParam(encoder_, AACENC_AFTERBURNER, 1) != AACENC_OK ||
            aacEncEncode(encoder_, nullptr, nullptr, nullptr, nullptr) != AACENC_OK ||
            aacEncInfo(encoder_, &encoderInfo_) != AACENC_OK) return;

        decoder_ = aacDecoder_Open(TT_MP4_ADTS, 1);
        if (!decoder_) return;
        aacDecoder_SetParam(decoder_, AAC_CONCEAL_METHOD, 1);
        aacDecoder_SetParam(decoder_, AAC_PCM_LIMITER_ENABLE, 0);
        pcmInput_.resize((size_t)encoderInfo_.frameLength * 2u);
        pcmOutput_.resize((size_t)encoderInfo_.frameLength * 2u);
        valid_ = true;
    }

    ~WifiAacCodec() {
        if (decoder_) aacDecoder_Close(decoder_);
        if (encoder_) aacEncClose(&encoder_);
    }

    bool valid() const { return valid_; }
    int frameLength() const { return valid_ ? (int)encoderInfo_.frameLength : 0; }

    int encode(const float *stereo, int frames, uint8_t *output, int capacity) {
        if (!valid_ || !stereo || !output || frames != frameLength() || capacity <= 0) return -1;
        for (size_t i = 0; i < pcmInput_.size(); ++i) {
            float value = std::clamp(stereo[i], -1.0f, 1.0f);
            pcmInput_[i] = (INT_PCM)(value * 32767.0f);
        }

        void *inputPtr = pcmInput_.data();
        int inputId = IN_AUDIO_DATA;
        int inputSize = (int)(pcmInput_.size() * sizeof(INT_PCM));
        int inputElementSize = sizeof(INT_PCM);
        AACENC_BufDesc inputDesc{1, &inputPtr, &inputId, &inputSize, &inputElementSize};
        AACENC_InArgs inputArgs{};
        inputArgs.numInSamples = (int)pcmInput_.size();

        void *outputPtr = output;
        int outputId = OUT_BITSTREAM_DATA;
        int outputSize = capacity;
        int outputElementSize = 1;
        AACENC_BufDesc outputDesc{1, &outputPtr, &outputId, &outputSize, &outputElementSize};
        AACENC_OutArgs outputArgs{};
        AACENC_ERROR result = aacEncEncode(encoder_, &inputDesc, &outputDesc, &inputArgs, &outputArgs);
        return result == AACENC_OK ? outputArgs.numOutBytes : -1;
    }

    int decode(const uint8_t *input, int inputSize, float *stereo, int capacityFrames) {
        if (!valid_ || !input || inputSize <= 0 || !stereo || capacityFrames <= 0) return -1;
        UCHAR *inputPtr = const_cast<UCHAR *>(input);
        UINT bufferSize = (UINT)inputSize;
        UINT validBytes = bufferSize;
        if (aacDecoder_Fill(decoder_, &inputPtr, &bufferSize, &validBytes) != AAC_DEC_OK) return -1;
        AAC_DECODER_ERROR result = aacDecoder_DecodeFrame(
                decoder_, pcmOutput_.data(), (INT)pcmOutput_.size(), 0);
        if (result != AAC_DEC_OK) return -1;
        CStreamInfo *info = aacDecoder_GetStreamInfo(decoder_);
        if (!info || info->frameSize <= 0 || info->numChannels <= 0) return -1;
        int frames = std::min(info->frameSize, capacityFrames);
        int channels = info->numChannels;
        for (int frame = 0; frame < frames; ++frame) {
            float left = (float)pcmOutput_[(size_t)frame * channels] / 32768.0f;
            float right = channels > 1
                    ? (float)pcmOutput_[(size_t)frame * channels + 1u] / 32768.0f
                    : left;
            stereo[(size_t)frame * 2u] = left;
            stereo[(size_t)frame * 2u + 1u] = right;
        }
        return frames;
    }

private:
    HANDLE_AACENCODER encoder_ = nullptr;
    HANDLE_AACDECODER decoder_ = nullptr;
    AACENC_InfoStruct encoderInfo_{};
    std::vector<INT_PCM> pcmInput_;
    std::vector<INT_PCM> pcmOutput_;
    bool valid_ = false;
};
}

extern "C" wifi_aac_t wifi_aac_create(int sample_rate, int bitrate_bps) {
    std::unique_ptr<WifiAacCodec> codec(new WifiAacCodec(sample_rate, bitrate_bps));
    return codec->valid() ? codec.release() : nullptr;
}

extern "C" void wifi_aac_destroy(wifi_aac_t codec) {
    delete static_cast<WifiAacCodec *>(codec);
}

extern "C" int wifi_aac_frame_length(wifi_aac_t codec) {
    return codec ? static_cast<WifiAacCodec *>(codec)->frameLength() : 0;
}

extern "C" int wifi_aac_encode(wifi_aac_t codec, const float *stereo, int frames,
                                uint8_t *output, int output_capacity) {
    return codec ? static_cast<WifiAacCodec *>(codec)->encode(stereo, frames, output, output_capacity) : -1;
}

extern "C" int wifi_aac_decode(wifi_aac_t codec, const uint8_t *input, int input_size,
                                float *stereo, int output_capacity_frames) {
    return codec ? static_cast<WifiAacCodec *>(codec)->decode(input, input_size, stereo, output_capacity_frames) : -1;
}
