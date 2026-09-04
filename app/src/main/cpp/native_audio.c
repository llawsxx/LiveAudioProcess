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
#include "usb_host_bridge.h"
#include "wifi_aac_bridge.h"

#define TAG "AudioProcessNative"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define MAX_FRAMES 2048
#define NET_PACKET_FRAMES 128
#define NET_AAC_FRAMES 1024
#define NET_AAC_MAX_PACKET_BYTES 8192
#define NET_AAC_STORED_PACKET_BYTES 4096
#define NET_AAC_JITTER_SLOTS 128
#define NET_JITTER_SLOTS 1024
#define NET_REORDER_BACKTRACK_PACKETS 8
#define NET_PROTOCOL_VERSION 2
#define NET_TRANSPORT_UDP 0
#define NET_TRANSPORT_TCP 1
#define NET_TCP_BUFFER_BYTES 262144
#define EQ_BANDS 4
#define MAX_INPUT_CHANNELS 8

typedef struct { float b0,b1,b2,a1,a2,z1,z2,freq,gain,q; } Biquad;
typedef struct __attribute__((packed)) { uint32_t magic; uint16_t version; uint8_t codec; uint8_t channels; uint32_t rate; uint32_t frames; uint64_t timestamp_ns; uint32_t sequence; } NetHeader;
typedef struct {
    uint32_t sequence;
    int valid;
    float samples[NET_PACKET_FRAMES * 2];
} NetJitterSlot;
typedef struct {
    uint32_t sequence;
    uint32_t size;
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
    int net_sock, net_listen_sock, net_transport, net_codec, net_bitrate, net_port, net_min_ms, net_max_ms;
    int net_tcp_connecting;
    wifi_aac_t net_aac;
    pthread_mutex_t net_codec_lock;
    atomic_int net_role, use_network_input, net_packet_seen;
    atomic_int net_rx_disconnect_requested, net_rx_timeout_reported, net_tx_disconnect_requested;
    atomic_uint net_tcp_connect_attempts;
    atomic_ullong net_last_packet_ns;
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
    int net_aac_packet_count, net_aac_started, net_aac_seq_initialized;
    int net_send_count;
    struct sockaddr_in net_addr;
    uint32_t net_seq, net_play_seq, net_high_seq;
    uint32_t net_aac_play_seq, net_aac_high_seq;
    uint64_t net_missing_packets, net_late_packets, net_duplicate_packets;
    uint8_t net_rx_buffer[NET_TCP_BUFFER_BYTES];
    size_t net_rx_used, net_tx_used, net_tx_offset;
    uint8_t net_tx_buffer[NET_TCP_BUFFER_BYTES];
    uint64_t net_tx_blocked_since_ns;
    uint64_t net_tcp_next_connect_ns;
    atomic_int tone_enabled;
    int tone_waveform;
    int tone_channels;
    float tone_frequency, tone_level;
    double tone_phase;
    uint32_t tone_noise_state;
    float net_send_buffer[NET_AAC_FRAMES * 2];
} Engine;

static Engine g = {
    .net_sock = -1,
    .net_listen_sock = -1,
    .usb_buffer_max_ms = 50,
    .output_buffer_max_ms = ATOMIC_VAR_INIT(40),
    .usb_input_buffer_max_ms = ATOMIC_VAR_INIT(20),
    .input_buffer_max_ms = ATOMIC_VAR_INIT(20),
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
static void network_tcp_open_sender(void) {
    if(g.net_transport!=NET_TRANSPORT_TCP||atomic_load(&g.net_role)!=1||g.net_sock>=0)return;
    uint64_t now=now_ns();
    if(now<g.net_tcp_next_connect_ns)return;
    atomic_fetch_add(&g.net_tcp_connect_attempts,1);
    g.net_sock=socket(AF_INET,SOCK_STREAM,0);
    if(g.net_sock<0){LOGI("Wi-Fi TCP sender socket failed errno=%d",errno);g.net_tcp_next_connect_ns=now+1000000000ull;return;}
    fcntl(g.net_sock,F_SETFL,O_NONBLOCK);
    int rc=connect(g.net_sock,(struct sockaddr*)&g.net_addr,sizeof(g.net_addr));
    if(rc==0) {
        g.net_tcp_connecting=0;
        g.net_tcp_next_connect_ns=0;
        LOGI("Wi-Fi TCP sender connected");
    } else if(errno==EINPROGRESS) {
        g.net_tcp_connecting=1;
    } else {
        LOGI("Wi-Fi TCP sender connect failed errno=%d",errno);
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
        LOGI("Wi-Fi TCP sender connection error errno=%d",error);
        close(g.net_sock);g.net_sock=-1;g.net_tcp_connecting=0;
        g.net_tcp_next_connect_ns=now_ns()+1000000000ull;
        g.net_tx_offset=g.net_tx_used=0;
        g.net_tx_blocked_since_ns=0;
        return;
    }
    while (g.net_tx_offset < g.net_tx_used) {
        ssize_t n=send(g.net_sock,g.net_tx_buffer+g.net_tx_offset,
                       g.net_tx_used-g.net_tx_offset,MSG_DONTWAIT);
        if(n>0) { g.net_tx_offset+=(size_t)n; g.net_tx_blocked_since_ns=0; continue; }
        if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR)) break;
        LOGI("Wi-Fi TCP sender send failed errno=%d",errno);
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
        sendto(g.net_sock,packet,size,0,(struct sockaddr*)&g.net_addr,sizeof(g.net_addr));
    }
}
static void network_send_pcm_packet(const float *data) {
    NetHeader h={NET_MAGIC,NET_PROTOCOL_VERSION,0,2,(uint32_t)g.rate,
                 NET_PACKET_FRAMES,now_ns(),g.net_seq++};
    uint8_t packet[sizeof(NetHeader)+NET_PACKET_FRAMES*2*sizeof(float)];
    memcpy(packet,&h,sizeof(h));
    memcpy(packet+sizeof(h),data,NET_PACKET_FRAMES*2*sizeof(float));
    if(g.net_transport==NET_TRANSPORT_TCP) {
        network_write_packet(packet,sizeof(packet));
    } else {
        ssize_t expected=(ssize_t)sizeof(packet);
        ssize_t sent=sendto(g.net_sock,packet,sizeof(packet),0,(struct sockaddr*)&g.net_addr,sizeof(g.net_addr));
        if(sent!=expected && (h.sequence==0 || h.sequence%100==0))
        LOGI("Wi-Fi send dropped sequence=%u bytes=%zd/%zd",h.sequence,sent,expected);
    }
    if(h.sequence==0)
        LOGI("Wi-Fi protocol v%d fixed packet: frames=%d bytes=%zu",NET_PROTOCOL_VERSION,
             NET_PACKET_FRAMES,sizeof(packet));
}

static void network_send_aac_packet(const float *data) {
    uint8_t packet[sizeof(NetHeader)+NET_AAC_MAX_PACKET_BYTES];
    pthread_mutex_lock(&g.net_codec_lock);
    int encoded=wifi_aac_encode(g.net_aac,data,NET_AAC_FRAMES,
                                packet+sizeof(NetHeader),NET_AAC_MAX_PACKET_BYTES);
    pthread_mutex_unlock(&g.net_codec_lock);
    if(encoded<=0)return;
    NetHeader h={NET_MAGIC,NET_PROTOCOL_VERSION,1,2,(uint32_t)g.rate,
                 NET_AAC_FRAMES,now_ns(),g.net_seq};
    g.net_seq+=NET_AAC_FRAMES/NET_PACKET_FRAMES;
    memcpy(packet,&h,sizeof(h));
    size_t packet_size=sizeof(h)+(size_t)encoded;
    if(g.net_transport==NET_TRANSPORT_TCP) {
        network_write_packet(packet,packet_size);
    } else {
        ssize_t sent=sendto(g.net_sock,packet,packet_size,0,(struct sockaddr*)&g.net_addr,sizeof(g.net_addr));
        if(sent!=(ssize_t)packet_size && (h.sequence==0 || h.sequence%800==0))
        LOGI("Wi-Fi AAC send dropped sequence=%u bytes=%zd/%zu",h.sequence,sent,packet_size);
    }
    if(h.sequence==0)
        LOGI("Wi-Fi AAC protocol v%d: frames=%d bitrate=%d bytes=%zu",NET_PROTOCOL_VERSION,
             NET_AAC_FRAMES,g.net_bitrate,packet_size);
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
        int packet_frames=g.net_codec==1?NET_AAC_FRAMES:NET_PACKET_FRAMES;
        int room=packet_frames-g.net_send_count;
        int take=frames-offset<room?frames-offset:room;
        memcpy(g.net_send_buffer+g.net_send_count*2,data+offset*2,(size_t)take*2*sizeof(float));
        g.net_send_count+=take;
        offset+=take;
        if(g.net_send_count==packet_frames) {
            if(g.net_codec==1)network_send_aac_packet(g.net_send_buffer);
            else network_send_pcm_packet(g.net_send_buffer);
            g.net_send_count=0;
        }
    }
}

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
    if(!g.net_aac_seq_initialized) {
        g.net_aac_seq_initialized=1;
        g.net_aac_play_seq=h->sequence;
        g.net_aac_high_seq=h->sequence;
    } else {
        int32_t delta=(int32_t)(h->sequence-g.net_aac_play_seq);
        if(delta<0&&!g.net_aac_started&&delta>=-(NET_AAC_FRAMES/NET_PACKET_FRAMES)) {
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
            g.net_aac_high_seq=h->sequence;
            delta=0;
        } else if(delta<0) {
            g.net_late_packets++;
            return;
        }
        if(delta>=(int32_t)(NET_AAC_JITTER_SLOTS*(NET_AAC_FRAMES/NET_PACKET_FRAMES))) {
            memset(g.net_aac_jitter,0,NET_AAC_JITTER_SLOTS*sizeof(*g.net_aac_jitter));
            g.net_aac_packet_count=0;g.net_aac_started=0;g.net_aac_play_seq=h->sequence;
        }
    }
    uint32_t packet_index=h->sequence/(NET_AAC_FRAMES/NET_PACKET_FRAMES);
    NetAacJitterSlot *slot=&g.net_aac_jitter[packet_index%NET_AAC_JITTER_SLOTS];
    if(slot->valid&&slot->sequence==h->sequence){g.net_duplicate_packets++;return;}
    if(slot->valid)g.net_aac_packet_count--;
    slot->sequence=h->sequence;slot->size=size;memcpy(slot->data,data,size);slot->valid=1;
    g.net_aac_packet_count++;
    if((int32_t)(h->sequence-g.net_aac_high_seq)>0)g.net_aac_high_seq=h->sequence;
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
        pthread_mutex_lock(&g.net_codec_lock);
        int decoded_frames=wifi_aac_decode(g.net_aac,slot->data,(int)slot->size,decoded,NET_AAC_FRAMES);
        pthread_mutex_unlock(&g.net_codec_lock);
        uint32_t sequence=slot->sequence;slot->valid=0;g.net_aac_packet_count--;
        g.net_aac_play_seq+=NET_AAC_FRAMES/NET_PACKET_FRAMES;
        if(decoded_frames!=NET_AAC_FRAMES)continue;
        for(int offset=0;offset<NET_AAC_FRAMES;offset+=NET_PACKET_FRAMES) {
            NetHeader slice={NET_MAGIC,NET_PROTOCOL_VERSION,1,2,(uint32_t)g.rate,
                             NET_PACKET_FRAMES,0,sequence+(uint32_t)(offset/NET_PACKET_FRAMES)};
            network_store_packet(&slice,decoded+offset*2,now_ns());
        }
    }
}

static void network_process_packet(const uint8_t *packet, size_t n) {
    if(!packet||n<sizeof(NetHeader))return;
    NetHeader h; memcpy(&h,packet,sizeof(h));
    if(h.magic!=NET_MAGIC||h.version!=NET_PROTOCOL_VERSION||h.channels!=2||
       h.rate!=(uint32_t)g.rate||h.codec!=(uint8_t)g.net_codec)return;
    uint64_t arrival=now_ns();
    if(h.codec==0) {
        size_t expected=sizeof(NetHeader)+NET_PACKET_FRAMES*2*sizeof(float);
        if(h.frames!=NET_PACKET_FRAMES||n!=expected)return;
        network_store_packet(&h,(const float *)(packet+sizeof(h)),arrival);
    } else if(h.codec==1) {
        if(h.frames!=NET_AAC_FRAMES||n<=sizeof(NetHeader)||
           n-sizeof(NetHeader)>NET_AAC_STORED_PACKET_BYTES)return;
        network_store_aac_packet(&h,packet+sizeof(NetHeader),
                                 (uint32_t)(n-sizeof(NetHeader)));
    } else return;
    atomic_store(&g.net_last_packet_ns,arrival);
    if(!atomic_exchange(&g.net_packet_seen,1))
        LOGI("first valid Wi-Fi packet: protocol=%u codec=%u rate=%u frames=%u bytes=%zu",
             h.version,h.codec,h.rate,h.frames,n);
    atomic_store(&g.net_rx_timeout_reported,0);
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
            if(accepted>=0) { fcntl(accepted,F_SETFL,O_NONBLOCK); g.net_sock=accepted; g.net_rx_used=0; LOGI("Wi-Fi TCP client connected"); }
        }
        if(g.net_sock>=0) {
            for(;;) {
                if(g.net_rx_used==sizeof(g.net_rx_buffer))break;
                ssize_t n=recv(g.net_sock,g.net_rx_buffer+g.net_rx_used,
                               sizeof(g.net_rx_buffer)-g.net_rx_used,MSG_DONTWAIT);
                if(n>0){g.net_rx_used+=(size_t)n;continue;}
                if(n==0){close(g.net_sock);g.net_sock=-1;g.net_rx_used=0;break;}
                if(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR)break;
                close(g.net_sock);g.net_sock=-1;g.net_rx_used=0;break;
            }
            size_t consumed=0;
            while(g.net_rx_used-consumed>=sizeof(uint32_t)) {
                uint32_t wire_len; memcpy(&wire_len,g.net_rx_buffer+consumed,sizeof(wire_len));
                size_t packet_len=ntohl(wire_len);
                if(packet_len<sizeof(NetHeader)||packet_len>sizeof(g.net_rx_buffer)-sizeof(uint32_t)) { consumed=g.net_rx_used; break; }
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
    uint8_t packet[sizeof(NetHeader)+NET_AAC_MAX_PACKET_BYTES];
    for (;;) {
        ssize_t n=recvfrom(g.net_sock,packet,sizeof(packet),MSG_DONTWAIT,NULL,NULL);
        if(n<(ssize_t)sizeof(NetHeader)) break;
        network_process_packet(packet,(size_t)n);
    }
    if(g.net_codec==1)network_decode_aac_packets();
}

static int network_receive(float *data, int frames) {
    network_fill();
    if ((g.net_sock < 0 && !(g.net_transport==NET_TRANSPORT_TCP && g.net_listen_sock>=0)) ||
        atomic_load(&g.net_role) != 2 || !g.net_jitter) return 0;
    int min_frames=(g.rate*g.net_min_ms)/1000;
    int max_frames=(g.rate*g.net_max_ms)/1000;
    int target_frames=(min_frames+max_frames)/2;
    if(target_frames<1) target_frames=1;
    if(max_frames<target_frames) max_frames=target_frames;
    if(!g.net_started) {
        if(network_valid_frames()>=target_frames) g.net_started=1;
        else { memset(data,0,(size_t)frames*2*sizeof(float)); return frames; }
    }
    if(network_span_frames()>=max_frames) {
        int keep_packets=(target_frames+NET_PACKET_FRAMES-1)/NET_PACKET_FRAMES;
        uint32_t new_play_seq=g.net_high_seq-(uint32_t)(keep_packets-1);
        network_drop_until(new_play_seq);
    }
    if(network_valid_frames()<min_frames) {
        g.net_started=0;
        memset(data,0,(size_t)frames*2*sizeof(float));
        return frames;
    }

    int produced=0;
    while(produced<frames) {
        NetJitterSlot *slot=&g.net_jitter[g.net_play_seq%NET_JITTER_SLOTS];
        if(slot->valid && slot->sequence==g.net_play_seq) {
            int available=NET_PACKET_FRAMES-g.net_play_offset;
            int take=frames-produced<available?frames-produced:available;
            memcpy(data+produced*2,slot->samples+g.net_play_offset*2,(size_t)take*2*sizeof(float));
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
            memset(data+produced*2,0,(size_t)take*2*sizeof(float));
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
        memset(data+produced*2,0,(size_t)(frames-produced)*2*sizeof(float));
        break;
    }
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
        int tone_waveform, tone_channels;
        float tone_frequency, tone_level;
        pthread_mutex_lock(&g.param_lock);
        memcpy(p,g.values,sizeof(p));
        tone_waveform=g.tone_waveform;
        tone_channels=g.tone_channels;
        tone_frequency=g.tone_frequency;
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
                    default: value=sin(phase*6.283185307179586); break;
                }
                value*=clampf(tone_level,0.f,1.f);
                l=(tone_channels==2)?0.f:(float)value;
                r=(tone_channels==1)?0.f:(float)value;
                phase += (double)tone_frequency/(double)g.rate;
                phase -= floor(phase);
                g.tone_phase=phase;
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
        if(g.net_sock>=0)close(g.net_sock);
        if(g.net_listen_sock>=0)close(g.net_listen_sock);
        g.net_sock=g.net_listen_sock=-1;
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
JNIEXPORT void JNICALL Java_com_llawsxx_audioprocess_NativeAudio_configureTone(JNIEnv*e,jobject o,jboolean enabled,jint waveform,jint channels,jfloat frequency,jfloat level){
    (void)e;(void)o;
    float max_frequency=g.rate>0?(float)g.rate*.45f:20000.f;
    pthread_mutex_lock(&g.param_lock);
    g.tone_waveform=waveform<0?0:(waveform>3?3:waveform);
    g.tone_channels=channels<0?0:(channels>2?2:channels);
    g.tone_frequency=frequency<1.f?1.f:(frequency>max_frequency?max_frequency:frequency);
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
JNIEXPORT jboolean JNICALL Java_com_llawsxx_audioprocess_NativeAudio_configureNetwork(JNIEnv*e,jobject o,jint role,jint transport,jint codec,jint sampleRate,jint bitrate,jstring host,jint port,jint minMs,jint maxMs){
    (void)o;
    if(transport!=NET_TRANSPORT_UDP&&transport!=NET_TRANSPORT_TCP)return JNI_FALSE;
    if(codec!=0&&codec!=1)return JNI_FALSE;
    int normalized_rate=sampleRate==44100||sampleRate==48000||sampleRate==96000?sampleRate:48000;
    int normalized_bitrate=bitrate<32000?32000:(bitrate>512000?512000:bitrate);
    wifi_aac_t next_aac=codec==1?wifi_aac_create(normalized_rate,normalized_bitrate):NULL;
    int codec_ready=codec==0||(next_aac&&wifi_aac_frame_length(next_aac)==NET_AAC_FRAMES);
    if(!codec_ready){wifi_aac_destroy(next_aac);LOGE("Wi-Fi AAC initialization failed rate=%d bitrate=%d",normalized_rate,normalized_bitrate);return JNI_FALSE;}
    const char*h=(*e)->GetStringUTFChars(e,host,NULL);
    struct in_addr requested_addr={0};
    inet_aton(h,&requested_addr);
    int current_role=atomic_load(&g.net_role);
    int socket_ready=g.net_sock>=0 || (g.net_transport==NET_TRANSPORT_TCP&&g.net_listen_sock>=0);
    int same_transport=current_role==role && socket_ready && g.net_transport==transport &&
            g.net_codec==codec && g.rate==normalized_rate && g.net_port==port &&
            g.net_addr.sin_addr.s_addr==requested_addr.s_addr;
    int normalized_min=minMs<0?0:(minMs>200?200:minMs);
    int normalized_max=maxMs<50?50:(maxMs>1000?1000:maxMs);
    if (normalized_max<normalized_min) normalized_min=normalized_max;
    if (same_transport) {
        /* Parameter/UI updates must not tear down a live transport. In
         * receive mode, bitrate is carried by each ADTS frame and the
         * decoder must stay alive; only a sender needs a new encoder. */
        g.net_min_ms=normalized_min;
        g.net_max_ms=normalized_max;
        if (codec==1 && role==1 && normalized_bitrate != g.net_bitrate) {
            pthread_mutex_lock(&g.net_codec_lock);
            wifi_aac_t previous=g.net_aac;
            g.net_aac=next_aac;
            g.net_bitrate=normalized_bitrate;
            pthread_mutex_unlock(&g.net_codec_lock);
            wifi_aac_destroy(previous);
            (*e)->ReleaseStringUTFChars(e,host,h);
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
    if(g.net_sock>=0)close(g.net_sock);
    if(g.net_listen_sock>=0)close(g.net_listen_sock);
    g.net_sock=socket(AF_INET,SOCK_DGRAM,0);
    g.net_listen_sock=-1;
    if(transport==NET_TRANSPORT_TCP) {
        close(g.net_sock); g.net_sock=-1;
        if(role==2) {
            g.net_listen_sock=socket(AF_INET,SOCK_STREAM,0);
            if(g.net_listen_sock>=0) { int yes=1; setsockopt(g.net_listen_sock,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes)); fcntl(g.net_listen_sock,F_SETFL,O_NONBLOCK); }
        } else {
            g.net_sock=socket(AF_INET,SOCK_STREAM,0);
            if(g.net_sock>=0) { atomic_fetch_add(&g.net_tcp_connect_attempts,1); struct sockaddr_in target={0}; target.sin_family=AF_INET; target.sin_port=htons((uint16_t)port); target.sin_addr=requested_addr; fcntl(g.net_sock,F_SETFL,O_NONBLOCK); int rc=connect(g.net_sock,(struct sockaddr*)&target,sizeof(target)); if(rc==0)g.net_tcp_connecting=0; else if(errno==EINPROGRESS)g.net_tcp_connecting=1; else {close(g.net_sock);g.net_sock=-1;g.net_tcp_connecting=0;g.net_tcp_next_connect_ns=now_ns()+1000000000ull;} }
        }
    } else {
        g.net_sock=socket(AF_INET,SOCK_DGRAM,0);
        if(g.net_sock>=0) fcntl(g.net_sock,F_SETFL,O_NONBLOCK);
    }
    if((role==1&&g.net_sock<0)||(role==2&&transport==NET_TRANSPORT_TCP&&g.net_listen_sock<0)||(role==2&&transport==NET_TRANSPORT_UDP&&g.net_sock<0)){atomic_store(&g.net_role,0);(*e)->ReleaseStringUTFChars(e,host,h);return JNI_FALSE;}
    memset(&g.net_addr,0,sizeof(g.net_addr));g.net_addr.sin_family=AF_INET;g.net_addr.sin_port=htons((uint16_t)port);g.net_addr.sin_addr=requested_addr;
    atomic_store(&g.net_last_packet_ns,now_ns());atomic_store(&g.net_packet_seen,0);
    g.net_transport=transport;g.net_codec=codec;g.net_bitrate=normalized_bitrate;g.net_port=port;g.net_min_ms=minMs<0?0:(minMs>200?200:minMs);g.net_max_ms=maxMs<50?50:(maxMs>1000?1000:maxMs);if(g.net_max_ms<g.net_min_ms)g.net_min_ms=g.net_max_ms;
    g.net_send_count=0;g.net_seq=0;g.net_tcp_connecting=0;g.net_tcp_next_connect_ns=0;g.net_tx_used=g.net_tx_offset=0;g.net_tx_blocked_since_ns=0;atomic_store(&g.net_tcp_connect_attempts,0);atomic_store(&g.net_rx_disconnect_requested,0);atomic_store(&g.net_rx_timeout_reported,0);atomic_store(&g.net_tx_disconnect_requested,0);g.net_missing_packets=g.net_late_packets=g.net_duplicate_packets=0;network_clear_jitter_state();
    if(role==2) {
        int bind_sock=transport==NET_TRANSPORT_TCP?g.net_listen_sock:g.net_sock;
        if(bind(bind_sock,(struct sockaddr*)&g.net_addr,sizeof(g.net_addr))<0 ||
           (transport==NET_TRANSPORT_TCP&&listen(g.net_listen_sock,1)<0)) {
            if(g.net_sock>=0)close(g.net_sock); if(g.net_listen_sock>=0)close(g.net_listen_sock); g.net_sock=g.net_listen_sock=-1; atomic_store(&g.net_role,0); (*e)->ReleaseStringUTFChars(e,host,h); return JNI_FALSE;
        }
        if(transport==NET_TRANSPORT_TCP) LOGI("Wi-Fi TCP server listening on port=%d",port);
    }
    atomic_store(&g.net_role,role);(*e)->ReleaseStringUTFChars(e,host,h);return JNI_TRUE;
}
JNIEXPORT void JNICALL Java_com_llawsxx_audioprocess_NativeAudio_clearNetwork(JNIEnv*e,jobject o){(void)e;(void)o;if(g.net_sock>=0)close(g.net_sock);if(g.net_listen_sock>=0)close(g.net_listen_sock);g.net_sock=g.net_listen_sock=-1;g.net_tcp_connecting=0;g.net_tcp_next_connect_ns=0;g.net_rx_used=g.net_tx_used=g.net_tx_offset=0;g.net_tx_blocked_since_ns=0;atomic_store(&g.net_rx_disconnect_requested,0);atomic_store(&g.net_rx_timeout_reported,0);atomic_store(&g.net_tx_disconnect_requested,0);atomic_store(&g.net_role,0);g.net_send_count=0;network_clear_jitter_state();pthread_mutex_lock(&g.net_codec_lock);wifi_aac_destroy(g.net_aac);g.net_aac=NULL;pthread_mutex_unlock(&g.net_codec_lock);}
JNIEXPORT void JNICALL Java_com_llawsxx_audioprocess_NativeAudio_configureUsbOutputBuffer(JNIEnv*e,jobject o,jint maxMs){(void)e;(void)o;g.usb_buffer_max_ms=maxMs<5?5:(maxMs>200?200:maxMs);usb_host_audio_configure_output_buffer(g.usb_audio,g.usb_buffer_max_ms);}
JNIEXPORT void JNICALL Java_com_llawsxx_audioprocess_NativeAudio_configureOutputBufferMaxMs(JNIEnv*e,jobject o,jint maxMs){(void)e;(void)o;int normalized=maxMs<5?5:(maxMs>200?200:maxMs);atomic_store(&g.output_buffer_max_ms,normalized);output_ring_update_limits(g.rate,g.frames);}
JNIEXPORT void JNICALL Java_com_llawsxx_audioprocess_NativeAudio_configureInputBufferMaxMs(JNIEnv*e,jobject o,jint maxMs){(void)e;(void)o;int normalized=maxMs<5?5:(maxMs>200?200:maxMs);atomic_store(&g.input_buffer_max_ms,normalized);}
JNIEXPORT void JNICALL Java_com_llawsxx_audioprocess_NativeAudio_configureUsbInputBuffer(JNIEnv*e,jobject o,jint maxMs){(void)e;(void)o;int normalized=maxMs<5?5:(maxMs>200?200:maxMs);atomic_store(&g.usb_input_buffer_max_ms,normalized);usb_host_audio_configure_input_buffer(g.usb_audio,normalized);}
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
