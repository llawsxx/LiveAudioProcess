#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void *wifi_aac_t;

wifi_aac_t wifi_aac_create(int sample_rate, int bitrate_bps);
void wifi_aac_destroy(wifi_aac_t codec);
int wifi_aac_frame_length(wifi_aac_t codec);
int wifi_aac_encode(wifi_aac_t codec, const float *stereo, int frames,
                    uint8_t *output, int output_capacity);
int wifi_aac_decode(wifi_aac_t codec, const uint8_t *input, int input_size,
                    float *stereo, int output_capacity_frames);

#ifdef __cplusplus
}
#endif
