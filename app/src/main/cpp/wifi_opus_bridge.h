#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *wifi_opus_t;

enum {
    WIFI_OPUS_APPLICATION_VOIP = 2048,
    WIFI_OPUS_APPLICATION_AUDIO = 2049,
    WIFI_OPUS_APPLICATION_RESTRICTED_LOWDELAY = 2051
};

wifi_opus_t wifi_opus_create(int sample_rate, int bitrate_bps, int application,
                             int frame_size);
void wifi_opus_destroy(wifi_opus_t codec);
int wifi_opus_frame_length(wifi_opus_t codec);
int wifi_opus_sample_rate(wifi_opus_t codec);
int wifi_opus_encode(wifi_opus_t codec, const float *stereo, int frames,
                     uint8_t *output, int output_capacity);
int wifi_opus_decode(wifi_opus_t codec, const uint8_t *input, int input_size,
                     float *stereo, int output_capacity_frames);

#ifdef __cplusplus
}
#endif
