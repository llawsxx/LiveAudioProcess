#include <aaudio/AAudio.h>
#include <jni.h>
#include <android/log.h>
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <arpa/inet.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include "convolution_reverb.h"
#include "test_music.h"
#include "usb_host_bridge.h"
#include "wifi_aac_bridge.h"

#define TAG "AudioProcessNative"
#define NET_LOG_LINES 1000
#define NET_LOG_LINE_BYTES 256
static uint64_t now_ns(void);
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_log_lines[NET_LOG_LINES][NET_LOG_LINE_BYTES];
static int g_log_write, g_log_count;
static void app_log_print(android_LogPriority priority, const char *fmt, ...) {
    va_list args, copy;
    va_start(args, fmt);
    va_copy(copy, args);
    __android_log_vprint(priority, TAG, fmt, args);
    pthread_mutex_lock(&g_log_lock);
    struct timespec wall_time;
    struct tm local_time;
    clock_gettime(CLOCK_REALTIME,&wall_time);
    localtime_r(&wall_time.tv_sec,&local_time);
    int prefix=snprintf(g_log_lines[g_log_write],NET_LOG_LINE_BYTES,
                        "%04d-%02d-%02d %02d:%02d:%02d.%03ld %c | ",
                        local_time.tm_year+1900,local_time.tm_mon+1,local_time.tm_mday,
                        local_time.tm_hour,local_time.tm_min,local_time.tm_sec,
                        wall_time.tv_nsec/1000000L,
                        priority==ANDROID_LOG_ERROR?'E':'I');
    if(prefix<0)prefix=0;
    if(prefix>=NET_LOG_LINE_BYTES)prefix=NET_LOG_LINE_BYTES-1;
    vsnprintf(g_log_lines[g_log_write]+prefix,NET_LOG_LINE_BYTES-(size_t)prefix,fmt,copy);
    g_log_write=(g_log_write+1)%NET_LOG_LINES;
    if(g_log_count<NET_LOG_LINES)g_log_count++;
    pthread_mutex_unlock(&g_log_lock);
    va_end(copy);
    va_end(args);
}
#define LOGE(...) app_log_print(ANDROID_LOG_ERROR, __VA_ARGS__)
#define LOGI(...) app_log_print(ANDROID_LOG_INFO, __VA_ARGS__)
#define MAX_FRAMES 2048
#define NET_PACKET_FRAMES 128
#define NET_PCM_MAX_PACKET_FRAMES 9600
#define NET_UDP_MAX_PAYLOAD_BYTES 65507
#define NET_AAC_FRAMES 1024
#define NET_AAC_MAX_PACKET_BYTES 8192
#define NET_AAC_MAX_FRAMES_PER_PACKET 10
#define NET_AAC_STORED_PACKET_BYTES 16384
#define NET_AAC_JITTER_SLOTS 128
#define NET_JITTER_SLOTS 1024
#define NET_REORDER_BACKTRACK_PACKETS 8
#define NET_PROTOCOL_VERSION 3
#define NET_TRANSPORT_UDP 0
#define NET_TRANSPORT_TCP 1
#define NET_TCP_BUFFER_BYTES 262144
#define NET_TCP_RECEIVE_BUFFER_BYTES (1024 * 1024)
#define NET_TCP_SEND_BUFFER_BYTES (512 * 1024)
#define NET_UDP_RECEIVE_BUFFER_BYTES (1024 * 1024)
#define NET_UDP_SEND_BUFFER_BYTES (512 * 1024)
#define NET_PLC_HISTORY_FRAMES 4096
#define EQ_BANDS 4
#define MAX_INPUT_CHANNELS 8

typedef struct { float b0,b1,b2,a1,a2,z1,z2,freq,gain,q; } Biquad;
typedef struct __attribute__((packed)) { uint32_t magic; uint16_t version; uint8_t codec; uint8_t channels; uint32_t rate; uint32_t frames; uint64_t timestamp_ns; uint32_t sequence; } NetHeader;
enum {
    NET_ERROR_NONE = 0,
    NET_ERROR_INVALID_ROLE = 1,
    NET_ERROR_INVALID_TRANSPORT = 2,
    NET_ERROR_INVALID_CODEC = 3,
    NET_ERROR_INVALID_SAMPLE_RATE = 4,
    NET_ERROR_INVALID_HOST = 5,
    NET_ERROR_INVALID_PORT = 6,
    NET_ERROR_AAC_INIT = 7,
    NET_ERROR_SOCKET = 8,
    NET_ERROR_ADDRESS_IN_USE = 9,
    NET_ERROR_BIND = 10,
    NET_ERROR_LISTEN = 11,
    NET_ERROR_CONNECT = 12,
    NET_ERROR_TCP_WAITING_DATA = 13,
    NET_ERROR_PACKET_TOO_SHORT = 20,
    NET_ERROR_MAGIC_MISMATCH = 21,
    NET_ERROR_VERSION_MISMATCH = 22,
    NET_ERROR_CODEC_MISMATCH = 23,
    NET_ERROR_CHANNEL_MISMATCH = 24,
    NET_ERROR_RATE_MISMATCH = 25,
    NET_ERROR_FRAMES_MISMATCH = 26,
    NET_ERROR_PCM_FORMAT_MISMATCH = 27,
    NET_ERROR_AAC_PACKET_SIZE = 28,
    NET_ERROR_AAC_DECODE = 29,
    NET_ERROR_TCP_FRAMING = 30,
    NET_ERROR_TCP_RECEIVE = 31,
    NET_ERROR_PEER_DISCONNECTED = 32
};
typedef struct {
    uint32_t sequence;
    int valid;
    float samples[NET_PACKET_FRAMES * 2];
} NetJitterSlot;
typedef struct {
    uint32_t sequence;
    uint32_t size;
    uint32_t frames;
    int valid;
    uint8_t data[NET_AAC_STORED_PACKET_BYTES];
} NetAacJitterSlot;
#define RECORD_QUEUE_BLOCKS 32
#define LOUDNESS_SUBBLOCKS 30
#define LOUDNESS_GATE_BLOCKS 300
#define LOUDNESS_LRA_BLOCKS 30
typedef struct {
    int frames;
    int16_t dry[MAX_FRAMES * 2];
    int16_t wet[MAX_FRAMES * 2];
} RecordBlock;
typedef struct {
    AAudioStream *input, *output;
    pthread_t thread;
    atomic_int running, recording, flags;
    _Atomic(float) levels[6];
    _Atomic(float) peak_levels[4];
    uint64_t peak_hold_until_ns[4];
    float waveform_dry[512], waveform_wet[512];
    int waveform_count, waveform_pos;
    pthread_mutex_t waveform_lock;
    int rate, output_rate, frames, in_channels, pair;
    double output_resample_phase;
    float values[28];
    pthread_mutex_t param_lock, file_lock, stream_lock;
    Biquad eq[2][EQ_BANDS];
    ConvolutionReverb *reverb;
    pthread_mutex_t reverb_lock;
    pthread_t reverb_thread;
    atomic_int reverb_worker_running, reverb_worker_started;
    atomic_uint reverb_generation;
    float *lookahead;
    int look_size, look_pos;
    int *limiter_next_pos;
    double *limiter_next_delta;
    int limiter_delay_frames, limiter_next_iter, limiter_next_len;
    double limiter_gain, limiter_delta, limiter_peak_activity;
    /* BS.1770 measurement runs on a side-chain and never buffers output. */
    double loudness_b[5], loudness_a[5], loudness_v[2][5];
    double loudness_subblocks[LOUDNESS_SUBBLOCKS];
    double loudness_gate_blocks[LOUDNESS_GATE_BLOCKS];
    double loudness_lra_blocks[LOUDNESS_LRA_BLOCKS];
    double loudness_subblock_sum;
    int loudness_subblock_frames, loudness_subblock_count;
    int loudness_subblock_pos, loudness_subblock_valid;
    int loudness_gate_pos, loudness_gate_valid;
    int loudness_lra_pos, loudness_lra_valid, loudness_lra_hop;
    double loudness_measured_lra;
    double loudness_gain, loudness_desired_gain, loudness_gain_coeff;
    double loudness_peak_gain, loudness_peak_release_coeff, loudness_tp_limit;
    double loudness_tp_db;
    FILE *dry_file, *wet_file;
    uint32_t dry_bytes, wet_bytes;
    pthread_t record_thread;
    atomic_int record_worker_running, record_worker_started;
    pthread_mutex_t record_cv_mutex;
    pthread_cond_t record_cv;
    RecordBlock *record_blocks;
    atomic_uint record_write_block, record_read_block, record_drops, record_producers;
    atomic_ullong record_written_frames;
    /* A recording is one timeline. Route restarts may change g.rate, so keep
       the format/timebase captured when the files were opened. */
    int record_rate;
    atomic_ullong dsp_last_us, dsp_max_us;
    int net_sock, net_listen_sock, net_transport, net_codec, net_bitrate, net_port, net_packet_ms, net_pcm_packet_frames, net_aac_frames_per_packet, net_min_ms, net_max_ms;
    int net_max_hold_ms;
    uint64_t net_max_since_ns;
    int net_tcp_connecting;
    wifi_aac_t net_aac;
    pthread_mutex_t net_codec_lock;
    atomic_int net_role, use_network_input, net_packet_seen;
    atomic_int net_error, net_error_detail0, net_error_detail1;
    atomic_int net_rx_disconnect_requested, net_rx_timeout_reported, net_tx_disconnect_requested;
    atomic_uint net_tcp_connect_attempts;
    atomic_ullong net_last_packet_ns;
    atomic_int net_buffer_ms;
    atomic_ullong net_min_buffer_events, net_max_buffer_events;
    NetJitterSlot *net_jitter;
    NetAacJitterSlot *net_aac_jitter;
    usb_host_audio_t usb_audio;
    int usb_input_host, usb_output_host;
    int usb_buffer_max_ms;
    atomic_int usb_input_buffer_max_ms;
    atomic_int output_buffer_max_ms;
    float *input_ring;
    uint32_t input_ring_capacity;
    atomic_uint input_write_frame, input_read_frame;
    atomic_uint input_callback_overruns, input_buffer_clears;
    atomic_int input_buffer_max_ms;
    atomic_int input_error;
    float *output_ring;
    uint32_t output_ring_capacity;
    atomic_uint output_prefill_frames, output_queue_limit;
    atomic_uint output_write_frame, output_read_frame;
    atomic_uint output_callback_underflows, output_buffer_clears;
    atomic_int output_callback_started, output_error;
    int net_packet_count, net_started, net_play_offset, net_seq_initialized;
    int net_buffer_monitor_started, net_min_boundary_latched, net_max_boundary_latched;
    int net_aac_packet_count, net_aac_started, net_aac_seq_initialized;
    int net_send_count;
    struct sockaddr_in net_addr;
    uint32_t net_seq, net_play_seq, net_high_seq;
    uint32_t net_aac_play_seq, net_aac_high_seq;
    uint64_t net_missing_packets, net_late_packets, net_duplicate_packets;
    float net_plc_history[NET_PLC_HISTORY_FRAMES * 2];
    int net_plc_history_write, net_plc_history_count;
    int net_plc_active, net_plc_pitch_lag, net_plc_generated_frames;
    int net_plc_recovery_frames, net_plc_recovery_total;
    uint8_t net_rx_buffer[NET_TCP_BUFFER_BYTES];
    size_t net_rx_used, net_tx_used, net_tx_offset;
    uint8_t net_tx_buffer[NET_TCP_BUFFER_BYTES];
    uint64_t net_tx_blocked_since_ns;
    uint64_t net_tcp_next_connect_ns;
    atomic_int tone_enabled;
    int tone_waveform;
    int tone_music;
    int tone_channels;
    float tone_frequency, tone_frequency2, tone_duration_seconds, tone_click_interval_ms, tone_level;
    double tone_phase, tone_phase2;
    uint64_t tone_frame_counter;
    uint32_t tone_noise_state;
    TestMusicState tone_music_state;
    float net_send_buffer[NET_PCM_MAX_PACKET_FRAMES * 2];
    uint8_t net_aac_send_buffer[NET_AAC_STORED_PACKET_BYTES];
    size_t net_aac_send_used;
    int net_aac_send_frames;
} Engine;

static Engine g = {
    .net_sock = -1,
    .net_listen_sock = -1,
    .usb_buffer_max_ms = 50,
    .output_buffer_max_ms = ATOMIC_VAR_INIT(40),
    .usb_input_buffer_max_ms = ATOMIC_VAR_INIT(20),
    .input_buffer_max_ms = ATOMIC_VAR_INIT(20),
    .net_max_hold_ms = 1000,
    .param_lock = PTHREAD_MUTEX_INITIALIZER,
    .file_lock = PTHREAD_MUTEX_INITIALIZER,
    .stream_lock = PTHREAD_MUTEX_INITIALIZER,
    .record_cv_mutex = PTHREAD_MUTEX_INITIALIZER,
    .record_cv = PTHREAD_COND_INITIALIZER,
    .net_codec_lock = PTHREAD_MUTEX_INITIALIZER,
    .reverb_lock = PTHREAD_MUTEX_INITIALIZER,
    .waveform_lock = PTHREAD_MUTEX_INITIALIZER,
    .tone_noise_state = 0x13579BDFu
};

/* shutdown() makes a TCP peer and any in-flight operation observe a
 * transport switch immediately; close() then releases the descriptor/port. */
static void network_close_socket(int *fd) {
    if (!fd || *fd < 0) return;
    shutdown(*fd, SHUT_RDWR);
    close(*fd);
    *fd = -1;
}

#define NET_MAGIC 0x50464C58u

enum { DSP_ON=1, EQ_ON=2, REVERB_ON=4, LIMITER_ON=8, LOUDNESS_ON=16 };
enum {
    P_EQ1_F,P_EQ1_G,P_EQ1_Q,P_EQ2_F,P_EQ2_G,P_EQ2_Q,P_EQ3_F,P_EQ3_G,P_EQ3_Q,P_EQ4_F,P_EQ4_G,P_EQ4_Q,
    P_ROOM,P_DECAY,P_DAMP,P_MIX,P_LIM_IN,P_LIMIT,P_RELEASE,P_CEILING,P_LOOKAHEAD,P_ADAPTIVE_RELEASE,
    P_LOUDNESS_TARGET,P_LOUDNESS_LRA,P_LOUDNESS_TP
};

static float db_to_linear(float db) { return powf(10.f, db / 20.f); }
static float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

static uint32_t next_power_of_two(uint32_t value) {
    uint32_t result = 1;
    while (result < value && result < (1u << 30)) result <<= 1;
    return result;
}

static uint32_t input_ring_queued_frames(void) {
    /* Monotonic uint32 cursors intentionally wrap; unsigned subtraction keeps
       the SPSC distance valid because the queue can never span 2^32 frames. */
    uint32_t write = atomic_load_explicit(&g.input_write_frame, memory_order_acquire);
    uint32_t read = atomic_load_explicit(&g.input_read_frame, memory_order_acquire);
    uint32_t queued = write - read;
    return queued <= g.input_ring_capacity ? queued : 0;
}

static int input_ring_create(int frames_per_burst, int processing_frames) {
    uint32_t burst = frames_per_burst > 0 ? (uint32_t)frames_per_burst : 192u;
    uint32_t block = processing_frames > 0 ? (uint32_t)processing_frames : burst;
    uint32_t channels = g.in_channels > 0 && g.in_channels <= MAX_INPUT_CHANNELS
            ? (uint32_t)g.in_channels : 1u;
    uint32_t configured_frames = (uint32_t)(((uint64_t)g.rate * 200u) / 1000u);
    uint32_t required = configured_frames + burst * 2u + block;
    if (required < 4096u) required = 4096u;
    g.input_ring_capacity = next_power_of_two(required);
    g.input_ring = calloc((size_t)g.input_ring_capacity * channels, sizeof(float));
    atomic_store(&g.input_write_frame, 0);
    atomic_store(&g.input_read_frame, 0);
    atomic_store(&g.input_callback_overruns, 0);
    atomic_store(&g.input_buffer_clears, 0);
    return g.input_ring != NULL;
}

static void input_ring_destroy(void) {
    free(g.input_ring);
    g.input_ring = NULL;
    g.input_ring_capacity = 0;
}

static int input_ring_read(float *data, uint32_t frames) {
    if (!g.input_ring || !data || frames == 0 || frames > g.input_ring_capacity) return 0;
    uint32_t read;
    while (atomic_load(&g.running) && atomic_load(&g.input_error) == AAUDIO_OK) {
        uint32_t write = atomic_load_explicit(&g.input_write_frame, memory_order_acquire);
        read = atomic_load_explicit(&g.input_read_frame, memory_order_relaxed);
        uint32_t available = write - read;
        int max_ms = atomic_load_explicit(&g.input_buffer_max_ms, memory_order_relaxed);
        uint32_t max_frames = (uint32_t)(((uint64_t)g.rate * (uint32_t)max_ms) / 1000u);
        if (max_frames < frames) max_frames = frames;
        if (available > max_frames) {
            atomic_store_explicit(&g.input_read_frame, write, memory_order_release);
            unsigned int count = atomic_fetch_add(&g.input_buffer_clears, 1) + 1;
            if (count == 1 || count % 100 == 0)
                LOGI("AAudio input buffer cleared: queued=%u limit=%u count=%u",
                     available, max_frames, count);
            continue;
        }
        if (available <= g.input_ring_capacity && available >= frames) break;
        usleep(250);
    }
    if (!atomic_load(&g.running) || atomic_load(&g.input_error) != AAUDIO_OK) return 0;
    uint32_t channels = (uint32_t)g.in_channels;
    uint32_t offset = read & (g.input_ring_capacity - 1u);
    uint32_t first = frames < g.input_ring_capacity - offset ? frames : g.input_ring_capacity - offset;
    memcpy(data, g.input_ring + (size_t)offset * channels,
           (size_t)first * channels * sizeof(float));
    if (first < frames)
        memcpy(data + (size_t)first * channels, g.input_ring,
               (size_t)(frames - first) * channels * sizeof(float));
    atomic_store_explicit(&g.input_read_frame, read + frames, memory_order_release);
    return (int)frames;
}

static aaudio_data_callback_result_t input_data_callback(
        AAudioStream *stream, void *user_data, void *audio_data, int32_t num_frames) {
    (void)stream;
    (void)user_data;
    const float *source = audio_data;
    if (!source || num_frames <= 0 || !atomic_load(&g.running) || !g.input_ring)
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    uint32_t frames = (uint32_t)num_frames;
    uint32_t channels = (uint32_t)g.in_channels;
    uint32_t write = atomic_load_explicit(&g.input_write_frame, memory_order_relaxed);
    uint32_t read = atomic_load_explicit(&g.input_read_frame, memory_order_acquire);
    uint32_t queued = write - read;
    if (queued > g.input_ring_capacity || frames > g.input_ring_capacity - queued) {
        unsigned int count = atomic_fetch_add(&g.input_callback_overruns, 1) + 1;
        if (count == 1 || count % 100 == 0)
            LOGI("AAudio input callback overrun count=%u", count);
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }
    uint32_t offset = write & (g.input_ring_capacity - 1u);
    uint32_t first = frames < g.input_ring_capacity - offset ? frames : g.input_ring_capacity - offset;
    memcpy(g.input_ring + (size_t)offset * channels, source,
           (size_t)first * channels * sizeof(float));
    if (first < frames)
        memcpy(g.input_ring, source + (size_t)first * channels,
               (size_t)(frames - first) * channels * sizeof(float));
    atomic_store_explicit(&g.input_write_frame, write + frames, memory_order_release);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void input_error_callback(AAudioStream *stream, void *user_data, aaudio_result_t error) {
    (void)stream;
    (void)user_data;
    atomic_store(&g.input_error, error);
    LOGE("AAudio input stream error: %d", error);
}

static uint32_t output_ring_queued_frames(void) {
    uint32_t write = atomic_load_explicit(&g.output_write_frame, memory_order_acquire);
    uint32_t read = atomic_load_explicit(&g.output_read_frame, memory_order_acquire);
    uint32_t queued = write - read;
    return queued <= g.output_ring_capacity ? queued : 0;
}

static void output_ring_update_limits(int sample_rate, int processing_frames) {
    if (!g.output_ring || g.output_ring_capacity == 0) return;
    uint32_t block = processing_frames > 0 ? (uint32_t)processing_frames : 1u;
    int max_ms = atomic_load_explicit(&g.output_buffer_max_ms, memory_order_relaxed);
    uint32_t limit = sample_rate > 0
            ? (uint32_t)(((uint64_t)(uint32_t)sample_rate * (uint32_t)max_ms) / 1000u)
            : block;
    if (limit < block) limit = block;
    if (limit > g.output_ring_capacity) limit = g.output_ring_capacity;
    uint32_t prefill = limit / 2u;
    if (prefill < block) prefill = block;
    if (prefill > limit) prefill = limit;
    atomic_store_explicit(&g.output_prefill_frames, prefill, memory_order_release);
    atomic_store_explicit(&g.output_queue_limit, limit, memory_order_release);
}

static int output_ring_create(int frames_per_burst, int processing_frames, int sample_rate) {
    uint32_t burst = frames_per_burst > 0 ? (uint32_t)frames_per_burst : 192u;
    uint32_t block = processing_frames > 0 ? (uint32_t)processing_frames : burst;
    uint32_t configured_frames = sample_rate > 0
            ? (uint32_t)(((uint64_t)(uint32_t)sample_rate * 200u) / 1000u)
            : 9600u;
    uint32_t required = configured_frames > block * 2u ? configured_frames : block * 2u;
    g.output_ring_capacity = next_power_of_two(required);
    g.output_ring = calloc((size_t)g.output_ring_capacity * 2u, sizeof(float));
    if (!g.output_ring) return 0;
    output_ring_update_limits(sample_rate, processing_frames);
    atomic_store(&g.output_write_frame, 0);
    atomic_store(&g.output_read_frame, 0);
    atomic_store(&g.output_callback_underflows, 0);
    atomic_store(&g.output_buffer_clears, 0);
    atomic_store(&g.output_callback_started, 0);
    return 1;
}

static void output_ring_destroy(void) {
    free(g.output_ring);
    g.output_ring = NULL;
    g.output_ring_capacity = 0;
    atomic_store(&g.output_prefill_frames, 0);
    atomic_store(&g.output_queue_limit, 0);
}

static int output_ring_write(const float *data, uint32_t frames) {
    if (!g.output_ring || frames == 0 || frames > g.output_ring_capacity) return 0;
    uint32_t write;
    while (atomic_load(&g.running)) {
        write = atomic_load_explicit(&g.output_write_frame, memory_order_relaxed);
        uint32_t read = atomic_load_explicit(&g.output_read_frame, memory_order_acquire);
        uint32_t queued = write - read;
        /* Network input is packet-buffered and can be consumed faster than
         * wall clock. Apply backpressure at the same logical limit used by
         * output_data_callback; allowing the physical ring capacity here
         * creates a fill-to-16384/clear-to-zero loop. */
        uint32_t limit = atomic_load_explicit(&g.output_queue_limit,
                                              memory_order_acquire);
        if (limit == 0 || limit > g.output_ring_capacity)
            limit = g.output_ring_capacity;
        if (queued <= limit && frames <= limit - queued) break;
        usleep(250);
    }
    if (!atomic_load(&g.running)) return 0;
    uint32_t offset = write & (g.output_ring_capacity - 1u);
    uint32_t first = frames < g.output_ring_capacity - offset ? frames : g.output_ring_capacity - offset;
    memcpy(g.output_ring + (size_t)offset * 2u, data, (size_t)first * 2u * sizeof(float));
    if (first < frames)
        memcpy(g.output_ring, data + (size_t)first * 2u, (size_t)(frames - first) * 2u * sizeof(float));
    atomic_store_explicit(&g.output_write_frame, write + frames, memory_order_release);
    return 1;
}

static aaudio_data_callback_result_t output_data_callback(
        AAudioStream *stream, void *user_data, void *audio_data, int32_t num_frames) {
    (void)stream;
    (void)user_data;
    float *target = audio_data;
    if (!target || num_frames <= 0) return AAUDIO_CALLBACK_RESULT_CONTINUE;
    if (!atomic_load(&g.running) || !g.output_ring) {
        memset(target, 0, (size_t)num_frames * 2u * sizeof(float));
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }
    uint32_t write = atomic_load_explicit(&g.output_write_frame, memory_order_acquire);
    uint32_t read = atomic_load_explicit(&g.output_read_frame, memory_order_relaxed);
    uint32_t available = write - read;
    /* The writer applies the logical queue limit as backpressure. A live
     * limit reduction can leave a valid queue temporarily above that limit;
     * drain it instead of clearing audio. Only a physical ring overrun is
     * unrecoverable and warrants a reset. */
    if (available > g.output_ring_capacity) {
        uint32_t limit = atomic_load_explicit(&g.output_queue_limit,
                                              memory_order_acquire);
        atomic_store_explicit(&g.output_read_frame, write, memory_order_release);
        atomic_store_explicit(&g.output_callback_started, 0, memory_order_relaxed);
        unsigned int count = atomic_fetch_add(&g.output_buffer_clears, 1) + 1;
        if (count == 1 || count % 100 == 0)
            LOGI("AAudio output buffer cleared: queued=%u limit=%u count=%u",
                 available, limit, count);
        memset(target, 0, (size_t)num_frames * 2u * sizeof(float));
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }
    if (!atomic_load_explicit(&g.output_callback_started, memory_order_relaxed)) {
        uint32_t prefill = atomic_load_explicit(&g.output_prefill_frames, memory_order_acquire);
        if (available < prefill) {
            memset(target, 0, (size_t)num_frames * 2u * sizeof(float));
            return AAUDIO_CALLBACK_RESULT_CONTINUE;
        }
        atomic_store_explicit(&g.output_callback_started, 1, memory_order_relaxed);
    }
    uint32_t requested = (uint32_t)num_frames;
    double step = (double)(g.rate > 0 ? g.rate : 48000) /
                  (double)(g.output_rate > 0 ? g.output_rate : (g.rate > 0 ? g.rate : 48000));
    double end_position = g.output_resample_phase + (double)requested * step;
    uint32_t source_needed = (uint32_t)end_position;
    uint32_t last_index = requested > 0
            ? (uint32_t)(g.output_resample_phase + (double)(requested - 1u) * step) : 0u;
    if (available < source_needed || available <= last_index + 1u) {
        memset(target, 0, (size_t)requested * 2u * sizeof(float));
        unsigned int count = atomic_fetch_add(&g.output_callback_underflows, 1) + 1;
        atomic_store_explicit(&g.output_callback_started, 0, memory_order_relaxed);
        if (count == 1 || count % 100 == 0) LOGI("AAudio output callback underflow count=%u", count);
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }
    uint32_t offset = read & (g.output_ring_capacity - 1u);
    for (uint32_t i = 0; i < requested; ++i) {
        double position = g.output_resample_phase + (double)i * step;
        uint32_t source_offset = (uint32_t)position;
        float frac = (float)(position - (double)source_offset);
        uint32_t p0 = (offset + source_offset) & (g.output_ring_capacity - 1u);
        uint32_t p1 = (offset + source_offset + 1u) & (g.output_ring_capacity - 1u);
        target[i * 2] = g.output_ring[p0 * 2] + (g.output_ring[p1 * 2] - g.output_ring[p0 * 2]) * frac;
        target[i * 2 + 1] = g.output_ring[p0 * 2 + 1] + (g.output_ring[p1 * 2 + 1] - g.output_ring[p0 * 2 + 1]) * frac;
    }
    g.output_resample_phase = end_position - (double)source_needed;
    atomic_store_explicit(&g.output_read_frame, read + source_needed, memory_order_release);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void output_error_callback(AAudioStream *stream, void *user_data, aaudio_result_t error) {
    (void)stream;
    (void)user_data;
    atomic_store(&g.output_error, error);
    LOGE("AAudio output stream error: %d", error);
}

/* Coefficient form follows FFmpeg af_biquads equalizer and uses transposed DF-II. */
static void biquad_config(Biquad *b, int rate, float freq, float gain, float q) {
    freq = clampf(freq, 20.f, rate * .49f); q = clampf(q, .1f, 20.f);
    if (b->freq == freq && b->gain == gain && b->q == q) return;
    b->freq=freq; b->gain=gain; b->q=q;
    float A=powf(10.f,gain/40.f), w=2.f*(float)M_PI*freq/rate;
    float alpha=sinf(w)/(2.f*q), c=cosf(w), a0=1.f+alpha/A;
    b->b0=(1.f+alpha*A)/a0; b->b1=(-2.f*c)/a0; b->b2=(1.f-alpha*A)/a0;
    b->a1=(-2.f*c)/a0; b->a2=(1.f-alpha/A)/a0;
}
static float biquad_process(Biquad *b, float x) {
    float y=b->b0*x+b->z1; b->z1=b->b1*x-b->a1*y+b->z2; b->z2=b->b2*x-b->a2*y; return y;
}

/* IR preparation is deliberately kept off the real-time AAudio processing thread. */
static void *reverb_worker(void *unused) {
    (void)unused;
    unsigned int built_generation = atomic_load(&g.reverb_generation);
    while (atomic_load(&g.reverb_worker_running)) {
        unsigned int requested_generation = atomic_load(&g.reverb_generation);
        if (requested_generation == built_generation) {
            usleep(10000);
            continue;
        }
        float room, decay, damping;
        pthread_mutex_lock(&g.param_lock);
        room = g.values[P_ROOM];
        decay = g.values[P_DECAY];
        damping = g.values[P_DAMP];
        pthread_mutex_unlock(&g.param_lock);
        ConvolutionReverb *replacement = convolution_reverb_create(
                g.rate, room, decay, damping);
        if (!atomic_load(&g.reverb_worker_running)) {
            convolution_reverb_destroy(replacement);
            break;
        }
        if (replacement) {
            pthread_mutex_lock(&g.reverb_lock);
            ConvolutionReverb *previous = g.reverb;
            g.reverb = replacement;
            pthread_mutex_unlock(&g.reverb_lock);
            convolution_reverb_destroy(previous);
        }
        built_generation = requested_generation;
    }
    return NULL;
}

static void limiter_reset(int delay_frames) {
    memset(g.lookahead, 0, (size_t)g.look_size * sizeof(*g.lookahead));
    memset(g.limiter_next_delta, 0, (size_t)g.look_size * sizeof(*g.limiter_next_delta));
    for (int i = 0; i < g.look_size; i++) g.limiter_next_pos[i] = -1;
    g.look_pos = 0;
    g.limiter_delay_frames = delay_frames;
    g.limiter_next_iter = 0;
    g.limiter_next_len = 0;
    g.limiter_gain = 1.0;
    g.limiter_delta = 0.0;
    g.limiter_peak_activity = 0.0;
}

static double loudness_to_energy(double loudness) {
    return pow(10.0, (loudness + 0.691) / 10.0);
}

static double loudness_from_energy(double energy) {
    return 10.0 * log10(fmax(energy, 1.0e-12)) - 0.691;
}

static double loudness_recent_average(const double *values, int size,
                                      int next_pos, int count) {
    double sum = 0.0;
    if (count > size) count = size;
    for (int i = 1; i <= count; ++i) {
        int index = next_pos - i;
        if (index < 0) index += size;
        sum += values[index];
    }
    return count > 0 ? sum / (double)count : 0.0;
}

/* Coefficients match FFmpeg's ebur128 K-weighting filter. The filtered signal
 * is used only for measurement, so its phase response adds no output delay. */
static void loudness_init_filter(int rate) {
    double f0 = 1681.974450955533;
    double gain = 3.999843853973347;
    double q = 0.7071752369554196;
    double k = tan(M_PI * f0 / (double)rate);
    double vh = pow(10.0, gain / 20.0);
    double vb = pow(vh, 0.4996667741545416);
    double pb[3], pa[3] = {1.0, 0.0, 0.0};
    double rb[3] = {1.0, -2.0, 1.0}, ra[3] = {1.0, 0.0, 0.0};
    double a0 = 1.0 + k / q + k * k;
    pb[0] = (vh + vb * k / q + k * k) / a0;
    pb[1] = 2.0 * (k * k - vh) / a0;
    pb[2] = (vh - vb * k / q + k * k) / a0;
    pa[1] = 2.0 * (k * k - 1.0) / a0;
    pa[2] = (1.0 - k / q + k * k) / a0;

    f0 = 38.13547087602444;
    q = 0.5003270373238773;
    k = tan(M_PI * f0 / (double)rate);
    a0 = 1.0 + k / q + k * k;
    ra[1] = 2.0 * (k * k - 1.0) / a0;
    ra[2] = (1.0 - k / q + k * k) / a0;

    g.loudness_b[0] = pb[0] * rb[0];
    g.loudness_b[1] = pb[0] * rb[1] + pb[1] * rb[0];
    g.loudness_b[2] = pb[0] * rb[2] + pb[1] * rb[1] + pb[2] * rb[0];
    g.loudness_b[3] = pb[1] * rb[2] + pb[2] * rb[1];
    g.loudness_b[4] = pb[2] * rb[2];
    g.loudness_a[0] = 1.0;
    g.loudness_a[1] = ra[1] + pa[1];
    g.loudness_a[2] = ra[2] + pa[1] * ra[1] + pa[2];
    g.loudness_a[3] = pa[1] * ra[2] + pa[2] * ra[1];
    g.loudness_a[4] = pa[2] * ra[2];
}

static void loudness_reset(void) {
    memset(g.loudness_v, 0, sizeof(g.loudness_v));
    memset(g.loudness_subblocks, 0, sizeof(g.loudness_subblocks));
    memset(g.loudness_gate_blocks, 0, sizeof(g.loudness_gate_blocks));
    memset(g.loudness_lra_blocks, 0, sizeof(g.loudness_lra_blocks));
    g.loudness_subblock_sum = 0.0;
    g.loudness_subblock_count = 0;
    g.loudness_subblock_pos = g.loudness_subblock_valid = 0;
    g.loudness_gate_pos = g.loudness_gate_valid = 0;
    g.loudness_lra_pos = g.loudness_lra_valid = g.loudness_lra_hop = 0;
    g.loudness_measured_lra = 0.0;
    g.loudness_gain = g.loudness_desired_gain = 1.0;
    g.loudness_gain_coeff = 0.0;
    g.loudness_peak_gain = 1.0;
    g.loudness_tp_limit = 1.0;
    g.loudness_tp_db = 1000.0;
}

static void loudness_init(int rate) {
    int safe_rate = rate > 0 ? rate : 48000;
    loudness_init_filter(safe_rate);
    g.loudness_subblock_frames = (safe_rate + 5) / 10;
    if (g.loudness_subblock_frames < 1) g.loudness_subblock_frames = 1;
    g.loudness_peak_release_coeff = 1.0 -
            exp(-1.0 / ((double)safe_rate * 0.100));
    loudness_reset();
}

static double loudness_filter_sample(int channel, double sample) {
    double *v = g.loudness_v[channel];
    v[0] = sample - g.loudness_a[1] * v[1] - g.loudness_a[2] * v[2]
                  - g.loudness_a[3] * v[3] - g.loudness_a[4] * v[4];
    double output = g.loudness_b[0] * v[0] + g.loudness_b[1] * v[1]
                  + g.loudness_b[2] * v[2] + g.loudness_b[3] * v[3]
                  + g.loudness_b[4] * v[4];
    v[4] = v[3]; v[3] = v[2]; v[2] = v[1]; v[1] = v[0];
    return output;
}

static void loudness_update_lra(double short_term_energy) {
    if (++g.loudness_lra_hop < 10) return;
    g.loudness_lra_hop = 0;
    g.loudness_lra_blocks[g.loudness_lra_pos] = short_term_energy;
    g.loudness_lra_pos = (g.loudness_lra_pos + 1) % LOUDNESS_LRA_BLOCKS;
    if (g.loudness_lra_valid < LOUDNESS_LRA_BLOCKS) g.loudness_lra_valid++;

    const double absolute_gate = loudness_to_energy(-70.0);
    double sum = 0.0;
    int count = 0;
    for (int i = 0; i < g.loudness_lra_valid; ++i) {
        if (g.loudness_lra_blocks[i] >= absolute_gate) {
            sum += g.loudness_lra_blocks[i];
            count++;
        }
    }
    if (count < 4) return;
    double gate = fmax(absolute_gate, (sum / (double)count) * 0.01);
    double sorted[LOUDNESS_LRA_BLOCKS];
    int sorted_count = 0;
    for (int i = 0; i < g.loudness_lra_valid; ++i) {
        double energy = g.loudness_lra_blocks[i];
        if (energy < gate) continue;
        double loudness = loudness_from_energy(energy);
        int insert = sorted_count;
        while (insert > 0 && sorted[insert - 1] > loudness) {
            sorted[insert] = sorted[insert - 1];
            insert--;
        }
        sorted[insert] = loudness;
        sorted_count++;
    }
    if (sorted_count < 4) return;
    int low = (int)floor(0.10 * (double)(sorted_count - 1));
    int high = (int)ceil(0.95 * (double)(sorted_count - 1));
    g.loudness_measured_lra = sorted[high] - sorted[low];
}

static void loudness_update_target(const float *p) {
    if (g.loudness_subblock_valid < 4) return;

    double momentary_energy = loudness_recent_average(
            g.loudness_subblocks, LOUDNESS_SUBBLOCKS,
            g.loudness_subblock_pos, 4);
    g.loudness_gate_blocks[g.loudness_gate_pos] = momentary_energy;
    g.loudness_gate_pos = (g.loudness_gate_pos + 1) % LOUDNESS_GATE_BLOCKS;
    if (g.loudness_gate_valid < LOUDNESS_GATE_BLOCKS) g.loudness_gate_valid++;

    /* BS.1770 integrated gate: first reject blocks below -70 LUFS, then reject
     * blocks more than 10 LU below the absolute-gated mean. */
    const double absolute_gate = loudness_to_energy(-70.0);
    double absolute_sum = 0.0;
    int absolute_count = 0;
    for (int i = 0; i < g.loudness_gate_valid; ++i) {
        double energy = g.loudness_gate_blocks[i];
        if (energy >= absolute_gate) { absolute_sum += energy; absolute_count++; }
    }
    if (absolute_count == 0) return;
    double relative_gate = (absolute_sum / (double)absolute_count) * 0.1;
    double gate = fmax(absolute_gate, relative_gate);
    double gated_sum = 0.0;
    int gated_count = 0;
    for (int i = 0; i < g.loudness_gate_valid; ++i) {
        double energy = g.loudness_gate_blocks[i];
        if (energy >= gate) { gated_sum += energy; gated_count++; }
    }
    if (gated_count == 0) return;

    double integrated = loudness_from_energy(gated_sum / (double)gated_count);
    int short_count = g.loudness_subblock_valid < LOUDNESS_SUBBLOCKS ?
                      g.loudness_subblock_valid : LOUDNESS_SUBBLOCKS;
    double short_term_energy = loudness_recent_average(
            g.loudness_subblocks, LOUDNESS_SUBBLOCKS,
            g.loudness_subblock_pos, short_count);
    double short_term = loudness_from_energy(short_term_energy);
    if (short_term < -60.0) return;
    if (short_count == LOUDNESS_SUBBLOCKS)
        loudness_update_lra(short_term_energy);

    double target = clampf(p[P_LOUDNESS_TARGET], -70.f, -5.f);
    double target_lra = clampf(p[P_LOUDNESS_LRA], 1.f, 50.f);
    double deviation = short_term - integrated;
    double dynamic_correction = 0.0;
    if (g.loudness_measured_lra > target_lra) {
        double range_ratio = target_lra / g.loudness_measured_lra;
        dynamic_correction = deviation * (range_ratio - 1.0);
    }
    dynamic_correction = fmin(fmax(dynamic_correction, -6.0), 6.0);
    double gain_db = fmin(fmax(target - integrated + dynamic_correction,
                              -12.0), 18.0);
    g.loudness_desired_gain = pow(10.0, gain_db / 20.0);
    double gain_tau = g.loudness_desired_gain < g.loudness_gain ? 0.080 :
                      fmin(1.500, 0.250 + 0.035 * target_lra);
    g.loudness_gain_coeff = 1.0 - exp(-1.0 / ((double)g.rate * gain_tau));
}

/* K-weighted, gated loudness measurement and linked-stereo gain control. All
 * windows live in the measurement side-chain; the current sample is emitted
 * immediately, so this stage adds zero samples of audio latency. */
static void loudness_process(float *l, float *r, const float *p) {
    double target_tp = clampf(p[P_LOUDNESS_TP], -9.f, 0.f);
    if (target_tp != g.loudness_tp_db) {
        g.loudness_tp_db = target_tp;
        g.loudness_tp_limit = pow(10.0, target_tp / 20.0);
    }
    double weighted_l = loudness_filter_sample(0, *l);
    double weighted_r = loudness_filter_sample(1, *r);
    g.loudness_subblock_sum += weighted_l * weighted_l + weighted_r * weighted_r;
    if (++g.loudness_subblock_count >= g.loudness_subblock_frames) {
        double energy = g.loudness_subblock_sum /
                        (double)g.loudness_subblock_count;
        g.loudness_subblocks[g.loudness_subblock_pos] = energy;
        g.loudness_subblock_pos = (g.loudness_subblock_pos + 1) % LOUDNESS_SUBBLOCKS;
        if (g.loudness_subblock_valid < LOUDNESS_SUBBLOCKS)
            g.loudness_subblock_valid++;
        g.loudness_subblock_sum = 0.0;
        g.loudness_subblock_count = 0;
        loudness_update_target(p);
    }

    g.loudness_gain += (g.loudness_desired_gain - g.loudness_gain) *
                       g.loudness_gain_coeff;
    g.loudness_gain = fmin(fmax(g.loudness_gain, 0.0630957), 7.94328);

    double peak = fmax(fabs((double)*l), fabs((double)*r));
    double applied_gain = g.loudness_gain * g.loudness_peak_gain;
    if (peak > 1.0e-9 && peak * applied_gain > g.loudness_tp_limit)
        g.loudness_peak_gain = fmin(g.loudness_peak_gain,
                                    g.loudness_tp_limit /
                                    (peak * g.loudness_gain));
    else
        g.loudness_peak_gain += (1.0 - g.loudness_peak_gain) *
                                g.loudness_peak_release_coeff;
    g.loudness_peak_gain = fmin(fmax(g.loudness_peak_gain, 0.0), 1.0);
    applied_gain = g.loudness_gain * g.loudness_peak_gain;
    *l = (float)((double)*l * applied_gain);
    *r = (float)((double)*r * applied_gain);
}

/*
 * Linked-stereo lookahead limiter adapted from FFmpeg af_alimiter's
 * scheduled-peak attenuation algorithm. Each detected peak installs an
 * attack ramp that reaches the required gain when that peak leaves the ring.
 */
static void limiter_process(float *l, float *r, const float *p) {
    const int channels = 2;
    float input_gain = db_to_linear(p[P_LIM_IN]);
    double threshold = fmin(fmax(pow(10.0, (double)p[P_LIMIT] / 20.0), 0.000001), 1.0);
    double ceiling = fmin(fmax(pow(10.0, (double)p[P_CEILING] / 20.0), 0.000001), 1.0);
    double limit = fmin(threshold, ceiling);
    double base_release = fmax((double)p[P_RELEASE], 10.0) / 1000.0;
    int adaptive_release = p[P_ADAPTIVE_RELEASE] >= 0.5f;
    int delay_frames = (int)(clampf(p[P_LOOKAHEAD], 0.f, 5.f) * g.rate / 1000.f);
    int max_delay_frames = g.look_size / channels;
    if (delay_frames < 1) delay_frames = 1;
    if (delay_frames > max_delay_frames) delay_frames = max_delay_frames;
    int buffer_size = delay_frames * channels;

    if (delay_frames != g.limiter_delay_frames) limiter_reset(delay_frames);

    float x0 = *l * input_gain;
    float x1 = *r * input_gain;
    float peak = fmaxf(fabsf(x0), fabsf(x1));
    g.lookahead[g.look_pos] = x0;
    g.lookahead[g.look_pos + 1] = x1;

    /* A smoothed overload activity tracks peak duration without resetting at
       waveform zero crossings. Transients use 0.5x release; sustained peaks
       approach 2x release. The final time remains inside the UI's range. */
    if (adaptive_release) {
        if ((double)peak > limit)
            g.limiter_peak_activity += (1.0 - g.limiter_peak_activity) /
                                       ((double)g.rate * 0.150);
        else
            g.limiter_peak_activity -= g.limiter_peak_activity /
                                       ((double)g.rate * 0.400);
        g.limiter_peak_activity = fmin(fmax(g.limiter_peak_activity, 0.0), 1.0);
    } else {
        g.limiter_peak_activity = 0.0;
    }
    double release_scale = adaptive_release ?
            0.5 + 1.5 * g.limiter_peak_activity : 1.0;
    double release = fmin(fmax(base_release * release_scale, 0.010), 10.0);

    if (peak > limit) {
        double target_gain = limit / (double)peak;
        double release_delta = (1.0 - target_gain) / ((double)g.rate * release);
        double attack_delta = (target_gain - g.limiter_gain) / (double)delay_frames;

        if (attack_delta < g.limiter_delta) {
            g.limiter_delta = attack_delta;
            g.limiter_next_pos[0] = g.look_pos;
            if (buffer_size > 1) g.limiter_next_pos[1] = -1;
            g.limiter_next_delta[0] = release_delta;
            g.limiter_next_len = 1;
            g.limiter_next_iter = 0;
        } else {
            int found = 0;
            int i;
            for (i = g.limiter_next_iter;
                 i < g.limiter_next_iter + g.limiter_next_len; i++) {
                int j = i % buffer_size;
                int scheduled_pos = g.limiter_next_pos[j];
                if (scheduled_pos < 0) continue;
                float scheduled_peak = fmaxf(fabsf(g.lookahead[scheduled_pos]),
                                              fabsf(g.lookahead[scheduled_pos + 1]));
                int distance_frames = ((buffer_size - scheduled_pos + g.look_pos) %
                                       buffer_size) / channels;
                if (scheduled_peak <= 0.f || distance_frames <= 0) continue;
                double scheduled_delta = (target_gain - limit / (double)scheduled_peak) /
                                         (double)distance_frames;
                if (scheduled_delta < g.limiter_next_delta[j]) {
                    g.limiter_next_delta[j] = scheduled_delta;
                    found = 1;
                    break;
                }
            }
            if (found) {
                g.limiter_next_len = i - g.limiter_next_iter + 1;
                int insert = (g.limiter_next_iter + g.limiter_next_len) % buffer_size;
                int sentinel = (insert + 1) % buffer_size;
                g.limiter_next_pos[insert] = g.look_pos;
                g.limiter_next_delta[insert] = release_delta;
                g.limiter_next_pos[sentinel] = -1;
                g.limiter_next_len++;
            }
        }
    }

    int output_pos = (g.look_pos + channels) % buffer_size;
    float out0 = g.lookahead[output_pos];
    float out1 = g.lookahead[output_pos + 1];
    float output_peak = fmaxf(fabsf(out0), fabsf(out1));

    g.limiter_gain += g.limiter_delta;
    *l = (float)((double)out0 * g.limiter_gain);
    *r = (float)((double)out1 * g.limiter_gain);

    if (g.limiter_next_len > 0 &&
        output_pos == g.limiter_next_pos[g.limiter_next_iter]) {
        g.limiter_delta = g.limiter_next_delta[g.limiter_next_iter];
        g.limiter_gain = output_peak > 0.f ? fmin(limit / (double)output_peak, 1.0) : 1.0;
        g.limiter_next_len--;
        g.limiter_next_pos[g.limiter_next_iter] = -1;
        g.limiter_next_iter = (g.limiter_next_iter + 1) % buffer_size;
    }

    /* Numerical guard rails copied from af_alimiter: keep the envelope
       finite and avoid spending time accumulating sub-ULP deltas. */
    if (g.limiter_gain > 1.0) {
        g.limiter_gain = 1.0;
        g.limiter_delta = 0.0;
        g.limiter_next_iter = 0;
        g.limiter_next_len = 0;
        g.limiter_next_pos[0] = -1;
    }
    if (g.limiter_gain <= 0.0) {
        g.limiter_gain = 0.0000000000001;
        g.limiter_delta = (1.0 - g.limiter_gain) / ((double)g.rate * release);
    }
    if (g.limiter_gain != 1.0 && (1.0 - g.limiter_gain) < 0.0000000000001)
        g.limiter_gain = 1.0;
    if (g.limiter_delta != 0.0 && fabs(g.limiter_delta) < 0.00000000000001)
        g.limiter_delta = 0.0;

    *l = clampf(*l, -ceiling, ceiling);
    *r = clampf(*r, -ceiling, ceiling);
    atomic_store(&g.levels[4], (float)g.limiter_gain);
    atomic_store(&g.levels[5], (float)(release * 1000.0));
    g.look_pos = output_pos;
}

static void wav_header(FILE *f, uint32_t bytes, int rate) {
    uint16_t one=1, channels=2, bits=16, align=4; uint32_t riff=36+bytes, byte_rate=rate*4, fmt=16;
    fseek(f,0,SEEK_SET); fwrite("RIFF",1,4,f); fwrite(&riff,4,1,f); fwrite("WAVEfmt ",1,8,f); fwrite(&fmt,4,1,f);
    fwrite(&one,2,1,f); fwrite(&channels,2,1,f); fwrite(&rate,4,1,f); fwrite(&byte_rate,4,1,f); fwrite(&align,2,1,f); fwrite(&bits,2,1,f); fwrite("data",1,4,f); fwrite(&bytes,4,1,f);
}
static void record_samples(const float *dry, const float *wet, int frames) {
    if (!atomic_load_explicit(&g.recording, memory_order_relaxed) || !g.record_blocks || frames <= 0) return;
    atomic_fetch_add_explicit(&g.record_producers, 1, memory_order_acquire);
    if (!atomic_load_explicit(&g.recording, memory_order_relaxed) || !g.record_blocks) {
        atomic_fetch_sub_explicit(&g.record_producers, 1, memory_order_release);
        return;
    }
    if (frames > MAX_FRAMES) frames = MAX_FRAMES;
    uint32_t write = atomic_load_explicit(&g.record_write_block, memory_order_relaxed);
    uint32_t read = atomic_load_explicit(&g.record_read_block, memory_order_acquire);
    if (write - read >= RECORD_QUEUE_BLOCKS) {
        atomic_fetch_add_explicit(&g.record_drops, 1, memory_order_relaxed);
        atomic_fetch_sub_explicit(&g.record_producers, 1, memory_order_release);
        return;
    }
    RecordBlock *block = &g.record_blocks[write % RECORD_QUEUE_BLOCKS];
    block->frames = frames;
    for (int i = 0; i < frames * 2; ++i) {
        block->dry[i] = (int16_t)(clampf(dry[i], -1.f, 1.f) * 32767.f);
        block->wet[i] = (int16_t)(clampf(wet[i], -1.f, 1.f) * 32767.f);
    }
    atomic_store_explicit(&g.record_write_block, write + 1, memory_order_release);
    pthread_cond_signal(&g.record_cv);
    atomic_fetch_sub_explicit(&g.record_producers, 1, memory_order_release);
}

static uint64_t now_ns(void);

static void write_record_block(const RecordBlock *block) {
    if (!block) return;
    if (g.dry_file) { fwrite(block->dry, sizeof(int16_t), (size_t)block->frames * 2u, g.dry_file); g.dry_bytes += (uint32_t)block->frames * 4u; }
    if (g.wet_file) { fwrite(block->wet, sizeof(int16_t), (size_t)block->frames * 2u, g.wet_file); g.wet_bytes += (uint32_t)block->frames * 4u; }
    atomic_fetch_add_explicit(&g.record_written_frames, (unsigned int)block->frames, memory_order_relaxed);
}

static void *record_worker(void *unused) {
    (void)unused;
    for (;;) {
        uint32_t read = atomic_load_explicit(&g.record_read_block, memory_order_relaxed);
        uint32_t write = atomic_load_explicit(&g.record_write_block, memory_order_acquire);
        if (read == write) {
            if (!atomic_load_explicit(&g.record_worker_running, memory_order_acquire)) break;
            pthread_mutex_lock(&g.record_cv_mutex);
            if (atomic_load_explicit(&g.record_worker_running, memory_order_acquire) &&
                atomic_load_explicit(&g.record_read_block, memory_order_relaxed) ==
                atomic_load_explicit(&g.record_write_block, memory_order_acquire)) {
                struct timespec deadline;
                clock_gettime(CLOCK_REALTIME, &deadline);
                deadline.tv_nsec += 2000000L;
                if (deadline.tv_nsec >= 1000000000L) { deadline.tv_nsec -= 1000000000L; deadline.tv_sec++; }
                pthread_cond_timedwait(&g.record_cv, &g.record_cv_mutex, &deadline);
            }
            pthread_mutex_unlock(&g.record_cv_mutex);
            continue;
        }
        RecordBlock *block = &g.record_blocks[read % RECORD_QUEUE_BLOCKS];
        write_record_block(block);
        atomic_store_explicit(&g.record_read_block, read + 1, memory_order_release);
    }
    return NULL;
}

static void stop_recording_internal(void) {
    atomic_store_explicit(&g.recording, 0, memory_order_release);
    while (atomic_load_explicit(&g.record_producers, memory_order_acquire) != 0) usleep(100);
    atomic_store_explicit(&g.record_worker_running, 0, memory_order_release);
    pthread_cond_broadcast(&g.record_cv);
    if (atomic_exchange(&g.record_worker_started, 0)) pthread_join(g.record_thread, NULL);
    pthread_mutex_lock(&g.file_lock);
    int rate = g.record_rate > 0 ? g.record_rate : (g.rate > 0 ? g.rate : 48000);
    if (g.dry_file) { wav_header(g.dry_file, g.dry_bytes, rate); fclose(g.dry_file); g.dry_file = NULL; }
    if (g.wet_file) { wav_header(g.wet_file, g.wet_bytes, rate); fclose(g.wet_file); g.wet_file = NULL; }
    pthread_mutex_unlock(&g.file_lock);
    free(g.record_blocks); g.record_blocks = NULL;
    atomic_store(&g.record_written_frames, 0);
}
static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec*1000000000ull + (uint64_t)t.tv_nsec; }
static void network_set_error(int code, int detail0, int detail1) {
    atomic_store(&g.net_error_detail0, detail0);
    atomic_store(&g.net_error_detail1, detail1);
    atomic_store(&g.net_error, code);
}

static void network_configure_udp_buffer(int fd, int role) {
    if(fd<0)return;
    int option=role==2?SO_RCVBUF:SO_SNDBUF;
    int requested=role==2?NET_UDP_RECEIVE_BUFFER_BYTES:NET_UDP_SEND_BUFFER_BYTES;
    if(setsockopt(fd,SOL_SOCKET,option,&requested,sizeof(requested))<0) {
        LOGI("Wi-Fi UDP %s buffer request failed: bytes=%d errno=%d",
             role==2?"receive":"send",requested,errno);
    }
    int actual=0;
    socklen_t actual_size=sizeof(actual);
    if(getsockopt(fd,SOL_SOCKET,option,&actual,&actual_size)==0) {
        LOGI("Wi-Fi UDP %s buffer: requested=%d actual=%d",
             role==2?"receive":"send",requested,actual);
    }
}

static void network_configure_tcp_buffer(int fd, int role) {
    if(fd<0)return;
    int option=role==2?SO_RCVBUF:SO_SNDBUF;
    int requested=role==2?NET_TCP_RECEIVE_BUFFER_BYTES:NET_TCP_SEND_BUFFER_BYTES;
    if(setsockopt(fd,SOL_SOCKET,option,&requested,sizeof(requested))<0) {
        LOGI("Wi-Fi TCP %s buffer request failed: bytes=%d errno=%d",
             role==2?"receive":"send",requested,errno);
    }
    int actual=0;
    socklen_t actual_size=sizeof(actual);
    if(getsockopt(fd,SOL_SOCKET,option,&actual,&actual_size)==0) {
        LOGI("Wi-Fi TCP %s buffer: requested=%d actual=%d",
             role==2?"receive":"send",requested,actual);
    }
}

static void network_tcp_open_sender(void) {
    if(g.net_transport!=NET_TRANSPORT_TCP||atomic_load(&g.net_role)!=1||g.net_sock>=0)return;
    uint64_t now=now_ns();
    if(now<g.net_tcp_next_connect_ns)return;
    atomic_fetch_add(&g.net_tcp_connect_attempts,1);
    g.net_sock=socket(AF_INET,SOCK_STREAM,0);
    if(g.net_sock<0){int error=errno;network_set_error(NET_ERROR_SOCKET,error,0);LOGI("Wi-Fi TCP sender socket failed errno=%d",error);g.net_tcp_next_connect_ns=now+1000000000ull;return;}
    network_configure_tcp_buffer(g.net_sock,1);
    fcntl(g.net_sock,F_SETFL,O_NONBLOCK);
    int rc=connect(g.net_sock,(struct sockaddr*)&g.net_addr,sizeof(g.net_addr));
    if(rc==0) {
        g.net_tcp_connecting=0;
        g.net_tcp_next_connect_ns=0;
        network_set_error(NET_ERROR_NONE,0,0);
        LOGI("Wi-Fi TCP sender connected");
    } else if(errno==EINPROGRESS) {
        g.net_tcp_connecting=1;
    } else {
        int error=errno;
        network_set_error(NET_ERROR_CONNECT,error,g.net_port);
        LOGI("Wi-Fi TCP sender connect failed errno=%d",error);
        close(g.net_sock);g.net_sock=-1;g.net_tcp_connecting=0;
        g.net_tcp_next_connect_ns=now+1000000000ull;
    }
}
static void network_tcp_flush(void) {
    if (g.net_transport != NET_TRANSPORT_TCP || g.net_sock < 0) return;
    if (atomic_exchange(&g.net_tx_disconnect_requested,0)) {
        shutdown(g.net_sock,SHUT_RDWR); close(g.net_sock); g.net_sock=-1;
        g.net_tcp_connecting=0; g.net_tx_used=g.net_tx_offset=0;
        g.net_tx_blocked_since_ns=0;
        return;
    }
    if (g.net_tcp_connecting) {
        struct pollfd pfd={g.net_sock,POLLOUT|POLLERR|POLLHUP,0};
        int ready=poll(&pfd,1,0);
        if(ready<=0)return;
        g.net_tcp_connecting=0;
    }
    int error=0; socklen_t error_len=sizeof(error);
    if (getsockopt(g.net_sock,SOL_SOCKET,SO_ERROR,&error,&error_len)==0 && error!=0) {
        network_set_error(NET_ERROR_CONNECT,error,g.net_port);
        LOGI("Wi-Fi TCP sender connection error errno=%d",error);
        close(g.net_sock);g.net_sock=-1;g.net_tcp_connecting=0;
        g.net_tcp_next_connect_ns=now_ns()+1000000000ull;
        g.net_tx_offset=g.net_tx_used=0;
        g.net_tx_blocked_since_ns=0;
        return;
    }
    network_set_error(NET_ERROR_NONE,0,0);
    while (g.net_tx_offset < g.net_tx_used) {
        ssize_t n=send(g.net_sock,g.net_tx_buffer+g.net_tx_offset,
                       g.net_tx_used-g.net_tx_offset,MSG_DONTWAIT);
        if(n>0) { g.net_tx_offset+=(size_t)n; g.net_tx_blocked_since_ns=0; continue; }
        if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR)) break;
        int error=errno;
        network_set_error(NET_ERROR_CONNECT,error,g.net_port);
        LOGI("Wi-Fi TCP sender send failed errno=%d",error);
        close(g.net_sock); g.net_sock=-1; g.net_tcp_connecting=0;
        g.net_tcp_next_connect_ns=now_ns()+1000000000ull;
        g.net_tx_offset=g.net_tx_used;
        g.net_tx_blocked_since_ns=0;
        break;
    }
    if(g.net_tx_offset==g.net_tx_used) {
        g.net_tx_used=g.net_tx_offset=0;
        g.net_tx_blocked_since_ns=0;
    } else if(g.net_tx_blocked_since_ns==0) {
        g.net_tx_blocked_since_ns=now_ns();
    }
}
static void network_write_packet(const uint8_t *packet, size_t size) {
    if(!packet||size==0)return;
    if(g.net_transport==NET_TRANSPORT_TCP) {
        network_tcp_open_sender();
        if(g.net_sock<0)return;
        uint32_t wire_len=htonl((uint32_t)size);
        size_t total=size+sizeof(wire_len);
        network_tcp_flush();
        if(total>sizeof(g.net_tx_buffer)-g.net_tx_used)return;
        memcpy(g.net_tx_buffer+g.net_tx_used,&wire_len,sizeof(wire_len));
        memcpy(g.net_tx_buffer+g.net_tx_used+sizeof(wire_len),packet,size);
        g.net_tx_used+=total;
        if(g.net_tx_blocked_since_ns==0)g.net_tx_blocked_since_ns=now_ns();
        network_tcp_flush();
    } else {
        if(g.net_sock<0)return;
        ssize_t sent=sendto(g.net_sock,packet,size,0,(struct sockaddr*)&g.net_addr,sizeof(g.net_addr));
        if(sent<0)network_set_error(NET_ERROR_CONNECT,errno,g.net_port);
        else network_set_error(NET_ERROR_NONE,0,0);
    }
}
static void network_send_pcm_packet(const float *data, int frames) {
    NetHeader h={NET_MAGIC,NET_PROTOCOL_VERSION,0,2,(uint32_t)g.rate,
                 (uint32_t)frames,now_ns(),g.net_seq};
    g.net_seq+=(uint32_t)(frames/NET_PACKET_FRAMES);
    uint8_t packet[sizeof(NetHeader)+NET_PCM_MAX_PACKET_FRAMES*2*sizeof(float)];
    size_t packet_size=sizeof(NetHeader)+(size_t)frames*2*sizeof(float);
    memcpy(packet,&h,sizeof(h));
    memcpy(packet+sizeof(h),data,(size_t)frames*2*sizeof(float));
    if(g.net_transport==NET_TRANSPORT_TCP) {
        network_write_packet(packet,packet_size);
    } else {
        ssize_t expected=(ssize_t)packet_size;
        ssize_t sent=sendto(g.net_sock,packet,packet_size,0,(struct sockaddr*)&g.net_addr,sizeof(g.net_addr));
        if(sent!=expected && (h.sequence==0 || h.sequence%100==0))
        LOGI("Wi-Fi send dropped sequence=%u bytes=%zd/%zd",h.sequence,sent,expected);
    }
    if(h.sequence==0)
        LOGI("Wi-Fi protocol v%d PCM packet: frames=%d bytes=%zu",NET_PROTOCOL_VERSION,
             frames,packet_size);
}

static void network_flush_aac_packet(void) {
    if(g.net_aac_send_frames<=0||g.net_aac_send_used==0)return;
    uint8_t packet[sizeof(NetHeader)+NET_AAC_STORED_PACKET_BYTES];
    int frames=g.net_aac_send_frames*NET_AAC_FRAMES;
    NetHeader h={NET_MAGIC,NET_PROTOCOL_VERSION,1,2,(uint32_t)g.rate,
                 (uint32_t)frames,now_ns(),g.net_seq};
    g.net_seq+=(uint32_t)(frames/NET_PACKET_FRAMES);
    memcpy(packet,&h,sizeof(h));
    memcpy(packet+sizeof(h),g.net_aac_send_buffer,g.net_aac_send_used);
    size_t packet_size=sizeof(h)+g.net_aac_send_used;
    network_write_packet(packet,packet_size);
    if(h.sequence==0)
        LOGI("Wi-Fi AAC protocol v%d: frames=%d bitrate=%d bytes=%zu",NET_PROTOCOL_VERSION,
             frames,g.net_bitrate,packet_size);
    g.net_aac_send_used=0;
    g.net_aac_send_frames=0;
}

static void network_append_aac_frame(const float *data) {
    uint8_t encoded_data[NET_AAC_MAX_PACKET_BYTES];
    pthread_mutex_lock(&g.net_codec_lock);
    int encoded=wifi_aac_encode(g.net_aac,data,NET_AAC_FRAMES,
                                encoded_data,NET_AAC_MAX_PACKET_BYTES);
    pthread_mutex_unlock(&g.net_codec_lock);
    if(encoded<=0)return;
    size_t framed_size=sizeof(uint32_t)+(size_t)encoded;
    size_t transport_limit=g.net_transport==NET_TRANSPORT_UDP
            ? NET_UDP_MAX_PAYLOAD_BYTES-sizeof(NetHeader)
            : NET_AAC_STORED_PACKET_BYTES;
    if(g.net_aac_send_frames>0&&g.net_aac_send_used+framed_size>transport_limit) {
        network_flush_aac_packet();
    }
    if(framed_size>sizeof(g.net_aac_send_buffer))return;
    uint32_t wire_size=htonl((uint32_t)encoded);
    memcpy(g.net_aac_send_buffer+g.net_aac_send_used,&wire_size,sizeof(wire_size));
    memcpy(g.net_aac_send_buffer+g.net_aac_send_used+sizeof(wire_size),encoded_data,(size_t)encoded);
    g.net_aac_send_used+=framed_size;
    g.net_aac_send_frames++;
    if(g.net_aac_send_frames>=g.net_aac_frames_per_packet)network_flush_aac_packet();
}

static void network_send(const float *data, int frames) {
    if (atomic_load(&g.net_role) != 1 || !data || frames<=0) return;
    if (atomic_exchange(&g.net_tx_disconnect_requested,0)) {
        if(g.net_sock>=0){shutdown(g.net_sock,SHUT_RDWR);close(g.net_sock);}
        g.net_sock=-1; g.net_tcp_connecting=0;
        g.net_tx_used=g.net_tx_offset=0; g.net_tx_blocked_since_ns=0;
    }
    if (g.net_transport != NET_TRANSPORT_TCP && g.net_sock < 0) return;
    int offset=0;
    while(offset<frames) {
        int packet_frames=g.net_codec==1?NET_AAC_FRAMES:g.net_pcm_packet_frames;
        int room=packet_frames-g.net_send_count;
        int take=frames-offset<room?frames-offset:room;
        memcpy(g.net_send_buffer+g.net_send_count*2,data+offset*2,(size_t)take*2*sizeof(float));
        g.net_send_count+=take;
        offset+=take;
        if(g.net_send_count==packet_frames) {
            if(g.net_codec==1)network_append_aac_frame(g.net_send_buffer);
            else network_send_pcm_packet(g.net_send_buffer,packet_frames);
            g.net_send_count=0;
        }
    }
}

static void network_plc_reset(void);

static void network_clear_jitter_state(void) {
    if(g.net_jitter) memset(g.net_jitter,0,NET_JITTER_SLOTS*sizeof(*g.net_jitter));
    if(g.net_aac_jitter) memset(g.net_aac_jitter,0,NET_AAC_JITTER_SLOTS*sizeof(*g.net_aac_jitter));
    g.net_packet_count=0;
    g.net_started=0;
    g.net_play_offset=0;
    g.net_seq_initialized=0;
    g.net_play_seq=0;
    g.net_high_seq=0;
    g.net_aac_packet_count=0;
    g.net_aac_started=0;
    g.net_aac_seq_initialized=0;
    g.net_aac_play_seq=0;
    g.net_aac_high_seq=0;
    g.net_buffer_monitor_started=0;
    g.net_min_boundary_latched=0;
    g.net_max_boundary_latched=0;
    g.net_max_since_ns=0;
    atomic_store(&g.net_buffer_ms,0);
    network_plc_reset();
}

static void network_plc_reset(void) {
    g.net_plc_history_write=0;
    g.net_plc_history_count=0;
    g.net_plc_active=0;
    g.net_plc_pitch_lag=0;
    g.net_plc_generated_frames=0;
    g.net_plc_recovery_frames=0;
    g.net_plc_recovery_total=0;
}

static float network_plc_history_sample(int back, int channel) {
    if(back<1||back>g.net_plc_history_count)return 0.f;
    int index=g.net_plc_history_write-back;
    if(index<0)index+=NET_PLC_HISTORY_FRAMES;
    return g.net_plc_history[index*2+channel];
}

static void network_plc_record_real(const float *data, int frames) {
    if(!data||frames<=0)return;
    if(frames>=NET_PLC_HISTORY_FRAMES) {
        data += (size_t)(frames-NET_PLC_HISTORY_FRAMES)*2u;
        frames=NET_PLC_HISTORY_FRAMES;
    }
    int first=NET_PLC_HISTORY_FRAMES-g.net_plc_history_write;
    if(first>frames)first=frames;
    memcpy(g.net_plc_history+(size_t)g.net_plc_history_write*2u,
           data,(size_t)first*2u*sizeof(float));
    int remaining=frames-first;
    if(remaining>0) {
        memcpy(g.net_plc_history,data+(size_t)first*2u,
               (size_t)remaining*2u*sizeof(float));
    }
    g.net_plc_history_write=(g.net_plc_history_write+frames)%NET_PLC_HISTORY_FRAMES;
    g.net_plc_history_count+=frames;
    if(g.net_plc_history_count>NET_PLC_HISTORY_FRAMES)
        g.net_plc_history_count=NET_PLC_HISTORY_FRAMES;
}

static double network_plc_lag_score(int lag, int window, int stride) {
    double cross=0.0,current_energy=1e-12,delayed_energy=1e-12;
    for(int i=0;i<window;i+=stride) {
        float current_left=network_plc_history_sample(i+1,0);
        float current_right=network_plc_history_sample(i+1,1);
        float delayed_left=network_plc_history_sample(i+1+lag,0);
        float delayed_right=network_plc_history_sample(i+1+lag,1);
        cross+=(double)current_left*delayed_left+(double)current_right*delayed_right;
        current_energy+=(double)current_left*current_left+(double)current_right*current_right;
        delayed_energy+=(double)delayed_left*delayed_left+(double)delayed_right*delayed_right;
    }
    return cross/sqrt(current_energy*delayed_energy);
}

static int network_plc_estimate_pitch(void) {
    int rate=g.rate>0?g.rate:48000;
    int min_lag=rate/400;
    int max_lag=rate/60;
    if(min_lag<16)min_lag=16;
    if(max_lag>g.net_plc_history_count/2)max_lag=g.net_plc_history_count/2;
    int window=rate/100;
    if(window>g.net_plc_history_count-max_lag)window=g.net_plc_history_count-max_lag;
    if(max_lag<=min_lag||window<64) {
        int fallback=rate/200;
        if(fallback>g.net_plc_history_count)fallback=g.net_plc_history_count;
        return fallback>0?fallback:1;
    }
    int best_lag=min_lag;
    double best_score=-1.0;
    for(int lag=min_lag;lag<=max_lag;lag+=4) {
        double score=network_plc_lag_score(lag,window,4);
        if(score>best_score){best_score=score;best_lag=lag;}
    }
    int refine_start=best_lag-3>min_lag?best_lag-3:min_lag;
    int refine_end=best_lag+3<max_lag?best_lag+3:max_lag;
    for(int lag=refine_start;lag<=refine_end;lag++) {
        double score=network_plc_lag_score(lag,window,2);
        if(score>best_score){best_score=score;best_lag=lag;}
    }
    if(best_score<.35) {
        best_lag=rate/200;
        if(best_lag>g.net_plc_history_count)best_lag=g.net_plc_history_count;
    }
    return best_lag>0?best_lag:1;
}

static void network_plc_begin(void) {
    if(g.net_plc_active)return;
    g.net_plc_active=1;
    g.net_plc_pitch_lag=network_plc_estimate_pitch();
    g.net_plc_generated_frames=0;
    g.net_plc_recovery_frames=0;
    g.net_plc_recovery_total=0;
}

static void network_plc_next(float *left, float *right) {
    if(!left||!right||g.net_plc_history_count==0){if(left)*left=0.f;if(right)*right=0.f;return;}
    int rate=g.rate>0?g.rate:48000;
    int lag=g.net_plc_pitch_lag>0?g.net_plc_pitch_lag:1;
    int back=lag-(g.net_plc_generated_frames%lag);
    float gain=1.f;
    int hold_frames=rate/50;
    int fade_frames=rate/5;
    if(g.net_plc_generated_frames>hold_frames) {
        float progress=(float)(g.net_plc_generated_frames-hold_frames)/(float)(fade_frames-hold_frames);
        if(progress>=1.f)gain=0.f;
        else gain=.5f+.5f*cosf((float)M_PI*progress);
    }
    *left=network_plc_history_sample(back,0)*gain;
    *right=network_plc_history_sample(back,1)*gain;
    g.net_plc_generated_frames++;
}

static void network_plc_conceal(float *data, int frames) {
    if(!data||frames<=0)return;
    if(g.net_plc_history_count==0){memset(data,0,(size_t)frames*2*sizeof(float));return;}
    network_plc_begin();
    g.net_plc_recovery_frames=0;
    g.net_plc_recovery_total=0;
    for(int i=0;i<frames;i++)network_plc_next(&data[i*2],&data[i*2+1]);
}

static void network_plc_output_real(float *output, const float *real, int frames) {
    if(!output||!real||frames<=0)return;
    if(!g.net_plc_active) {
        memcpy(output,real,(size_t)frames*2*sizeof(float));
        network_plc_record_real(real,frames);
        return;
    }
    for(int i=0;i<frames;i++) {
        float left=real[i*2],right=real[i*2+1];
        if(g.net_plc_active) {
            if(g.net_plc_recovery_total==0) {
                g.net_plc_recovery_total=(g.rate>0?g.rate:48000)/200;
                if(g.net_plc_recovery_total<32)g.net_plc_recovery_total=32;
            }
            float predicted_left,predicted_right;
            network_plc_next(&predicted_left,&predicted_right);
            float progress=(float)(g.net_plc_recovery_frames+1)/(float)g.net_plc_recovery_total;
            if(progress>1.f)progress=1.f;
            float real_mix=.5f-.5f*cosf((float)M_PI*progress);
            output[i*2]=predicted_left*(1.f-real_mix)+left*real_mix;
            output[i*2+1]=predicted_right*(1.f-real_mix)+right*real_mix;
            g.net_plc_recovery_frames++;
            if(g.net_plc_recovery_frames>=g.net_plc_recovery_total) {
                g.net_plc_active=0;
                g.net_plc_generated_frames=0;
                g.net_plc_recovery_frames=0;
                g.net_plc_recovery_total=0;
            }
        } else {
            output[i*2]=left;
            output[i*2+1]=right;
        }
        network_plc_record_real(real+i*2,1);
    }
}

static void network_restart_jitter(uint32_t sequence) {
    network_clear_jitter_state();
    g.net_seq_initialized=1;
    g.net_play_seq=sequence;
    g.net_high_seq=sequence;
}

static int network_valid_frames(void) {
    int frames=g.net_packet_count*NET_PACKET_FRAMES;
    if(g.net_seq_initialized) {
        NetJitterSlot *slot=&g.net_jitter[g.net_play_seq%NET_JITTER_SLOTS];
        if(slot->valid && slot->sequence==g.net_play_seq) frames-=g.net_play_offset;
    }
    return frames>0?frames:0;
}

static int network_span_frames(void) {
    if(!g.net_seq_initialized) return 0;
    int32_t packets=(int32_t)(g.net_high_seq-g.net_play_seq)+1;
    if(packets<=0) return 0;
    int64_t frames=(int64_t)packets*NET_PACKET_FRAMES-g.net_play_offset;
    return frames>INT32_MAX?INT32_MAX:(frames>0?(int)frames:0);
}

static int network_violation_sustained(uint64_t *since_ns, int hold_ms, int violated, uint64_t now) {
    if (!violated) {
        *since_ns = 0;
        return 0;
    }
    if (*since_ns == 0) *since_ns = now;
    if (hold_ms <= 0) return 1;
    return now >= *since_ns && now - *since_ns >= (uint64_t)hold_ms * 1000000ull;
}

static void network_update_buffer_monitor(int allow_events) {
    int valid_frames=network_valid_frames();
    int span_frames=network_span_frames();
    int rate=g.rate>0?g.rate:48000;
    int buffer_ms=(int)(((int64_t)valid_frames*1000ll)/rate);
    atomic_store(&g.net_buffer_ms,buffer_ms);
    if(!allow_events||!g.net_buffer_monitor_started)return;
    int min_frames=(rate*g.net_min_ms)/1000;
    int max_frames=(rate*g.net_max_ms)/1000;
    uint64_t now=now_ns();
    int at_max=network_violation_sustained(&g.net_max_since_ns,g.net_max_hold_ms,max_frames>0&&span_frames>=max_frames,now);
    int at_min=valid_frames<=min_frames;
    if(at_min&&!g.net_min_boundary_latched)atomic_fetch_add(&g.net_min_buffer_events,1);
    if(at_max&&!g.net_max_boundary_latched)atomic_fetch_add(&g.net_max_buffer_events,1);
    g.net_min_boundary_latched=at_min;
    g.net_max_boundary_latched=at_max;
}

static void network_drop_until(uint32_t sequence) {
    int32_t packets=(int32_t)(sequence-g.net_play_seq);
    if(packets<=0) return;
    if(packets>NET_JITTER_SLOTS) packets=NET_JITTER_SLOTS;
    while(packets-->0) {
        NetJitterSlot *slot=&g.net_jitter[g.net_play_seq%NET_JITTER_SLOTS];
        if(slot->valid && slot->sequence==g.net_play_seq) {
            slot->valid=0;
            g.net_packet_count--;
        }
        g.net_play_seq++;
    }
    g.net_play_offset=0;
}

static void network_store_packet(const NetHeader *h, const float *samples, uint64_t arrival_ns) {
    uint64_t previous_arrival=atomic_load(&g.net_last_packet_ns);
    if(!g.net_seq_initialized) {
        network_restart_jitter(h->sequence);
    } else {
        int32_t delta=(int32_t)(h->sequence-g.net_play_seq);
        if(delta<0 && !g.net_started && g.net_play_offset==0 &&
           delta>=-NET_REORDER_BACKTRACK_PACKETS) {
            g.net_play_seq=h->sequence;
            delta=0;
        } else if(delta<0 && previous_arrival>0 && arrival_ns-previous_arrival>500000000ull) {
            network_restart_jitter(h->sequence);
            delta=0;
        } else if(delta<0) {
            g.net_late_packets++;
            if(g.net_late_packets==1 || g.net_late_packets%100==0)
                LOGI("Wi-Fi late packet count=%llu sequence=%u expected=%u",
                     (unsigned long long)g.net_late_packets,h->sequence,g.net_play_seq);
            return;
        }
        if(delta>=NET_JITTER_SLOTS) {
            LOGI("Wi-Fi sequence jump: sequence=%u expected=%u",h->sequence,g.net_play_seq);
            network_restart_jitter(h->sequence);
        }
    }

    NetJitterSlot *slot=&g.net_jitter[h->sequence%NET_JITTER_SLOTS];
    if(slot->valid && slot->sequence==h->sequence) {
        g.net_duplicate_packets++;
        if(g.net_duplicate_packets==1 || g.net_duplicate_packets%100==0)
            LOGI("Wi-Fi duplicate packet count=%llu sequence=%u",
                 (unsigned long long)g.net_duplicate_packets,h->sequence);
        return;
    }
    if(slot->valid) {
        slot->valid=0;
        g.net_packet_count--;
    }
    slot->sequence=h->sequence;
    memcpy(slot->samples,samples,NET_PACKET_FRAMES*2*sizeof(float));
    slot->valid=1;
    g.net_packet_count++;
    if((int32_t)(h->sequence-g.net_high_seq)>0) g.net_high_seq=h->sequence;
}

static void network_store_aac_packet(const NetHeader *h, const uint8_t *data, uint32_t size) {
    if(!g.net_aac_jitter||!data||size==0||size>NET_AAC_STORED_PACKET_BYTES)return;
    uint32_t end_sequence=h->sequence+h->frames/NET_PACKET_FRAMES-NET_AAC_FRAMES/NET_PACKET_FRAMES;
    if(!g.net_aac_seq_initialized) {
        g.net_aac_seq_initialized=1;
        g.net_aac_play_seq=h->sequence;
        g.net_aac_high_seq=end_sequence;
    } else {
        int32_t delta=(int32_t)(h->sequence-g.net_aac_play_seq);
        if(delta<0&&!g.net_aac_started&&delta>=-(NET_AAC_MAX_FRAMES_PER_PACKET*NET_AAC_FRAMES/NET_PACKET_FRAMES)) {
            g.net_aac_play_seq=h->sequence;
            delta=0;
        } else if(delta < -(int32_t)(NET_AAC_JITTER_SLOTS *
                                     (NET_AAC_FRAMES / NET_PACKET_FRAMES))) {
            /* A sender may restart its sequence at zero while changing
             * bitrate. Treat a large backwards jump as a new stream rather
             * than dropping every packet as late. */
            LOGI("Wi-Fi AAC sequence restart: sequence=%u expected=%u",
                 h->sequence, g.net_aac_play_seq);
            network_clear_jitter_state();
            g.net_aac_seq_initialized=1;
            g.net_aac_play_seq=h->sequence;
            g.net_aac_high_seq=end_sequence;
            delta=0;
        } else if(delta<0) {
            g.net_late_packets++;
            return;
        }
        if(delta>=(int32_t)(NET_AAC_JITTER_SLOTS*(NET_AAC_FRAMES/NET_PACKET_FRAMES))) {
            memset(g.net_aac_jitter,0,NET_AAC_JITTER_SLOTS*sizeof(*g.net_aac_jitter));
            g.net_aac_packet_count=0;g.net_aac_started=0;g.net_aac_play_seq=h->sequence;g.net_aac_high_seq=end_sequence;
        }
    }
    uint32_t packet_index=h->sequence/(NET_AAC_FRAMES/NET_PACKET_FRAMES);
    NetAacJitterSlot *slot=&g.net_aac_jitter[packet_index%NET_AAC_JITTER_SLOTS];
    if(slot->valid&&slot->sequence==h->sequence){g.net_duplicate_packets++;return;}
    if(slot->valid)g.net_aac_packet_count--;
    slot->sequence=h->sequence;slot->size=size;slot->frames=h->frames;memcpy(slot->data,data,size);slot->valid=1;
    g.net_aac_packet_count++;
    if((int32_t)(end_sequence-g.net_aac_high_seq)>0)g.net_aac_high_seq=end_sequence;
}

static void network_decode_aac_packets(void) {
    if(!g.net_aac_seq_initialized||!g.net_aac_jitter)return;
    int target_frames=((g.rate*g.net_min_ms)/1000+(g.rate*g.net_max_ms)/1000)/2;
    int target_packets=(target_frames+NET_AAC_FRAMES-1)/NET_AAC_FRAMES;
    if(target_packets<1)target_packets=1;
    int span=(int32_t)(g.net_aac_high_seq-g.net_aac_play_seq)/(NET_AAC_FRAMES/NET_PACKET_FRAMES)+1;
    if(!g.net_aac_started){if(span<target_packets)return;g.net_aac_started=1;}
    float decoded[NET_AAC_FRAMES*2];
    while(g.net_aac_packet_count>0) {
        uint32_t packet_index=g.net_aac_play_seq/(NET_AAC_FRAMES/NET_PACKET_FRAMES);
        NetAacJitterSlot *slot=&g.net_aac_jitter[packet_index%NET_AAC_JITTER_SLOTS];
        if(!slot->valid||slot->sequence!=g.net_aac_play_seq) {
            if((int32_t)(g.net_aac_high_seq-g.net_aac_play_seq)>0) {
                g.net_aac_play_seq+=NET_AAC_FRAMES/NET_PACKET_FRAMES;
                continue;
            }
            break;
        }
        uint32_t sequence=slot->sequence;
        uint32_t packet_frames=slot->frames;
        size_t payload_offset=0;
        int access_units=(int)(packet_frames/NET_AAC_FRAMES);
        slot->valid=0;g.net_aac_packet_count--;
        g.net_aac_play_seq+=(uint32_t)(packet_frames/NET_PACKET_FRAMES);
        if(!g.net_seq_initialized) {
            g.net_seq_initialized=1;
            g.net_play_seq=sequence;
            g.net_high_seq=sequence;
        }
        for(int unit=0;unit<access_units;unit++) {
            if(slot->size-payload_offset<sizeof(uint32_t)) {
                network_set_error(NET_ERROR_AAC_PACKET_SIZE,(int)slot->size,access_units);
                break;
            }
            uint32_t wire_size;
            memcpy(&wire_size,slot->data+payload_offset,sizeof(wire_size));
            uint32_t encoded_size=ntohl(wire_size);
            payload_offset+=sizeof(wire_size);
            if(encoded_size==0||encoded_size>slot->size-payload_offset) {
                network_set_error(NET_ERROR_AAC_PACKET_SIZE,(int)encoded_size,(int)(slot->size-payload_offset));
                break;
            }
            pthread_mutex_lock(&g.net_codec_lock);
            int decoded_frames=wifi_aac_decode(g.net_aac,slot->data+payload_offset,(int)encoded_size,decoded,NET_AAC_FRAMES);
            pthread_mutex_unlock(&g.net_codec_lock);
            payload_offset+=encoded_size;
            if(decoded_frames!=NET_AAC_FRAMES){
                network_set_error(NET_ERROR_AAC_DECODE,decoded_frames,NET_AAC_FRAMES);
                continue;
            }
            uint32_t unit_sequence=sequence+(uint32_t)(unit*NET_AAC_FRAMES/NET_PACKET_FRAMES);
            for(int offset=0;offset<NET_AAC_FRAMES;offset+=NET_PACKET_FRAMES) {
                NetHeader slice={NET_MAGIC,NET_PROTOCOL_VERSION,1,2,(uint32_t)g.rate,
                                 NET_PACKET_FRAMES,0,unit_sequence+(uint32_t)(offset/NET_PACKET_FRAMES)};
                network_store_packet(&slice,decoded+offset*2,now_ns());
            }
        }
    }
}

static void network_process_packet(const uint8_t *packet, size_t n) {
    if(!packet||n<sizeof(NetHeader)){
        network_set_error(NET_ERROR_PACKET_TOO_SHORT,(int)n,(int)sizeof(NetHeader));
        return;
    }
    NetHeader h; memcpy(&h,packet,sizeof(h));
    if(h.magic!=NET_MAGIC){network_set_error(NET_ERROR_MAGIC_MISMATCH,(int)h.magic,(int)NET_MAGIC);return;}
    if(h.version!=NET_PROTOCOL_VERSION){network_set_error(NET_ERROR_VERSION_MISMATCH,h.version,NET_PROTOCOL_VERSION);return;}
    if(h.codec!=(uint8_t)g.net_codec){network_set_error(NET_ERROR_CODEC_MISMATCH,h.codec,g.net_codec);return;}
    if(h.channels!=2){network_set_error(NET_ERROR_CHANNEL_MISMATCH,h.channels,2);return;}
    if(h.rate!=(uint32_t)g.rate){network_set_error(NET_ERROR_RATE_MISMATCH,(int)h.rate,g.rate);return;}
    uint64_t arrival=now_ns();
    if(h.codec==0) {
        if(h.frames<NET_PACKET_FRAMES||h.frames>NET_PCM_MAX_PACKET_FRAMES||h.frames%NET_PACKET_FRAMES!=0){network_set_error(NET_ERROR_FRAMES_MISMATCH,(int)h.frames,NET_PACKET_FRAMES);return;}
        size_t expected=sizeof(NetHeader)+(size_t)h.frames*2*sizeof(float);
        if(n!=expected){
            size_t payload=n-sizeof(NetHeader);
            size_t samples=(size_t)h.frames*h.channels;
            int bytes_per_sample=samples>0&&payload%samples==0?(int)(payload/samples):0;
            network_set_error(NET_ERROR_PCM_FORMAT_MISMATCH,bytes_per_sample,(int)sizeof(float));
            return;
        }
        for(uint32_t offset=0;offset<h.frames;offset+=NET_PACKET_FRAMES) {
            NetHeader slice=h;
            slice.frames=NET_PACKET_FRAMES;
            slice.sequence=h.sequence+offset/NET_PACKET_FRAMES;
            network_store_packet(&slice,(const float *)(packet+sizeof(h))+offset*2,arrival);
        }
    } else if(h.codec==1) {
        if(h.frames<NET_AAC_FRAMES||h.frames>NET_AAC_MAX_FRAMES_PER_PACKET*NET_AAC_FRAMES||h.frames%NET_AAC_FRAMES!=0){network_set_error(NET_ERROR_FRAMES_MISMATCH,(int)h.frames,NET_AAC_FRAMES);return;}
        if(n<=sizeof(NetHeader)||n-sizeof(NetHeader)>NET_AAC_STORED_PACKET_BYTES){
            network_set_error(NET_ERROR_AAC_PACKET_SIZE,(int)(n-sizeof(NetHeader)),NET_AAC_STORED_PACKET_BYTES);
            return;
        }
        network_store_aac_packet(&h,packet+sizeof(NetHeader),
                                 (uint32_t)(n-sizeof(NetHeader)));
    } else return;
    atomic_store(&g.net_last_packet_ns,arrival);
    if(!atomic_exchange(&g.net_packet_seen,1))
        LOGI("first valid Wi-Fi packet: protocol=%u codec=%u rate=%u frames=%u bytes=%zu",
             h.version,h.codec,h.rate,h.frames,n);
    atomic_store(&g.net_rx_timeout_reported,0);
    network_set_error(NET_ERROR_NONE,0,0);
}

static void network_fill(void) {
    if (atomic_load(&g.net_role) != 2 || !g.net_jitter) return;
    if (atomic_exchange(&g.net_rx_disconnect_requested,0)) {
        if(g.net_sock>=0){shutdown(g.net_sock,SHUT_RDWR);close(g.net_sock);}
        g.net_sock=-1; g.net_rx_used=0;
    }
    if(g.net_transport==NET_TRANSPORT_TCP) {
        if(g.net_sock<0 && g.net_listen_sock>=0) {
            int accepted=accept(g.net_listen_sock,NULL,NULL);
            if(accepted>=0) { network_configure_tcp_buffer(accepted,2); fcntl(accepted,F_SETFL,O_NONBLOCK); g.net_sock=accepted; g.net_rx_used=0; network_set_error(NET_ERROR_TCP_WAITING_DATA,0,0); LOGI("Wi-Fi TCP client connected"); }
        }
        if(g.net_sock>=0) {
            for(;;) {
                if(g.net_rx_used==sizeof(g.net_rx_buffer))break;
                ssize_t n=recv(g.net_sock,g.net_rx_buffer+g.net_rx_used,
                               sizeof(g.net_rx_buffer)-g.net_rx_used,MSG_DONTWAIT);
                if(n>0){g.net_rx_used+=(size_t)n;continue;}
                if(n==0){network_set_error(NET_ERROR_PEER_DISCONNECTED,0,0);close(g.net_sock);g.net_sock=-1;g.net_rx_used=0;break;}
                if(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR)break;
                network_set_error(NET_ERROR_TCP_RECEIVE,errno,0);
                close(g.net_sock);g.net_sock=-1;g.net_rx_used=0;break;
            }
            size_t consumed=0;
            while(g.net_rx_used-consumed>=sizeof(uint32_t)) {
                uint32_t wire_len; memcpy(&wire_len,g.net_rx_buffer+consumed,sizeof(wire_len));
                size_t packet_len=ntohl(wire_len);
                if(packet_len<sizeof(NetHeader)||packet_len>sizeof(g.net_rx_buffer)-sizeof(uint32_t)) { network_set_error(NET_ERROR_TCP_FRAMING,(int)packet_len,(int)(sizeof(g.net_rx_buffer)-sizeof(uint32_t))); consumed=g.net_rx_used; break; }
                if(g.net_rx_used-consumed<sizeof(uint32_t)+packet_len)break;
                network_process_packet(g.net_rx_buffer+consumed+sizeof(uint32_t),packet_len);
                consumed+=sizeof(uint32_t)+packet_len;
            }
            if(consumed){memmove(g.net_rx_buffer,g.net_rx_buffer+consumed,g.net_rx_used-consumed);g.net_rx_used-=consumed;}
        }
        if(g.net_codec==1)network_decode_aac_packets();
        return;
    }
    if (g.net_sock < 0) return;
    uint8_t packet[sizeof(NetHeader)+NET_PCM_MAX_PACKET_FRAMES*2*sizeof(float)];
    for (;;) {
        ssize_t n=recvfrom(g.net_sock,packet,sizeof(packet),MSG_DONTWAIT,NULL,NULL);
        if(n<0){if(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR)network_set_error(NET_ERROR_TCP_RECEIVE,errno,0);break;}
        if(n<(ssize_t)sizeof(NetHeader)){network_set_error(NET_ERROR_PACKET_TOO_SHORT,(int)n,(int)sizeof(NetHeader));break;}
        network_process_packet(packet,(size_t)n);
    }
    if(g.net_codec==1)network_decode_aac_packets();
}

static int network_receive(float *data, int frames) {
    network_fill();
    if ((g.net_sock < 0 && !(g.net_transport==NET_TRANSPORT_TCP && g.net_listen_sock>=0)) ||
        atomic_load(&g.net_role) != 2 || !g.net_jitter) {
        atomic_store(&g.net_buffer_ms,0);
        network_plc_conceal(data,frames);
        return frames;
    }
    int min_frames=(g.rate*g.net_min_ms)/1000;
    int max_frames=(g.rate*g.net_max_ms)/1000;
    int target_frames=(min_frames+max_frames)/2;
    if(target_frames<1) target_frames=1;
    if(max_frames<target_frames) max_frames=target_frames;
    if(!g.net_started) {
        if(network_valid_frames()>=target_frames) {
            g.net_started=1;
            g.net_buffer_monitor_started=1;
        } else {
            network_update_buffer_monitor(0);
            network_plc_conceal(data,frames);
            return frames;
        }
    }
    network_update_buffer_monitor(1);
    uint64_t violation_now=now_ns();
    int max_sustained=network_violation_sustained(&g.net_max_since_ns,g.net_max_hold_ms,
                                                  network_span_frames()>=max_frames,violation_now);
    if(max_sustained) {
        int keep_packets=(target_frames+NET_PACKET_FRAMES-1)/NET_PACKET_FRAMES;
        uint32_t new_play_seq=g.net_high_seq-(uint32_t)(keep_packets-1);
        network_drop_until(new_play_seq);
        network_update_buffer_monitor(1);
    }
    int valid_frames=network_valid_frames();
    if(valid_frames==0||valid_frames<min_frames) {
        g.net_started=0;
        network_update_buffer_monitor(1);
        network_plc_conceal(data,frames);
        return frames;
    }

    int produced=0;
    while(produced<frames) {
        NetJitterSlot *slot=&g.net_jitter[g.net_play_seq%NET_JITTER_SLOTS];
        if(slot->valid && slot->sequence==g.net_play_seq) {
            int available=NET_PACKET_FRAMES-g.net_play_offset;
            int take=frames-produced<available?frames-produced:available;
            network_plc_output_real(data+produced*2,slot->samples+g.net_play_offset*2,take);
            produced+=take;
            g.net_play_offset+=take;
            if(g.net_play_offset==NET_PACKET_FRAMES) {
                slot->valid=0;
                g.net_packet_count--;
                g.net_play_seq++;
                g.net_play_offset=0;
            }
            continue;
        }
        if((int32_t)(g.net_high_seq-g.net_play_seq)>0) {
            int missing=NET_PACKET_FRAMES-g.net_play_offset;
            int take=frames-produced<missing?frames-produced:missing;
            network_plc_conceal(data+produced*2,take);
            produced+=take;
            g.net_play_offset+=take;
            if(g.net_play_offset==NET_PACKET_FRAMES) {
                g.net_missing_packets++;
                if(g.net_missing_packets==1 || g.net_missing_packets%100==0)
                    LOGI("Wi-Fi missing packet count=%llu sequence=%u",
                         (unsigned long long)g.net_missing_packets,g.net_play_seq);
                g.net_play_seq++;
                g.net_play_offset=0;
            }
            continue;
        }
        network_plc_conceal(data+produced*2,frames-produced);
        break;
    }
    if(network_valid_frames()==0)g.net_started=0;
    network_update_buffer_monitor(1);
    return frames;
}

static void *audio_thread(void *unused) {
    (void)unused;
    if (setpriority(PRIO_PROCESS, 0, -16) != 0)
        LOGI("Audio DSP thread priority request failed: errno=%d", errno);
    float input[MAX_FRAMES*MAX_INPUT_CHANNELS], dry[MAX_FRAMES*2], output[MAX_FRAMES*2], p[28];
    while(atomic_load(&g.running)) {
        uint64_t block_begin_us = now_ns() / 1000ull;
        int want=g.frames<MAX_FRAMES?g.frames:MAX_FRAMES;
        aaudio_result_t got;
        int using_network=atomic_load(&g.use_network_input)&&atomic_load(&g.net_role)==2;
        int using_tone=atomic_load(&g.tone_enabled);
        if(using_network){ got=network_receive(output,want); if(got<=0){ memset(output,0,(size_t)want*2*sizeof(float)); got=want; } memcpy(dry,output,(size_t)got*2*sizeof(float)); }
        else if(g.usb_input_host && g.usb_audio){got=usb_host_audio_read(g.usb_audio,input,want);}
        else if(g.input){if(atomic_load(&g.net_role)==2)network_fill();got=input_ring_read(input,(uint32_t)want);}
        else if(using_tone){ got=want; }
        else { memset(input,0,(size_t)want*MAX_INPUT_CHANNELS*sizeof(float)); got=want; }
        if(got<=0){usleep(1000);continue;}
        int tone_waveform, tone_music, tone_channels;
        float tone_frequency, tone_frequency2, tone_duration_seconds, tone_click_interval_ms, tone_level;
        pthread_mutex_lock(&g.param_lock);
        memcpy(p,g.values,sizeof(p));
        tone_waveform=g.tone_waveform;
        tone_music=g.tone_music;
        tone_channels=g.tone_channels;
        tone_frequency=g.tone_frequency;
        tone_frequency2=g.tone_frequency2;
        tone_duration_seconds=g.tone_duration_seconds;
        tone_click_interval_ms=g.tone_click_interval_ms;
        tone_level=g.tone_level;
        pthread_mutex_unlock(&g.param_lock);
        int flags=atomic_load(&g.flags); float peaks[4]={0.f,0.f,0.f,0.f};
        if (!(flags&DSP_ON) || !(flags&LIMITER_ON)) {
            g.limiter_delay_frames=0;
            atomic_store(&g.levels[4],1.f);
            atomic_store(&g.levels[5],fmaxf(p[P_RELEASE],10.f));
        }
        if (!(flags & DSP_ON) || !(flags & LOUDNESS_ON)) loudness_reset();
        ConvolutionReverb *reverb=NULL;
        if((flags&REVERB_ON) && (flags&DSP_ON)) {
            pthread_mutex_lock(&g.reverb_lock);
            reverb=g.reverb;
        }
        for(int i=0;i<got;i++) {
            int first=g.pair*2; if(first>=g.in_channels)first=0;
            int second=first+1; if(second>=g.in_channels)second=first;
            float l, r;
            if (using_network) { l=output[i*2]; r=output[i*2+1]; }
            else if (using_tone) {
                double phase=g.tone_phase;
                double value;
                switch(tone_waveform) {
                    case 1: value=phase<0.5?1.0:-1.0; break;
                    case 2: value=4.0*fabs(phase-0.5)-1.0; break;
                    case 3:
                        g.tone_noise_state = g.tone_noise_state * 1664525u + 1013904223u;
                        value = ((double)(g.tone_noise_state >> 8) / 16777216.0) * 2.0 - 1.0;
                        break;
                    case 4: {
                        uint64_t sweep_frames=(uint64_t)fmax(1.0,(double)tone_duration_seconds*(double)g.rate);
                        double progress=(double)(g.tone_frame_counter%sweep_frames)/(double)sweep_frames;
                        double start=fmax(1.0,(double)tone_frequency);
                        double end=fmax(start,(double)tone_frequency2);
                        double sweep_frequency=start*pow(end/start,progress);
                        value=sin(phase*6.283185307179586);
                        phase+=sweep_frequency/(double)g.rate;
                        break;
                    }
                    case 5: {
                        uint64_t interval=(uint64_t)fmax(1.0,(double)tone_click_interval_ms*(double)g.rate/1000.0);
                        value=(g.tone_frame_counter%interval)==0?1.0:0.0;
                        break;
                    }
                    case 6: {
                        double phase2=g.tone_phase2;
                        value=0.5*(sin(phase*6.283185307179586)+sin(phase2*6.283185307179586));
                        phase2+=(double)tone_frequency2/(double)g.rate;
                        phase2-=floor(phase2);
                        g.tone_phase2=phase2;
                        break;
                    }
                    case 7:
                        value=test_music_render(&g.tone_music_state,tone_music,g.rate);
                        break;
                    default: value=sin(phase*6.283185307179586); break;
                }
                value*=clampf(tone_level,0.f,1.f);
                l=(tone_channels==2)?0.f:(float)value;
                r=(tone_channels==1)?0.f:(float)value;
                if(tone_waveform!=4&&tone_waveform!=5&&tone_waveform!=7)phase += (double)tone_frequency/(double)g.rate;
                phase -= floor(phase);
                g.tone_phase=phase;
                g.tone_frame_counter++;
            } else { l=g.usb_input_host ? input[i*2] : input[i*g.in_channels+first]; r=g.usb_input_host ? input[i*2+1] : input[i*g.in_channels+second]; }
            dry[i*2]=l;dry[i*2+1]=r;
            peaks[0]=fmaxf(peaks[0],fabsf(l)); peaks[1]=fmaxf(peaks[1],fabsf(r));
            if(flags&DSP_ON) {
                if(flags&EQ_ON)for(int b=0;b<EQ_BANDS;b++){biquad_config(&g.eq[0][b],g.rate,p[b*3],p[b*3+1],p[b*3+2]);biquad_config(&g.eq[1][b],g.rate,p[b*3],p[b*3+1],p[b*3+2]);l=biquad_process(&g.eq[0][b],l);r=biquad_process(&g.eq[1][b],r);}
                if((flags&REVERB_ON) && reverb){float wet_l=0.f,wet_r=0.f,mix=clampf(p[P_MIX]/100.f,0.f,1.f);convolution_reverb_process(reverb,l,r,&wet_l,&wet_r);l=l*(1.f-mix)+wet_l*mix;r=r*(1.f-mix)+wet_r*mix;}
                if(flags&LOUDNESS_ON) loudness_process(&l,&r,p);
                if(flags&LIMITER_ON)limiter_process(&l,&r,p);
            }
            output[i*2]=l;output[i*2+1]=r;peaks[2]=fmaxf(peaks[2],fabsf(l));peaks[3]=fmaxf(peaks[3],fabsf(r));
        }
        if(reverb) pthread_mutex_unlock(&g.reverb_lock);
        for(int i=0;i<4;i++)atomic_store(&g.levels[i],atomic_load(&g.levels[i])*.84f+peaks[i]*.16f);
        uint64_t peak_now=now_ns();
        for(int i=0;i<4;i++) {
            float held=atomic_load(&g.peak_levels[i]);
            if(peaks[i]>=held) { atomic_store(&g.peak_levels[i],peaks[i]); g.peak_hold_until_ns[i]=peak_now+1500000000ull; }
            else if(peak_now>g.peak_hold_until_ns[i]) atomic_store(&g.peak_levels[i],held*.985f);
        }
        pthread_mutex_lock(&g.waveform_lock);
        int wave_start=got>512?got-512:0;
        for(int i=wave_start;i<got;i++) {
            int pos=g.waveform_pos;
            g.waveform_dry[pos]=(dry[i*2]+dry[i*2+1])*0.5f;
            g.waveform_wet[pos]=(output[i*2]+output[i*2+1])*0.5f;
            g.waveform_pos=(pos+1)&511;
            if(g.waveform_count<512)g.waveform_count++;
        }
        pthread_mutex_unlock(&g.waveform_lock);
        if(g.net_role==1) network_send(output,got);
        if (g.usb_output_host && g.usb_audio) usb_host_audio_write(g.usb_audio, output, got);
        else if (g.output) output_ring_write(output, (uint32_t)got);
        record_samples(dry,output,got);
        unsigned long long block_us = (unsigned long long)(now_ns() / 1000ull - block_begin_us);
        atomic_store_explicit(&g.dsp_last_us, block_us, memory_order_relaxed);
        unsigned long long old_max = atomic_load_explicit(&g.dsp_max_us, memory_order_relaxed);
        while (old_max < block_us && !atomic_compare_exchange_weak_explicit(
                &g.dsp_max_us, &old_max, block_us, memory_order_relaxed, memory_order_relaxed)) {}
    }
    return NULL;
}

static int open_stream(AAudioStream **stream, aaudio_direction_t direction, int channels, int device, int rate, int frames) {
    if (!stream) return 0;
    *stream = NULL;
    AAudioStreamBuilder *b=NULL;
    aaudio_result_t create_result = AAudio_createStreamBuilder(&b);
    if(create_result!=AAUDIO_OK || !b) { LOGE("AAudio_createStreamBuilder failed: %d", create_result); return 0; }
    AAudioStreamBuilder_setDirection(b,direction); AAudioStreamBuilder_setFormat(b,AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setSampleRate(b,rate); AAudioStreamBuilder_setChannelCount(b,channels);
    AAudioStreamBuilder_setPerformanceMode(b,AAUDIO_PERFORMANCE_MODE_LOW_LATENCY); AAudioStreamBuilder_setSharingMode(b,AAUDIO_SHARING_MODE_EXCLUSIVE);
    if (direction == AAUDIO_DIRECTION_OUTPUT) {
        typedef void (*set_usage_fn)(AAudioStreamBuilder *, aaudio_usage_t);
        typedef void (*set_content_type_fn)(AAudioStreamBuilder *, aaudio_content_type_t);
        set_usage_fn set_usage = (set_usage_fn)dlsym(RTLD_DEFAULT, "AAudioStreamBuilder_setUsage");
        set_content_type_fn set_content_type = (set_content_type_fn)dlsym(RTLD_DEFAULT, "AAudioStreamBuilder_setContentType");
        if (set_usage) set_usage(b, AAUDIO_USAGE_GAME);
        if (set_content_type) set_content_type(b, AAUDIO_CONTENT_TYPE_MUSIC);
        AAudioStreamBuilder_setDataCallback(b, output_data_callback, &g);
        AAudioStreamBuilder_setErrorCallback(b, output_error_callback, &g);
    } else {
        AAudioStreamBuilder_setDataCallback(b, input_data_callback, &g);
        AAudioStreamBuilder_setErrorCallback(b, input_error_callback, &g);
    }
    if(device>=0)AAudioStreamBuilder_setDeviceId(b,device);
    aaudio_result_t result=AAudioStreamBuilder_openStream(b,stream);
    if(result!=AAUDIO_OK || !*stream){
        if (*stream) { AAudioStream_close(*stream); *stream = NULL; }
        AAudioStreamBuilder_setSharingMode(b,AAUDIO_SHARING_MODE_SHARED);
        result=AAudioStreamBuilder_openStream(b,stream);
    }
    AAudioStreamBuilder_delete(b);
    if (result != AAUDIO_OK || !*stream) {
        LOGE("AAudioStreamBuilder_openStream failed: %d", result);
        if (*stream) { AAudioStream_close(*stream); *stream = NULL; }
        return 0;
    }
    if (direction == AAUDIO_DIRECTION_OUTPUT) {
        g.output_rate = AAudioStream_getSampleRate(*stream);
        int burst = AAudioStream_getFramesPerBurst(*stream);
        int capacity = AAudioStream_getBufferCapacityInFrames(*stream);
        int requested_buffer = burst > 0 ? burst * 2 : frames * 2;
        if (capacity > 0 && requested_buffer > capacity) requested_buffer = capacity;
        aaudio_result_t actual_buffer = AAudioStream_setBufferSizeInFrames(*stream, requested_buffer);
        if (actual_buffer < 0) LOGE("AAudio output buffer size request failed: %d", actual_buffer);
        if (!output_ring_create(burst, frames, g.rate)) {
            LOGE("AAudio output ring allocation failed");
            AAudioStream_close(*stream);
            *stream = NULL;
            return 0;
        }
        atomic_store(&g.output_error, AAUDIO_OK);
        LOGI("AAudio output opened: rate=%d channels=%d performance=%d sharing=%d burst=%d buffer=%d capacity=%d device=%d ring=%u prefill=%u limit=%u",
             AAudioStream_getSampleRate(*stream), AAudioStream_getChannelCount(*stream),
             AAudioStream_getPerformanceMode(*stream), AAudioStream_getSharingMode(*stream), burst,
             AAudioStream_getBufferSizeInFrames(*stream), capacity, AAudioStream_getDeviceId(*stream),
             g.output_ring_capacity, atomic_load(&g.output_prefill_frames),
             atomic_load(&g.output_queue_limit));
    } else {
        g.in_channels = AAudioStream_getChannelCount(*stream);
        if (g.in_channels < 1 || g.in_channels > MAX_INPUT_CHANNELS) {
            LOGE("AAudio input channel count unsupported: %d", g.in_channels);
            AAudioStream_close(*stream);
            *stream = NULL;
            return 0;
        }
        int burst = AAudioStream_getFramesPerBurst(*stream);
        int capacity = AAudioStream_getBufferCapacityInFrames(*stream);
        int requested_buffer = burst > 0 ? burst * 2 : frames * 2;
        if (capacity > 0 && requested_buffer > capacity) requested_buffer = capacity;
        aaudio_result_t actual_buffer = AAudioStream_setBufferSizeInFrames(*stream, requested_buffer);
        if (actual_buffer < 0) LOGE("AAudio input buffer size request failed: %d", actual_buffer);
        if (!input_ring_create(burst, frames)) {
            LOGE("AAudio input ring allocation failed");
            AAudioStream_close(*stream);
            *stream = NULL;
            return 0;
        }
        atomic_store(&g.input_error, AAUDIO_OK);
        LOGI("AAudio input opened: rate=%d channels=%d performance=%d sharing=%d burst=%d buffer=%d capacity=%d device=%d ring=%u",
             AAudioStream_getSampleRate(*stream), AAudioStream_getChannelCount(*stream),
             AAudioStream_getPerformanceMode(*stream), AAudioStream_getSharingMode(*stream), burst,
             AAudioStream_getBufferSizeInFrames(*stream), capacity, AAudioStream_getDeviceId(*stream),
             g.input_ring_capacity);
    }
    return 1;
}

JNIEXPORT jboolean JNICALL Java_com_llawsxx_audioprocess_NativeAudio_start(JNIEnv*e,jobject o,jint rate,jint outputRate,jint frames,jint inDev,jint outDev,jboolean enableOutput,jint channels,jint pair,jboolean useNetworkInput,jint usbFd,jboolean usbInputHost,jboolean usbOutputHost,jint usbInputBitDepth,jint usbOutputBitDepth,jint usbInputBurstPackets,jint usbOutputBurstPackets){
    (void)e;(void)o;
    if(atomic_load(&g.running))return JNI_TRUE;
    int input_started=0, output_started=0, audio_thread_started=0;
    input_ring_destroy();
    output_ring_destroy();
    atomic_store(&g.dsp_last_us, 0);
    atomic_store(&g.dsp_max_us, 0);
    for(int i=0;i<4;i++){atomic_store(&g.peak_levels[i],0.f);g.peak_hold_until_ns[i]=0;}
    pthread_mutex_lock(&g.waveform_lock);
    memset(g.waveform_dry,0,sizeof(g.waveform_dry));
    memset(g.waveform_wet,0,sizeof(g.waveform_wet));
    g.waveform_count=g.waveform_pos=0;
    pthread_mutex_unlock(&g.waveform_lock);
    memset(g.eq,0,sizeof(g.eq));g.rate=rate;g.output_rate=outputRate>0?outputRate:rate;g.output_resample_phase=0.0;g.frames=frames;g.in_channels=channels;g.pair=pair;g.limiter_gain=1.0;g.limiter_delta=0.0;g.limiter_delay_frames=0;g.limiter_next_iter=g.limiter_next_len=0;loudness_init(rate);g.usb_audio=NULL;g.usb_input_host=usbInputHost?1:0;g.usb_output_host=usbOutputHost?1:0;if(usbFd<0){g.usb_input_host=0;g.usb_output_host=0;}atomic_store(&g.use_network_input,useNetworkInput?1:0);if(useNetworkInput)atomic_store(&g.net_last_packet_ns,now_ns());
    atomic_store(&g.reverb_worker_started,0);
    if(usbFd >= 0 && (g.usb_input_host || g.usb_output_host)) {
        g.usb_audio = usb_host_audio_start(usbFd, rate, rate, usbInputBitDepth, g.output_rate, usbOutputBitDepth, g.usb_input_host, g.usb_output_host,
                                           g.usb_buffer_max_ms,
                                           frames,
                                           usbInputBurstPackets < 1 ? 1 : (usbInputBurstPackets > 128 ? 128 : usbInputBurstPackets),
                                           usbOutputBurstPackets < 1 ? 1 : (usbOutputBurstPackets > 128 ? 128 : usbOutputBurstPackets));
        if (!g.usb_audio) { LOGE("USB Host audio initialization failed"); return JNI_FALSE; }
        usb_host_audio_configure_input_buffer(g.usb_audio, atomic_load(&g.usb_input_buffer_max_ms));
        if (g.usb_input_host) g.in_channels = 2;
    }
    if(!g.usb_input_host && !atomic_load(&g.use_network_input) && !atomic_load(&g.tone_enabled) && !open_stream(&g.input,AAUDIO_DIRECTION_INPUT,channels,inDev,rate,frames)){LOGE("AAudio input open failed");usb_host_audio_stop(g.usb_audio);g.usb_audio=NULL;return JNI_FALSE;}
    if(enableOutput && !g.usb_output_host && !open_stream(&g.output,AAUDIO_DIRECTION_OUTPUT,2,outDev,g.output_rate,frames)){LOGE("AAudio output open failed");if(g.input){AAudioStream_close(g.input);g.input=NULL;}input_ring_destroy();usb_host_audio_stop(g.usb_audio);g.usb_audio=NULL;return JNI_FALSE;}
    g.reverb=convolution_reverb_create(rate,g.values[P_ROOM],g.values[P_DECAY],g.values[P_DAMP]);
    g.net_jitter=calloc(NET_JITTER_SLOTS,sizeof(*g.net_jitter));
    g.net_aac_jitter=calloc(NET_AAC_JITTER_SLOTS,sizeof(*g.net_aac_jitter));
    network_clear_jitter_state();
    g.look_pos=0;g.look_size=(int)(rate*.006f)*2+4;g.lookahead=calloc((size_t)g.look_size,sizeof(float));
    g.limiter_next_pos=malloc((size_t)g.look_size*sizeof(*g.limiter_next_pos));
    g.limiter_next_delta=calloc((size_t)g.look_size,sizeof(*g.limiter_next_delta));
    if (!g.reverb || !g.lookahead || !g.limiter_next_pos || !g.limiter_next_delta || !g.net_jitter || !g.net_aac_jitter) { LOGE("DSP or network buffer allocation failed"); goto fail; }
    atomic_store(&g.running,1);
    if (pthread_create(&g.thread,NULL,audio_thread,NULL) != 0) {
        LOGE("audio thread creation failed");
        atomic_store(&g.running,0);
        goto fail;
    }
    audio_thread_started=1;
    if(g.output) {
        aaudio_result_t output_start_result=AAudioStream_requestStart(g.output);
        if(output_start_result!=AAUDIO_OK) { LOGE("AAudio output requestStart failed: %d",output_start_result); goto fail; }
        output_started=1;
    }
    if(g.input) {
        aaudio_result_t input_start_result=AAudioStream_requestStart(g.input);
        if(input_start_result!=AAUDIO_OK) { LOGE("AAudio input requestStart failed: %d",input_start_result); goto fail; }
        input_started=1;
    }
    atomic_store(&g.reverb_worker_running,1);
    if(pthread_create(&g.reverb_thread,NULL,reverb_worker,NULL)==0) atomic_store(&g.reverb_worker_started,1);
    else { atomic_store(&g.reverb_worker_running,0); LOGE("reverb worker creation failed; live IR updates disabled"); }
    return JNI_TRUE;
fail:
    atomic_store(&g.running,0);
    atomic_store(&g.tone_enabled,0);
    usb_host_audio_request_stop(g.usb_audio);
    if (input_started && g.input) AAudioStream_requestStop(g.input);
    if (audio_thread_started) pthread_join(g.thread,NULL);
    if (output_started && g.output) AAudioStream_requestStop(g.output);
    if (g.input) { AAudioStream_close(g.input); g.input = NULL; }
    if (g.output) { AAudioStream_close(g.output); g.output = NULL; }
    convolution_reverb_destroy(g.reverb); g.reverb=NULL;
    usb_host_audio_stop(g.usb_audio); g.usb_audio=NULL;
    free(g.lookahead); g.lookahead=NULL;
    free(g.limiter_next_pos); g.limiter_next_pos=NULL;
    free(g.limiter_next_delta); g.limiter_next_delta=NULL;
    free(g.net_jitter); g.net_jitter=NULL;
    free(g.net_aac_jitter); g.net_aac_jitter=NULL;
    input_ring_destroy();
    output_ring_destroy();
    return JNI_FALSE;
}
/* Route changes restart AAudio/DSP, but the Wi-Fi transport can remain alive.
 * Keeping the socket avoids turning a transient input timeout into a server
 * teardown/reconnect cycle. A full stop still closes all network resources. */
static void native_stop_internal(int finalize_recording, int preserve_network) {
    int was_running = atomic_exchange(&g.running,0);
    atomic_store(&g.tone_enabled,0);
    /* A disconnected USB OUT endpoint no longer drains its ring. Wake a DSP
     * thread blocked in usb_host_audio_write() before waiting for that thread;
     * destroying the USB object happens only after the join, when callbacks
     * can no longer race the DSP thread. */
    usb_host_audio_request_stop(g.usb_audio);
    if (!was_running) {
        if (finalize_recording) stop_recording_internal();
        return;
    }
    if(g.input)AAudioStream_requestStop(g.input);
    pthread_join(g.thread,NULL);
    atomic_store(&g.reverb_worker_running,0);
    if(atomic_exchange(&g.reverb_worker_started,0))pthread_join(g.reverb_thread,NULL);
    pthread_mutex_lock(&g.stream_lock);
    if(g.output)AAudioStream_requestStop(g.output);
    if(g.input)AAudioStream_close(g.input);
    if(g.output)AAudioStream_close(g.output);
    g.input=g.output=NULL;
    pthread_mutex_unlock(&g.stream_lock);
    if (finalize_recording) stop_recording_internal();
    usb_host_audio_stop(g.usb_audio);g.usb_audio=NULL;g.usb_input_host=g.usb_output_host=0;
    pthread_mutex_lock(&g.reverb_lock);ConvolutionReverb *reverb=g.reverb;g.reverb=NULL;pthread_mutex_unlock(&g.reverb_lock);
    convolution_reverb_destroy(reverb);free(g.lookahead);g.lookahead=NULL;free(g.limiter_next_pos);g.limiter_next_pos=NULL;free(g.limiter_next_delta);g.limiter_next_delta=NULL;free(g.net_jitter);g.net_jitter=NULL;free(g.net_aac_jitter);g.net_aac_jitter=NULL;input_ring_destroy();output_ring_destroy();network_clear_jitter_state();
    if (!preserve_network) {
        network_close_socket(&g.net_sock);
        network_close_socket(&g.net_listen_sock);
        g.net_tcp_connecting=0;
        g.net_tcp_next_connect_ns=0;
        g.net_rx_used=g.net_tx_used=g.net_tx_offset=0;
        g.net_tx_blocked_since_ns=0;
        atomic_store(&g.net_rx_disconnect_requested,0);
        atomic_store(&g.net_rx_timeout_reported,0);
        atomic_store(&g.net_tx_disconnect_requested,0);
    }
}
JNIEXPORT void JNICALL Java_com_llawsxx_audioprocess_NativeAudio_stop(JNIEnv*e,jobject o){(void)e;(void)o;native_stop_internal(1,0);}
JNIEXPORT void JNICALL Java_com_llawsxx_audioprocess_NativeAudio_stopForRouteChange(JNIEnv*e,jobject o){(void)e;(void)o;native_stop_internal(0,1);}
JNIEXPORT void JNICALL Java_com_llawsxx_audioprocess_NativeAudio_update(JNIEnv*e,jobject o,jint flags,jfloatArray values){(void)o;float incoming[28]={0};jsize n=(*e)->GetArrayLength(e,values);if(n>28)n=28;(*e)->GetFloatArrayRegion(e,values,0,n,incoming);pthread_mutex_lock(&g.param_lock);int reverb_changed=n>P_DAMP&&(g.values[P_ROOM]!=incoming[P_ROOM]||g.values[P_DECAY]!=incoming[P_DECAY]||g.values[P_DAMP]!=incoming[P_DAMP]);memcpy(g.values,incoming,(size_t)n*sizeof(float));pthread_mutex_unlock(&g.param_lock);if(reverb_changed)atomic_fetch_add(&g.reverb_generation,1);atomic_store(&g.flags,flags);}
JNIEXPORT void JNICALL Java_com_llawsxx_audioprocess_NativeAudio_configureTone(JNIEnv*e,jobject o,jboolean enabled,jint waveform,jint music,jint channels,jfloat frequency,jfloat frequency2,jfloat durationSeconds,jfloat clickIntervalMs,jfloat level){
    (void)e;(void)o;
    float max_frequency=g.rate>0?(float)g.rate*.45f:20000.f;
    pthread_mutex_lock(&g.param_lock);
    int normalized_waveform=waveform<0?0:(waveform>7?7:waveform);
    int music_count=test_music_track_count();
    int normalized_music=music<0?0:(music>=music_count?music_count-1:music);
    int restart=g.tone_waveform!=normalized_waveform||g.tone_music!=normalized_music||(!atomic_load(&g.tone_enabled)&&enabled);
    if(restart){g.tone_phase=0.0;g.tone_phase2=0.0;g.tone_frame_counter=0;test_music_reset(&g.tone_music_state);}
    g.tone_waveform=normalized_waveform;
    g.tone_music=normalized_music;
    g.tone_channels=channels<0?0:(channels>2?2:channels);
    g.tone_frequency=frequency<1.f?1.f:(frequency>max_frequency?max_frequency:frequency);
    g.tone_frequency2=frequency2<1.f?1.f:(frequency2>max_frequency?max_frequency:frequency2);
    g.tone_duration_seconds=durationSeconds<1.f?1.f:(durationSeconds>60.f?60.f:durationSeconds);
    g.tone_click_interval_ms=clickIntervalMs<50.f?50.f:(clickIntervalMs>5000.f?5000.f:clickIntervalMs);
    g.tone_level=level<0.f?0.f:(level>1.f?1.f:level);
    pthread_mutex_unlock(&g.param_lock);
    atomic_store(&g.tone_enabled,enabled?1:0);
}
JNIEXPORT jfloatArray JNICALL Java_com_llawsxx_audioprocess_NativeAudio_levels(JNIEnv*e,jobject o){(void)o;jfloat v[10];for(int i=0;i<6;i++)v[i]=atomic_load(&g.levels[i]);for(int i=0;i<4;i++)v[6+i]=atomic_load(&g.peak_levels[i]);jfloatArray a=(*e)->NewFloatArray(e,10);(*e)->SetFloatArrayRegion(e,a,0,10,v);return a;}
JNIEXPORT jfloatArray JNICALL Java_com_llawsxx_audioprocess_NativeAudio_waveform(JNIEnv*e,jobject o){
    (void)o; jfloat v[1024]={0};
    pthread_mutex_lock(&g.waveform_lock);
    int n=g.waveform_count<512?g.waveform_count:512;
    int start=(g.waveform_pos-n+512)&511;
    int dst=512-n;
    for(int i=0;i<n;i++) {
        int src=(start+i)&511;
        v[dst+i]=g.waveform_dry[src];
        v[512+dst+i]=g.waveform_wet[src];
    }
    pthread_mutex_unlock(&g.waveform_lock);
    jfloatArray a=(*e)->NewFloatArray(e,1024); (*e)->SetFloatArrayRegion(e,a,0,1024,v); return a;
}
JNIEXPORT jboolean JNICALL Java_com_llawsxx_audioprocess_NativeAudio_configureNetwork(JNIEnv*e,jobject o,jint role,jint transport,jint codec,jint sampleRate,jint bitrate,jstring host,jint port,jint packetMs,jint minMs,jint maxMs,jint maxHoldMs){
    (void)o;
    if(role!=1&&role!=2){network_set_error(NET_ERROR_INVALID_ROLE,role,0);return JNI_FALSE;}
    if(transport!=NET_TRANSPORT_UDP&&transport!=NET_TRANSPORT_TCP){network_set_error(NET_ERROR_INVALID_TRANSPORT,transport,0);return JNI_FALSE;}
    if(codec!=0&&codec!=1){network_set_error(NET_ERROR_INVALID_CODEC,codec,0);return JNI_FALSE;}
    if(sampleRate!=44100&&sampleRate!=48000&&sampleRate!=96000){network_set_error(NET_ERROR_INVALID_SAMPLE_RATE,sampleRate,0);return JNI_FALSE;}
    if(port<1||port>65535){network_set_error(NET_ERROR_INVALID_PORT,port,0);return JNI_FALSE;}
    int normalized_rate=sampleRate;
    int normalized_bitrate=bitrate<32000?32000:(bitrate>1000000?1000000:bitrate);
    int normalized_packet_ms=packetMs<1?1:(packetMs>100?100:packetMs);
    int pcm_packet_frames=(normalized_rate*normalized_packet_ms/1000/NET_PACKET_FRAMES)*NET_PACKET_FRAMES;
    if(pcm_packet_frames<NET_PACKET_FRAMES)pcm_packet_frames=NET_PACKET_FRAMES;
    if(pcm_packet_frames>NET_PCM_MAX_PACKET_FRAMES)pcm_packet_frames=NET_PCM_MAX_PACKET_FRAMES;
    if(transport==NET_TRANSPORT_UDP) {
        int udp_max_frames=((NET_UDP_MAX_PAYLOAD_BYTES-(int)sizeof(NetHeader))/(2*(int)sizeof(float))/NET_PACKET_FRAMES)*NET_PACKET_FRAMES;
        if(pcm_packet_frames>udp_max_frames)pcm_packet_frames=udp_max_frames;
    }
    int aac_frames_per_packet=(normalized_rate*normalized_packet_ms+NET_AAC_FRAMES*500)/(NET_AAC_FRAMES*1000);
    if(aac_frames_per_packet<1)aac_frames_per_packet=1;
    if(aac_frames_per_packet>NET_AAC_MAX_FRAMES_PER_PACKET)aac_frames_per_packet=NET_AAC_MAX_FRAMES_PER_PACKET;
    wifi_aac_t next_aac=codec==1?wifi_aac_create(normalized_rate,normalized_bitrate):NULL;
    int codec_ready=codec==0||(next_aac&&wifi_aac_frame_length(next_aac)==NET_AAC_FRAMES);
    if(!codec_ready){wifi_aac_destroy(next_aac);network_set_error(NET_ERROR_AAC_INIT,normalized_rate,normalized_bitrate);LOGE("Wi-Fi AAC initialization failed rate=%d bitrate=%d",normalized_rate,normalized_bitrate);return JNI_FALSE;}
    const char*h=(*e)->GetStringUTFChars(e,host,NULL);
    struct in_addr requested_addr={0};
    if(!h||inet_pton(AF_INET,h,&requested_addr)!=1){
        if(h)(*e)->ReleaseStringUTFChars(e,host,h);
        wifi_aac_destroy(next_aac);
        network_set_error(NET_ERROR_INVALID_HOST,0,0);
        return JNI_FALSE;
    }
    int current_role=atomic_load(&g.net_role);
    int socket_ready=g.net_sock>=0 || (g.net_transport==NET_TRANSPORT_TCP&&g.net_listen_sock>=0);
    int same_transport=current_role==role && socket_ready && g.net_transport==transport &&
            g.net_codec==codec && g.rate==normalized_rate && g.net_port==port &&
            g.net_addr.sin_addr.s_addr==requested_addr.s_addr;
    int normalized_min=minMs<0?0:(minMs>200?200:minMs);
    int normalized_max=maxMs<50?50:(maxMs>1000?1000:maxMs);
    int normalized_max_hold=maxHoldMs<0?0:(maxHoldMs>60000?60000:maxHoldMs);
    if (normalized_max<normalized_min) normalized_min=normalized_max;
    if (same_transport) {
        /* Parameter/UI updates must not tear down a live transport. In
         * receive mode, bitrate is carried by each ADTS frame and the
         * decoder must stay alive; only a sender needs a new encoder. */
        g.net_min_ms=normalized_min;
        g.net_max_ms=normalized_max;
        g.net_max_hold_ms=normalized_max_hold;
        if(role==1&&(g.net_packet_ms!=normalized_packet_ms||g.net_pcm_packet_frames!=pcm_packet_frames||g.net_aac_frames_per_packet!=aac_frames_per_packet||g.net_bitrate!=normalized_bitrate)) {
            g.net_send_count=0;
            g.net_aac_send_used=0;
            g.net_aac_send_frames=0;
        }
        g.net_packet_ms=normalized_packet_ms;
        g.net_pcm_packet_frames=pcm_packet_frames;
        g.net_aac_frames_per_packet=aac_frames_per_packet;
        g.net_max_since_ns=0;
        if (codec==1 && role==1 && normalized_bitrate != g.net_bitrate) {
            pthread_mutex_lock(&g.net_codec_lock);
            wifi_aac_t previous=g.net_aac;
            g.net_aac=next_aac;
            g.net_bitrate=normalized_bitrate;
            pthread_mutex_unlock(&g.net_codec_lock);
            wifi_aac_destroy(previous);
            (*e)->ReleaseStringUTFChars(e,host,h);
            network_set_error(NET_ERROR_NONE,0,0);
            LOGI("Wi-Fi AAC bitrate updated in place: bitrate=%d", normalized_bitrate);
            return JNI_TRUE;
        }
        wifi_aac_destroy(next_aac);
        (*e)->ReleaseStringUTFChars(e,host,h);
        return JNI_TRUE;
    }
    pthread_mutex_lock(&g.net_codec_lock);
    wifi_aac_destroy(g.net_aac);g.net_aac=next_aac;
    pthread_mutex_unlock(&g.net_codec_lock);
    atomic_store(&g.net_role,0);
    network_close_socket(&g.net_sock);
    network_close_socket(&g.net_listen_sock);
    g.net_transport=transport;g.net_codec=codec;g.net_bitrate=normalized_bitrate;g.net_port=port;
    g.net_packet_ms=normalized_packet_ms;g.net_pcm_packet_frames=pcm_packet_frames;
    g.net_aac_frames_per_packet=aac_frames_per_packet;
    g.net_tcp_connecting=0;g.net_tcp_next_connect_ns=0;
    atomic_store(&g.net_tcp_connect_attempts,0);
    network_set_error(NET_ERROR_NONE,0,0);
    if(transport==NET_TRANSPORT_TCP) {
        if(role==2) {
            g.net_listen_sock=socket(AF_INET,SOCK_STREAM,0);
            if(g.net_listen_sock>=0) { int yes=1; setsockopt(g.net_listen_sock,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes)); network_configure_tcp_buffer(g.net_listen_sock,2); fcntl(g.net_listen_sock,F_SETFL,O_NONBLOCK); }
        } else {
            g.net_sock=socket(AF_INET,SOCK_STREAM,0);
            if(g.net_sock>=0) {
                network_configure_tcp_buffer(g.net_sock,1);
                atomic_fetch_add(&g.net_tcp_connect_attempts,1);
                struct sockaddr_in target={0};target.sin_family=AF_INET;target.sin_port=htons((uint16_t)port);target.sin_addr=requested_addr;
                fcntl(g.net_sock,F_SETFL,O_NONBLOCK);
                int rc=connect(g.net_sock,(struct sockaddr*)&target,sizeof(target));
                if(rc==0)g.net_tcp_connecting=0;
                else if(errno==EINPROGRESS)g.net_tcp_connecting=1;
                else {int error=errno;network_set_error(NET_ERROR_CONNECT,error,port);close(g.net_sock);g.net_sock=-1;g.net_tcp_next_connect_ns=now_ns()+1000000000ull;}
            }
        }
    } else {
        g.net_sock=socket(AF_INET,SOCK_DGRAM,0);
        if(g.net_sock>=0) {
            network_configure_udp_buffer(g.net_sock,role);
            fcntl(g.net_sock,F_SETFL,O_NONBLOCK);
        }
    }
    if((role==1&&g.net_sock<0)||(role==2&&transport==NET_TRANSPORT_TCP&&g.net_listen_sock<0)||(role==2&&transport==NET_TRANSPORT_UDP&&g.net_sock<0)){
        if(atomic_load(&g.net_error)==NET_ERROR_NONE)network_set_error(NET_ERROR_SOCKET,errno,0);
        LOGE("Wi-Fi socket creation failed role=%d transport=%d errno=%d",role,transport,errno);
        atomic_store(&g.net_role,0);(*e)->ReleaseStringUTFChars(e,host,h);return JNI_FALSE;
    }
    memset(&g.net_addr,0,sizeof(g.net_addr));g.net_addr.sin_family=AF_INET;g.net_addr.sin_port=htons((uint16_t)port);g.net_addr.sin_addr=requested_addr;
    atomic_store(&g.net_last_packet_ns,now_ns());atomic_store(&g.net_packet_seen,0);
    g.net_min_ms=normalized_min;g.net_max_ms=normalized_max;
    g.net_max_hold_ms=normalized_max_hold;
    g.net_send_count=0;g.net_aac_send_used=0;g.net_aac_send_frames=0;g.net_seq=0;g.net_tx_used=g.net_tx_offset=0;g.net_tx_blocked_since_ns=0;atomic_store(&g.net_rx_disconnect_requested,0);atomic_store(&g.net_rx_timeout_reported,0);atomic_store(&g.net_tx_disconnect_requested,0);g.net_missing_packets=g.net_late_packets=g.net_duplicate_packets=0;network_clear_jitter_state();
    atomic_store(&g.net_min_buffer_events,0);
    atomic_store(&g.net_max_buffer_events,0);
    if(role==2) {
        int bind_sock=transport==NET_TRANSPORT_TCP?g.net_listen_sock:g.net_sock;
        if(bind(bind_sock,(struct sockaddr*)&g.net_addr,sizeof(g.net_addr))<0) {
            int error=errno;
            network_set_error(error==EADDRINUSE?NET_ERROR_ADDRESS_IN_USE:NET_ERROR_BIND,port,error);
            LOGE("Wi-Fi bind failed host=%s port=%d errno=%d",h,port,error);
            network_close_socket(&g.net_sock);network_close_socket(&g.net_listen_sock);atomic_store(&g.net_role,0);(*e)->ReleaseStringUTFChars(e,host,h);return JNI_FALSE;
        }
        if(transport==NET_TRANSPORT_TCP&&listen(g.net_listen_sock,1)<0) {
            int error=errno;
            network_set_error(NET_ERROR_LISTEN,port,error);
            LOGE("Wi-Fi listen failed port=%d errno=%d",port,error);
            network_close_socket(&g.net_listen_sock);atomic_store(&g.net_role,0);(*e)->ReleaseStringUTFChars(e,host,h);return JNI_FALSE;
        }
        if(transport==NET_TRANSPORT_TCP) LOGI("Wi-Fi TCP server listening on port=%d",port);
        else LOGI("Wi-Fi UDP receiver bound to port=%d",port);
    }
    atomic_store(&g.net_role,role);(*e)->ReleaseStringUTFChars(e,host,h);return JNI_TRUE;
}
JNIEXPORT void JNICALL Java_com_llawsxx_audioprocess_NativeAudio_clearNetwork(JNIEnv*e,jobject o){(void)e;(void)o;atomic_store(&g.net_role,0);network_close_socket(&g.net_sock);network_close_socket(&g.net_listen_sock);g.net_tcp_connecting=0;g.net_tcp_next_connect_ns=0;g.net_rx_used=g.net_tx_used=g.net_tx_offset=0;g.net_tx_blocked_since_ns=0;atomic_store(&g.net_rx_disconnect_requested,0);atomic_store(&g.net_rx_timeout_reported,0);atomic_store(&g.net_tx_disconnect_requested,0);g.net_send_count=0;g.net_aac_send_used=0;g.net_aac_send_frames=0;network_clear_jitter_state();atomic_store(&g.net_min_buffer_events,0);atomic_store(&g.net_max_buffer_events,0);network_set_error(NET_ERROR_NONE,0,0);pthread_mutex_lock(&g.net_codec_lock);wifi_aac_destroy(g.net_aac);g.net_aac=NULL;pthread_mutex_unlock(&g.net_codec_lock);}
JNIEXPORT jintArray JNICALL Java_com_llawsxx_audioprocess_NativeAudio_networkErrorInfo(JNIEnv*e,jobject o){
    (void)o;
    jint values[3];
    values[0]=(jint)atomic_load(&g.net_error);
    values[1]=(jint)atomic_load(&g.net_error_detail0);
    values[2]=(jint)atomic_load(&g.net_error_detail1);
    jintArray result=(*e)->NewIntArray(e,3);
    if(result)(*e)->SetIntArrayRegion(e,result,0,3,values);
    return result;
}
JNIEXPORT jlongArray JNICALL Java_com_llawsxx_audioprocess_NativeAudio_networkReceiveStats(JNIEnv*e,jobject o){
    (void)o;
    jlong values[5]={
        (jlong)atomic_load(&g.net_buffer_ms),
        (jlong)atomic_load(&g.net_min_buffer_events),
        (jlong)atomic_load(&g.net_max_buffer_events),
        (jlong)g.net_min_ms,
        (jlong)g.net_max_ms
    };
    jlongArray result=(*e)->NewLongArray(e,5);
    if(result)(*e)->SetLongArrayRegion(e,result,0,5,values);
    return result;
}
JNIEXPORT jobjectArray JNICALL Java_com_llawsxx_audioprocess_NativeAudio_logs(JNIEnv*e,jobject o){
    (void)o;
    jclass string_class=(*e)->FindClass(e,"java/lang/String");
    if(!string_class)return NULL;
    char snapshot[NET_LOG_LINES][NET_LOG_LINE_BYTES];
    pthread_mutex_lock(&g_log_lock);
    int count=g_log_count;
    int write=g_log_write;
    for(int i=0;i<count;i++) {
        int index=(write-g_log_count+i+NET_LOG_LINES)%NET_LOG_LINES;
        memcpy(snapshot[i],g_log_lines[index],NET_LOG_LINE_BYTES);
    }
    pthread_mutex_unlock(&g_log_lock);
    jobjectArray result=(*e)->NewObjectArray(e,count,string_class,NULL);
    for(int i=0;i<count&&result;i++) {
        jstring line=(*e)->NewStringUTF(e,snapshot[i]);
        (*e)->SetObjectArrayElement(e,result,i,line);
        (*e)->DeleteLocalRef(e,line);
    }
    return result;
}
JNIEXPORT void JNICALL Java_com_llawsxx_audioprocess_NativeAudio_clearLogs(JNIEnv*e,jobject o){
    (void)e;(void)o;
    pthread_mutex_lock(&g_log_lock);
    g_log_write=0;g_log_count=0;
    pthread_mutex_unlock(&g_log_lock);
}
JNIEXPORT void JNICALL Java_com_llawsxx_audioprocess_NativeAudio_configureUsbOutputBuffer(JNIEnv*e,jobject o,jint maxMs){(void)e;(void)o;g.usb_buffer_max_ms=maxMs<5?5:(maxMs>200?200:maxMs);usb_host_audio_configure_output_buffer(g.usb_audio,g.usb_buffer_max_ms);}
JNIEXPORT void JNICALL Java_com_llawsxx_audioprocess_NativeAudio_configureOutputBufferMaxMs(JNIEnv*e,jobject o,jint maxMs){(void)e;(void)o;int normalized=maxMs<5?5:(maxMs>200?200:maxMs);atomic_store(&g.output_buffer_max_ms,normalized);output_ring_update_limits(g.rate,g.frames);}
JNIEXPORT void JNICALL Java_com_llawsxx_audioprocess_NativeAudio_configureInputBufferMaxMs(JNIEnv*e,jobject o,jint maxMs){(void)e;(void)o;int normalized=maxMs<5?5:(maxMs>200?200:maxMs);atomic_store(&g.input_buffer_max_ms,normalized);}
JNIEXPORT void JNICALL Java_com_llawsxx_audioprocess_NativeAudio_configureUsbInputBuffer(JNIEnv*e,jobject o,jint maxMs){(void)e;(void)o;int normalized=maxMs<5?5:(maxMs>200?200:maxMs);atomic_store(&g.usb_input_buffer_max_ms,normalized);usb_host_audio_configure_input_buffer(g.usb_audio,normalized);}
JNIEXPORT jboolean JNICALL Java_com_llawsxx_audioprocess_NativeAudio_setUsbVolume(JNIEnv*e,jobject o,jint percent){(void)e;(void)o;return usb_host_audio_set_volume(g.usb_audio,percent)?JNI_TRUE:JNI_FALSE;}
JNIEXPORT jboolean JNICALL Java_com_llawsxx_audioprocess_NativeAudio_networkInputTimedOut(JNIEnv*e,jobject o,jint timeoutMs){
    (void)e;(void)o;
    if(atomic_load(&g.net_role)!=2||timeoutMs<=0)return JNI_FALSE;
    if(g.net_transport==NET_TRANSPORT_TCP&&atomic_load(&g.net_rx_timeout_reported))return JNI_TRUE;
    uint64_t last=atomic_load(&g.net_last_packet_ns),now=now_ns();
    if(g.net_transport==NET_TRANSPORT_TCP&&now>last&&
       now-last>=(uint64_t)timeoutMs*1000000ull) {
        /* Ask the audio thread to drop only the stale client. The listening
         * socket stays open so the sender can reconnect during fallback. */
        LOGI("Wi-Fi TCP input stalled for %d ms; dropping client for fallback",timeoutMs);
        atomic_store(&g.net_rx_timeout_reported,1);
        atomic_store(&g.net_rx_disconnect_requested,1);
        return JNI_TRUE;
    }
    return now>last&&(now-last)>=(uint64_t)timeoutMs*1000000ull?JNI_TRUE:JNI_FALSE;
}
JNIEXPORT jboolean JNICALL Java_com_llawsxx_audioprocess_NativeAudio_networkOutputTimedOut(JNIEnv*e,jobject o,jint timeoutMs){
    (void)e;(void)o;
    if(atomic_load(&g.net_role)!=1||g.net_transport!=NET_TRANSPORT_TCP||timeoutMs<=0||
       g.net_tx_blocked_since_ns==0)return JNI_FALSE;
    uint64_t now=now_ns();
    if(now<=g.net_tx_blocked_since_ns||
       now-g.net_tx_blocked_since_ns<(uint64_t)timeoutMs*1000000ull)return JNI_FALSE;
    atomic_store(&g.net_tx_disconnect_requested,1);
    g.net_tcp_next_connect_ns=0;
    LOGI("Wi-Fi TCP sender stalled for %d ms; reconnecting",timeoutMs);
    return JNI_TRUE;
}
JNIEXPORT jboolean JNICALL Java_com_llawsxx_audioprocess_NativeAudio_networkOutputConnected(JNIEnv*e,jobject o){
    (void)e;(void)o;
    if(atomic_load(&g.net_role)!=1||g.net_transport!=NET_TRANSPORT_TCP||
       g.net_sock<0||g.net_tcp_connecting)return JNI_FALSE;
    int error=0; socklen_t error_len=sizeof(error);
    if(getsockopt(g.net_sock,SOL_SOCKET,SO_ERROR,&error,&error_len)!=0||error!=0)return JNI_FALSE;
    return JNI_TRUE;
}
JNIEXPORT jint JNICALL Java_com_llawsxx_audioprocess_NativeAudio_networkOutputConnectAttempts(JNIEnv*e,jobject o){
    (void)e;(void)o;
    return (jint)atomic_load(&g.net_tcp_connect_attempts);
}
JNIEXPORT jintArray JNICALL Java_com_llawsxx_audioprocess_NativeAudio_routeInfo(JNIEnv*e,jobject o){(void)o;jint route[4];pthread_mutex_lock(&g.stream_lock);route[0]=g.usb_input_host?-3:(atomic_load(&g.use_network_input)&&atomic_load(&g.net_role)==2?-2:(g.input?AAudioStream_getDeviceId(g.input):-1));route[1]=g.usb_output_host?-4:(g.output?AAudioStream_getDeviceId(g.output):-1);route[2]=g.usb_input_host?2:(g.input?AAudioStream_getChannelCount(g.input):-1);route[3]=g.usb_output_host?2:(g.output?AAudioStream_getChannelCount(g.output):-1);pthread_mutex_unlock(&g.stream_lock);jintArray result=(*e)->NewIntArray(e,4);(*e)->SetIntArrayRegion(e,result,0,4,route);return result;}
JNIEXPORT jboolean JNICALL Java_com_llawsxx_audioprocess_NativeAudio_usbHostFailed(JNIEnv*e,jobject o){(void)e;(void)o;return usb_host_audio_has_failed(g.usb_audio)?JNI_TRUE:JNI_FALSE;}
JNIEXPORT jlongArray JNICALL Java_com_llawsxx_audioprocess_NativeAudio_inputInfo(JNIEnv*e,jobject o){
    (void)o;
    jlong values[14]={0};
    pthread_mutex_lock(&g.stream_lock);
    if(g.input){
        int rate=AAudioStream_getSampleRate(g.input);
        uint32_t app_queued=input_ring_queued_frames();
        values[0]=rate;values[1]=AAudioStream_getPerformanceMode(g.input);values[2]=AAudioStream_getSharingMode(g.input);
        values[3]=AAudioStream_getFramesPerBurst(g.input);values[4]=AAudioStream_getBufferSizeInFrames(g.input);
        values[5]=AAudioStream_getBufferCapacityInFrames(g.input);values[6]=AAudioStream_getDeviceId(g.input);
        values[7]=AAudioStream_getXRunCount(g.input);values[8]=app_queued;
        values[9]=atomic_load(&g.input_callback_overruns);values[10]=AAudioStream_getChannelCount(g.input);
        values[11]=rate>0?(jlong)((int64_t)app_queued*1000000ll/rate):0;
        values[12]=atomic_load(&g.input_error);
        values[13]=atomic_load(&g.input_buffer_clears);
    }
    pthread_mutex_unlock(&g.stream_lock);
    jlongArray result=(*e)->NewLongArray(e,14);(*e)->SetLongArrayRegion(e,result,0,14,values);return result;
}
JNIEXPORT jlongArray JNICALL Java_com_llawsxx_audioprocess_NativeAudio_outputInfo(JNIEnv*e,jobject o){
    (void)o;
    jlong values[16]={0};
    pthread_mutex_lock(&g.stream_lock);
    if(g.output){
        int rate=AAudioStream_getSampleRate(g.output);
        int64_t written=AAudioStream_getFramesWritten(g.output),read=AAudioStream_getFramesRead(g.output);
        int64_t system_queued=written>read?written-read:0;
        uint32_t app_queued=output_ring_queued_frames();
        values[0]=rate;values[1]=AAudioStream_getPerformanceMode(g.output);values[2]=AAudioStream_getSharingMode(g.output);
        values[3]=AAudioStream_getFramesPerBurst(g.output);values[4]=AAudioStream_getBufferSizeInFrames(g.output);
        values[5]=AAudioStream_getBufferCapacityInFrames(g.output);values[6]=AAudioStream_getDeviceId(g.output);
        values[7]=AAudioStream_getXRunCount(g.output);values[8]=app_queued;values[9]=system_queued;
        values[10]=atomic_load(&g.output_callback_underflows);
        values[11]=rate>0?(jlong)((system_queued+(int64_t)app_queued)*1000000ll/rate):0;
        values[12]=atomic_load(&g.output_error);
        values[13]=atomic_load(&g.output_buffer_clears);
        values[14]=atomic_load(&g.dsp_last_us);
        values[15]=atomic_load(&g.dsp_max_us);
    }
    pthread_mutex_unlock(&g.stream_lock);
    jlongArray result=(*e)->NewLongArray(e,16);(*e)->SetLongArrayRegion(e,result,0,16,values);return result;
}
JNIEXPORT jlongArray JNICALL Java_com_llawsxx_audioprocess_NativeAudio_usbStats(JNIEnv*e,jobject o){(void)o;usb_host_audio_stats_t s={0};usb_host_audio_get_stats(g.usb_audio,&s);s.dsp_last_us=atomic_load(&g.dsp_last_us);s.dsp_max_us=atomic_load(&g.dsp_max_us);jlong values[18]={(jlong)s.input_packet_errors,(jlong)s.input_empty_packets,(jlong)s.input_transfer_errors,(jlong)s.input_ring_overruns,(jlong)s.output_transfer_errors,(jlong)s.output_low_water_events,(jlong)s.input_sample_rate,(jlong)s.input_bit_resolution,(jlong)s.input_channels,(jlong)s.output_sample_rate,(jlong)s.output_bit_resolution,(jlong)s.output_channels,(jlong)s.input_callback_max_us,(jlong)s.output_callback_max_us,(jlong)s.output_ring_overruns,(jlong)s.input_buffer_clears,(jlong)s.dsp_last_us,(jlong)s.dsp_max_us};jlongArray result=(*e)->NewLongArray(e,18);(*e)->SetLongArrayRegion(e,result,0,18,values);return result;}
JNIEXPORT jboolean JNICALL Java_com_llawsxx_audioprocess_NativeAudio_startRecordingFd(JNIEnv*e,jobject o,jint dryFd,jint wetFd){
    (void)e;(void)o;
    stop_recording_internal();
    g.record_rate = g.rate > 0 ? g.rate : 48000;
    g.record_blocks = calloc(RECORD_QUEUE_BLOCKS, sizeof(*g.record_blocks));
    if (!g.record_blocks) return JNI_FALSE;
    pthread_mutex_lock(&g.file_lock);
    g.dry_file=fdopen(dup(dryFd),"wb+"); g.wet_file=fdopen(dup(wetFd),"wb+"); g.dry_bytes=g.wet_bytes=0;
    if(g.dry_file){uint8_t z[44]={0};fwrite(z,1,44,g.dry_file);} if(g.wet_file){uint8_t z[44]={0};fwrite(z,1,44,g.wet_file);}
    int ok=g.dry_file&&g.wet_file;
    if(!ok){if(g.dry_file){fclose(g.dry_file);g.dry_file=NULL;}if(g.wet_file){fclose(g.wet_file);g.wet_file=NULL;}}
    pthread_mutex_unlock(&g.file_lock);
    if (!ok) { free(g.record_blocks); g.record_blocks=NULL; return JNI_FALSE; }
    atomic_store(&g.record_write_block,0); atomic_store(&g.record_read_block,0); atomic_store(&g.record_drops,0);
    atomic_store(&g.record_written_frames, 0);
    atomic_store(&g.recording,1);
    atomic_store(&g.record_worker_running,1);
    if (pthread_create(&g.record_thread,NULL,record_worker,NULL) != 0) {
        atomic_store(&g.record_worker_running,0); stop_recording_internal(); return JNI_FALSE;
    }
    atomic_store(&g.record_worker_started,1); return JNI_TRUE;
}
JNIEXPORT void JNICALL Java_com_llawsxx_audioprocess_NativeAudio_stopRecording(JNIEnv*e,jobject o){(void)e;(void)o;stop_recording_internal();}
