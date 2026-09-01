#pragma once
#ifdef __cplusplus
extern "C" {
#endif
typedef void *usb_host_audio_t;
usb_host_audio_t usb_host_audio_start(int fd, int sample_rate, int enable_input, int enable_output,
                                      int output_min_buffer_ms, int output_max_buffer_ms,
                                      int input_burst_packets, int output_burst_packets);
int usb_host_audio_read(usb_host_audio_t audio, float *stereo, int frames);
int usb_host_audio_write(usb_host_audio_t audio, const float *stereo, int frames);
void usb_host_audio_configure_output_buffer(usb_host_audio_t audio, int min_buffer_ms,
                                            int max_buffer_ms);
void usb_host_audio_stop(usb_host_audio_t audio);
#ifdef __cplusplus
}
#endif
