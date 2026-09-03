#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef void *usb_host_audio_t;
typedef struct {
    uint64_t input_packet_errors;
    uint64_t input_empty_packets;
    uint64_t input_transfer_errors;
    uint64_t input_ring_overruns;
    uint64_t output_transfer_errors;
    uint64_t output_low_water_events;
    uint64_t input_callback_max_us;
    uint64_t output_callback_max_us;
    uint64_t output_ring_overruns;
    uint64_t input_buffer_clears;
    uint64_t dsp_last_us;
    uint64_t dsp_max_us;
    uint32_t input_sample_rate;
    uint32_t output_sample_rate;
    uint8_t input_bit_resolution;
    uint8_t input_channels;
    uint8_t output_bit_resolution;
    uint8_t output_channels;
} usb_host_audio_stats_t;
usb_host_audio_t usb_host_audio_start(int fd, int sample_rate, int bit_depth,
                                      int enable_input, int enable_output,
                                      int output_max_buffer_ms,
                                      int processing_frames,
                                      int input_burst_packets, int output_burst_packets);
int usb_host_audio_read(usb_host_audio_t audio, float *stereo, int frames);
int usb_host_audio_write(usb_host_audio_t audio, const float *stereo, int frames);
void usb_host_audio_configure_output_buffer(usb_host_audio_t audio, int max_buffer_ms);
void usb_host_audio_configure_input_buffer(usb_host_audio_t audio, int max_buffer_ms);
void usb_host_audio_get_stats(usb_host_audio_t audio, usb_host_audio_stats_t *stats);
void usb_host_audio_stop(usb_host_audio_t audio);
#ifdef __cplusplus
}
#endif
