#include <aaudio/AAudio.h>
#include <jni.h>
#include <android/log.h>
#include <math.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "convolution_reverb.h"
#include "usb_host_bridge.h"

#define TAG "PulseForgeNative"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define MAX_FRAMES 2048
#define NET_PACKET_FRAMES 128
#define NET_JITTER_SLOTS 1024
#define NET_REORDER_BACKTRACK_PACKETS 8
#define NET_PROTOCOL_VERSION 2
#define EQ_BANDS 4

typedef struct { float b0,b1,b2,a1,a2,z1,z2,freq,gain,q; } Biquad;
typedef struct __attribute__((packed)) { uint32_t magic; uint16_t version; uint8_t codec; uint8_t channels; uint32_t rate; uint32_t frames; uint64_t timestamp_ns; uint32_t sequence; } NetHeader;
typedef struct {
    uint32_t sequence;
    int valid;
    float samples[NET_PACKET_FRAMES * 2];
} NetJitterSlot;
typedef struct {
    AAudioStream *input, *output;
    pthread_t thread;
    atomic_int running, recording, flags;
    _Atomic(float) levels[6];
    int rate, frames, in_channels, pair;
    float values[24];
    pthread_mutex_t param_lock, file_lock;
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
    FILE *dry_file, *wet_file;
    uint32_t dry_bytes, wet_bytes;
    int net_sock, net_codec, net_port, net_min_ms, net_max_ms;
    atomic_int net_role, use_network_input, net_packet_seen;
    atomic_ullong net_last_packet_ns;
    NetJitterSlot *net_jitter;
    usb_host_audio_t usb_audio;
    int usb_input_host, usb_output_host;
    int usb_buffer_min_ms, usb_buffer_max_ms;
    int net_packet_count, net_started, net_play_offset, net_seq_initialized;
    int net_send_count;
    struct sockaddr_in net_addr;
    uint32_t net_seq, net_play_seq, net_high_seq;
    uint64_t net_missing_packets, net_late_packets, net_duplicate_packets;
    float net_send_buffer[NET_PACKET_FRAMES * 2];
} Engine;

static Engine g = {
    .net_sock = -1,
    .usb_buffer_min_ms = 16,
    .usb_buffer_max_ms = 50,
    .param_lock = PTHREAD_MUTEX_INITIALIZER,
    .file_lock = PTHREAD_MUTEX_INITIALIZER,
    .reverb_lock = PTHREAD_MUTEX_INITIALIZER
};

#define NET_MAGIC 0x50464C58u

enum { DSP_ON=1, EQ_ON=2, REVERB_ON=4, LIMITER_ON=8 };
enum {
    P_EQ1_F,P_EQ1_G,P_EQ1_Q,P_EQ2_F,P_EQ2_G,P_EQ2_Q,P_EQ3_F,P_EQ3_G,P_EQ3_Q,P_EQ4_F,P_EQ4_G,P_EQ4_Q,
    P_ROOM,P_DECAY,P_DAMP,P_MIX,P_LIM_IN,P_LIMIT,P_RELEASE,P_CEILING,P_LOOKAHEAD,P_ADAPTIVE_RELEASE
};

static float db_to_linear(float db) { return powf(10.f, db / 20.f); }
static float clampf(float x, float lo, float hi) { return x < lo ? lo : (x > hi ? hi : x); }

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
    if(!atomic_load(&g.recording))return;
    int16_t d[MAX_FRAMES*2],w[MAX_FRAMES*2];
    for(int i=0;i<frames*2;i++){d[i]=(int16_t)(clampf(dry[i],-1.f,1.f)*32767.f);w[i]=(int16_t)(clampf(wet[i],-1.f,1.f)*32767.f);}
    pthread_mutex_lock(&g.file_lock);
    if(g.dry_file){fwrite(d,sizeof(int16_t),frames*2,g.dry_file);g.dry_bytes+=frames*4;}
    if(g.wet_file){fwrite(w,sizeof(int16_t),frames*2,g.wet_file);g.wet_bytes+=frames*4;}
    pthread_mutex_unlock(&g.file_lock);
}
static uint64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (uint64_t)t.tv_sec*1000000000ull + (uint64_t)t.tv_nsec; }
static void network_send_packet(const float *data) {
    NetHeader h={NET_MAGIC,NET_PROTOCOL_VERSION,(uint8_t)g.net_codec,2,(uint32_t)g.rate,
                 NET_PACKET_FRAMES,now_ns(),g.net_seq++};
    uint8_t packet[sizeof(NetHeader)+NET_PACKET_FRAMES*2*sizeof(float)];
    memcpy(packet,&h,sizeof(h));
    memcpy(packet+sizeof(h),data,NET_PACKET_FRAMES*2*sizeof(float));
    ssize_t expected=(ssize_t)sizeof(packet);
    ssize_t sent=sendto(g.net_sock,packet,sizeof(packet),0,(struct sockaddr*)&g.net_addr,sizeof(g.net_addr));
    if(sent!=expected && (h.sequence==0 || h.sequence%100==0))
        LOGI("Wi-Fi send dropped sequence=%u bytes=%zd/%zd",h.sequence,sent,expected);
    if(h.sequence==0)
        LOGI("Wi-Fi protocol v%d fixed packet: frames=%d bytes=%zu",NET_PROTOCOL_VERSION,
             NET_PACKET_FRAMES,sizeof(packet));
}

static void network_send(const float *data, int frames) {
    if (g.net_sock < 0 || atomic_load(&g.net_role) != 1 || !data || frames<=0) return;
    int offset=0;
    while(offset<frames) {
        int room=NET_PACKET_FRAMES-g.net_send_count;
        int take=frames-offset<room?frames-offset:room;
        memcpy(g.net_send_buffer+g.net_send_count*2,data+offset*2,(size_t)take*2*sizeof(float));
        g.net_send_count+=take;
        offset+=take;
        if(g.net_send_count==NET_PACKET_FRAMES) {
            network_send_packet(g.net_send_buffer);
            g.net_send_count=0;
        }
    }
}

static void network_clear_jitter_state(void) {
    if(g.net_jitter) memset(g.net_jitter,0,NET_JITTER_SLOTS*sizeof(*g.net_jitter));
    g.net_packet_count=0;
    g.net_started=0;
    g.net_play_offset=0;
    g.net_seq_initialized=0;
    g.net_play_seq=0;
    g.net_high_seq=0;
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

static void network_fill(void) {
    if (g.net_sock < 0 || atomic_load(&g.net_role) != 2 || !g.net_jitter) return;
    uint8_t packet[sizeof(NetHeader)+NET_PACKET_FRAMES*2*sizeof(float)];
    for (;;) {
        ssize_t n=recvfrom(g.net_sock,packet,sizeof(packet),MSG_DONTWAIT,NULL,NULL);
        if(n<(ssize_t)sizeof(NetHeader)) return;
        NetHeader h; memcpy(&h,packet,sizeof(h));
        size_t expected=sizeof(NetHeader)+NET_PACKET_FRAMES*2*sizeof(float);
        if(h.magic!=NET_MAGIC || h.version!=NET_PROTOCOL_VERSION || h.channels!=2 ||
           h.rate!=(uint32_t)g.rate || h.codec!=(uint8_t)g.net_codec ||
           h.frames!=NET_PACKET_FRAMES || n!=(ssize_t)expected) continue;
        uint64_t arrival=now_ns();
        network_store_packet(&h,(const float *)(packet+sizeof(h)),arrival);
        atomic_store(&g.net_last_packet_ns,arrival);
        if(!atomic_exchange(&g.net_packet_seen,1))
            LOGI("first valid Wi-Fi packet: protocol=%u rate=%u frames=%u bytes=%zd",
                 h.version,h.rate,h.frames,n);
    }
}

static int network_receive(float *data, int frames) {
    network_fill();
    if (g.net_sock < 0 || atomic_load(&g.net_role) != 2 || !g.net_jitter) return 0;
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
    float input[MAX_FRAMES*4], dry[MAX_FRAMES*2], output[MAX_FRAMES*2], p[24];
    while(atomic_load(&g.running)) {
        int want=g.frames<MAX_FRAMES?g.frames:MAX_FRAMES;
        aaudio_result_t got;
        int using_network=atomic_load(&g.use_network_input)&&atomic_load(&g.net_role)==2;
        if(using_network){ got=network_receive(output,want); if(got<=0){ memset(output,0,(size_t)want*2*sizeof(float)); got=want; } memcpy(dry,output,(size_t)got*2*sizeof(float)); }
        else if(g.usb_input_host && g.usb_audio){got=usb_host_audio_read(g.usb_audio,input,want);}
        else if(g.input){if(atomic_load(&g.net_role)==2)network_fill();got=AAudioStream_read(g.input,input,want,100000000);}
        else { memset(input,0,(size_t)want*4*sizeof(float)); got=want; }
        if(got<=0)continue;
        pthread_mutex_lock(&g.param_lock); memcpy(p,g.values,sizeof(p)); pthread_mutex_unlock(&g.param_lock);
        int flags=atomic_load(&g.flags); float peaks[4]={0.f,0.f,0.f,0.f};
        if (!(flags&DSP_ON) || !(flags&LIMITER_ON)) {
            g.limiter_delay_frames=0;
            atomic_store(&g.levels[4],1.f);
            atomic_store(&g.levels[5],fmaxf(p[P_RELEASE],10.f));
        }
        ConvolutionReverb *reverb=NULL;
        if((flags&REVERB_ON) && (flags&DSP_ON)) {
            pthread_mutex_lock(&g.reverb_lock);
            reverb=g.reverb;
        }
        for(int i=0;i<got;i++) {
            int first=g.pair*2; if(first>=g.in_channels)first=0;
            int second=first+1; if(second>=g.in_channels)second=first;
            float l = using_network ? output[i*2] : (g.usb_input_host ? input[i*2] : input[i*g.in_channels+first]);
            float r = using_network ? output[i*2+1] : (g.usb_input_host ? input[i*2+1] : input[i*g.in_channels+second]);
            dry[i*2]=l;dry[i*2+1]=r;
            peaks[0]=fmaxf(peaks[0],fabsf(l)); peaks[1]=fmaxf(peaks[1],fabsf(r));
            if(flags&DSP_ON) {
                if(flags&EQ_ON)for(int b=0;b<EQ_BANDS;b++){biquad_config(&g.eq[0][b],g.rate,p[b*3],p[b*3+1],p[b*3+2]);biquad_config(&g.eq[1][b],g.rate,p[b*3],p[b*3+1],p[b*3+2]);l=biquad_process(&g.eq[0][b],l);r=biquad_process(&g.eq[1][b],r);}
                if((flags&REVERB_ON) && reverb){float wet_l=0.f,wet_r=0.f,mix=clampf(p[P_MIX]/100.f,0.f,1.f);convolution_reverb_process(reverb,l,r,&wet_l,&wet_r);l=l*(1.f-mix)+wet_l*mix;r=r*(1.f-mix)+wet_r*mix;}
                if(flags&LIMITER_ON)limiter_process(&l,&r,p);
            }
            output[i*2]=l;output[i*2+1]=r;peaks[2]=fmaxf(peaks[2],fabsf(l));peaks[3]=fmaxf(peaks[3],fabsf(r));
        }
        if(reverb) pthread_mutex_unlock(&g.reverb_lock);
        for(int i=0;i<4;i++)atomic_store(&g.levels[i],atomic_load(&g.levels[i])*.84f+peaks[i]*.16f);
        if(g.net_role==1) network_send(output,got);
        if (g.usb_output_host && g.usb_audio) usb_host_audio_write(g.usb_audio, output, got);
        else if (g.output) AAudioStream_write(g.output,output,got,100000000);
        record_samples(dry,output,got);
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
    AAudioStreamBuilder_setBufferCapacityInFrames(b,frames*2); if(device>=0)AAudioStreamBuilder_setDeviceId(b,device);
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
    return 1;
}

JNIEXPORT jboolean JNICALL Java_com_llawsxx_audioprocess_NativeAudio_start(JNIEnv*e,jobject o,jint rate,jint frames,jint inDev,jint outDev,jint channels,jint pair,jboolean useNetworkInput,jint usbFd,jboolean usbInputHost,jboolean usbOutputHost,jint usbBitDepth,jint usbInputBurstPackets,jint usbOutputBurstPackets){
    (void)e;(void)o;if(atomic_load(&g.running))return JNI_TRUE;memset(g.eq,0,sizeof(g.eq));g.rate=rate;g.frames=frames;g.in_channels=channels;g.pair=pair;g.limiter_gain=1.0;g.limiter_delta=0.0;g.limiter_delay_frames=0;g.limiter_next_iter=g.limiter_next_len=0;g.usb_audio=NULL;g.usb_input_host=usbInputHost?1:0;g.usb_output_host=usbOutputHost?1:0;if(usbFd<0){g.usb_input_host=0;g.usb_output_host=0;}atomic_store(&g.use_network_input,useNetworkInput?1:0);if(useNetworkInput)atomic_store(&g.net_last_packet_ns,now_ns());
    if(usbFd >= 0 && (g.usb_input_host || g.usb_output_host)) {
        g.usb_audio = usb_host_audio_start(usbFd, rate, usbBitDepth, g.usb_input_host, g.usb_output_host,
                                           g.usb_buffer_min_ms, g.usb_buffer_max_ms,
                                           usbInputBurstPackets < 1 ? 1 : (usbInputBurstPackets > 16 ? 16 : usbInputBurstPackets),
                                           usbOutputBurstPackets < 1 ? 1 : (usbOutputBurstPackets > 16 ? 16 : usbOutputBurstPackets));
        if (!g.usb_audio) { LOGE("USB Host audio initialization failed"); return JNI_FALSE; }
        if (g.usb_input_host) g.in_channels = 2;
    }
    if(!g.usb_input_host && !atomic_load(&g.use_network_input) && !open_stream(&g.input,AAUDIO_DIRECTION_INPUT,channels,inDev,rate,frames)){LOGE("AAudio input open failed");usb_host_audio_stop(g.usb_audio);g.usb_audio=NULL;return JNI_FALSE;}
    if(!g.usb_output_host && !open_stream(&g.output,AAUDIO_DIRECTION_OUTPUT,2,outDev,rate,frames)){LOGE("AAudio output open failed");if(g.input){AAudioStream_close(g.input);g.input=NULL;}usb_host_audio_stop(g.usb_audio);g.usb_audio=NULL;return JNI_FALSE;}
    g.reverb=convolution_reverb_create(rate,g.values[P_ROOM],g.values[P_DECAY],g.values[P_DAMP]);
    g.net_jitter=calloc(NET_JITTER_SLOTS,sizeof(*g.net_jitter));
    network_clear_jitter_state();
    g.look_pos=0;g.look_size=(int)(rate*.006f)*2+4;g.lookahead=calloc((size_t)g.look_size,sizeof(float));
    g.limiter_next_pos=malloc((size_t)g.look_size*sizeof(*g.limiter_next_pos));
    g.limiter_next_delta=calloc((size_t)g.look_size,sizeof(*g.limiter_next_delta));
    if (!g.reverb || !g.lookahead || !g.limiter_next_pos || !g.limiter_next_delta || !g.net_jitter) { LOGE("DSP or network buffer allocation failed"); goto fail; }
    if((g.output && AAudioStream_requestStart(g.output)!=AAUDIO_OK) || (g.input && AAudioStream_requestStart(g.input)!=AAUDIO_OK)) {
        LOGE("AAudioStream_requestStart failed");
        goto fail;
    }
    atomic_store(&g.running,1);
    if (pthread_create(&g.thread,NULL,audio_thread,NULL) != 0) {
        LOGE("audio thread creation failed");
        atomic_store(&g.running,0);
        goto fail;
    }
    atomic_store(&g.reverb_worker_running,1);
    if(pthread_create(&g.reverb_thread,NULL,reverb_worker,NULL)==0) atomic_store(&g.reverb_worker_started,1);
    else { atomic_store(&g.reverb_worker_running,0); LOGE("reverb worker creation failed; live IR updates disabled"); }
    return JNI_TRUE;
fail:
    if (g.input) { AAudioStream_requestStop(g.input); AAudioStream_close(g.input); g.input = NULL; }
    if (g.output) { AAudioStream_requestStop(g.output); AAudioStream_close(g.output); g.output = NULL; }
    convolution_reverb_destroy(g.reverb); g.reverb=NULL;
    usb_host_audio_stop(g.usb_audio); g.usb_audio=NULL;
    free(g.lookahead); g.lookahead=NULL;
    free(g.limiter_next_pos); g.limiter_next_pos=NULL;
    free(g.limiter_next_delta); g.limiter_next_delta=NULL;
    free(g.net_jitter); g.net_jitter=NULL;
    return JNI_FALSE;
}
JNIEXPORT void JNICALL Java_com_llawsxx_audioprocess_NativeAudio_stop(JNIEnv*e,jobject o){(void)e;(void)o;if(!atomic_exchange(&g.running,0))return;if(g.input)AAudioStream_requestStop(g.input);pthread_join(g.thread,NULL);atomic_store(&g.reverb_worker_running,0);if(atomic_exchange(&g.reverb_worker_started,0))pthread_join(g.reverb_thread,NULL);if(g.output)AAudioStream_requestStop(g.output);if(g.input)AAudioStream_close(g.input);if(g.output)AAudioStream_close(g.output);g.input=g.output=NULL;usb_host_audio_stop(g.usb_audio);g.usb_audio=NULL;g.usb_input_host=g.usb_output_host=0;pthread_mutex_lock(&g.reverb_lock);ConvolutionReverb *reverb=g.reverb;g.reverb=NULL;pthread_mutex_unlock(&g.reverb_lock);convolution_reverb_destroy(reverb);free(g.lookahead);g.lookahead=NULL;free(g.limiter_next_pos);g.limiter_next_pos=NULL;free(g.limiter_next_delta);g.limiter_next_delta=NULL;free(g.net_jitter);g.net_jitter=NULL;network_clear_jitter_state();}
JNIEXPORT void JNICALL Java_com_llawsxx_audioprocess_NativeAudio_update(JNIEnv*e,jobject o,jint flags,jfloatArray values){(void)o;float incoming[24]={0};jsize n=(*e)->GetArrayLength(e,values);if(n>24)n=24;(*e)->GetFloatArrayRegion(e,values,0,n,incoming);pthread_mutex_lock(&g.param_lock);int reverb_changed=n>P_DAMP&&(g.values[P_ROOM]!=incoming[P_ROOM]||g.values[P_DECAY]!=incoming[P_DECAY]||g.values[P_DAMP]!=incoming[P_DAMP]);memcpy(g.values,incoming,(size_t)n*sizeof(float));pthread_mutex_unlock(&g.param_lock);if(reverb_changed)atomic_fetch_add(&g.reverb_generation,1);atomic_store(&g.flags,flags);}
JNIEXPORT jfloatArray JNICALL Java_com_llawsxx_audioprocess_NativeAudio_levels(JNIEnv*e,jobject o){(void)o;jfloat v[6];for(int i=0;i<6;i++)v[i]=atomic_load(&g.levels[i]);jfloatArray a=(*e)->NewFloatArray(e,6);(*e)->SetFloatArrayRegion(e,a,0,6,v);return a;}
JNIEXPORT jboolean JNICALL Java_com_llawsxx_audioprocess_NativeAudio_configureNetwork(JNIEnv*e,jobject o,jint role,jint codec,jstring host,jint port,jint minMs,jint maxMs){(void)o;if(codec==1)return JNI_FALSE;const char*h=(*e)->GetStringUTFChars(e,host,NULL);if(g.net_sock>=0)close(g.net_sock);g.net_sock=socket(AF_INET,SOCK_DGRAM,0);if(g.net_sock<0){atomic_store(&g.net_role,0);(*e)->ReleaseStringUTFChars(e,host,h);return JNI_FALSE;}fcntl(g.net_sock,F_SETFL,O_NONBLOCK);memset(&g.net_addr,0,sizeof(g.net_addr));g.net_addr.sin_family=AF_INET;g.net_addr.sin_port=htons((uint16_t)port);inet_aton(h,&g.net_addr.sin_addr);atomic_store(&g.net_role,role);atomic_store(&g.net_last_packet_ns,now_ns());atomic_store(&g.net_packet_seen,0);g.net_codec=codec;g.net_port=port;g.net_min_ms=minMs<0?0:(minMs>200?200:minMs);g.net_max_ms=maxMs<50?50:(maxMs>1000?1000:maxMs);if(g.net_max_ms<g.net_min_ms)g.net_min_ms=g.net_max_ms;g.net_send_count=0;g.net_missing_packets=g.net_late_packets=g.net_duplicate_packets=0;network_clear_jitter_state();if(role==2&&bind(g.net_sock,(struct sockaddr*)&g.net_addr,sizeof(g.net_addr))<0){close(g.net_sock);g.net_sock=-1;atomic_store(&g.net_role,0);(*e)->ReleaseStringUTFChars(e,host,h);return JNI_FALSE;}(*e)->ReleaseStringUTFChars(e,host,h);return JNI_TRUE;}
JNIEXPORT void JNICALL Java_com_llawsxx_audioprocess_NativeAudio_clearNetwork(JNIEnv*e,jobject o){(void)e;(void)o;if(g.net_sock>=0)close(g.net_sock);g.net_sock=-1;atomic_store(&g.net_role,0);g.net_send_count=0;network_clear_jitter_state();}
JNIEXPORT void JNICALL Java_com_llawsxx_audioprocess_NativeAudio_configureUsbOutputBuffer(JNIEnv*e,jobject o,jint minMs,jint maxMs){(void)e;(void)o;g.usb_buffer_min_ms=minMs<8?8:(minMs>200?200:minMs);g.usb_buffer_max_ms=maxMs<8?8:(maxMs>500?500:maxMs);if(g.usb_buffer_max_ms<g.usb_buffer_min_ms)g.usb_buffer_max_ms=g.usb_buffer_min_ms;usb_host_audio_configure_output_buffer(g.usb_audio,g.usb_buffer_min_ms,g.usb_buffer_max_ms);}
JNIEXPORT jboolean JNICALL Java_com_llawsxx_audioprocess_NativeAudio_networkInputTimedOut(JNIEnv*e,jobject o,jint timeoutMs){(void)e;(void)o;if(atomic_load(&g.net_role)!=2||timeoutMs<=0)return JNI_FALSE;uint64_t last=atomic_load(&g.net_last_packet_ns),now=now_ns();return now>last&&(now-last)>=(uint64_t)timeoutMs*1000000ull?JNI_TRUE:JNI_FALSE;}
JNIEXPORT jintArray JNICALL Java_com_llawsxx_audioprocess_NativeAudio_routeInfo(JNIEnv*e,jobject o){(void)o;jint route[4];route[0]=g.usb_input_host?-3:(atomic_load(&g.use_network_input)&&atomic_load(&g.net_role)==2?-2:(g.input?AAudioStream_getDeviceId(g.input):-1));route[1]=g.usb_output_host?-4:(g.output?AAudioStream_getDeviceId(g.output):-1);route[2]=g.usb_input_host?2:(g.input?AAudioStream_getChannelCount(g.input):-1);route[3]=g.usb_output_host?2:(g.output?AAudioStream_getChannelCount(g.output):-1);jintArray result=(*e)->NewIntArray(e,4);(*e)->SetIntArrayRegion(e,result,0,4,route);return result;}
JNIEXPORT jlongArray JNICALL Java_com_llawsxx_audioprocess_NativeAudio_usbStats(JNIEnv*e,jobject o){(void)o;usb_host_audio_stats_t s={0};usb_host_audio_get_stats(g.usb_audio,&s);jlong values[12]={(jlong)s.input_packet_errors,(jlong)s.input_empty_packets,(jlong)s.input_transfer_errors,(jlong)s.input_ring_overruns,(jlong)s.output_transfer_errors,(jlong)s.output_low_water_events,(jlong)s.input_sample_rate,(jlong)s.input_bit_resolution,(jlong)s.input_channels,(jlong)s.output_sample_rate,(jlong)s.output_bit_resolution,(jlong)s.output_channels};jlongArray result=(*e)->NewLongArray(e,12);(*e)->SetLongArrayRegion(e,result,0,12,values);return result;}
JNIEXPORT jboolean JNICALL Java_com_llawsxx_audioprocess_NativeAudio_startRecordingFd(JNIEnv*e,jobject o,jint dryFd,jint wetFd){(void)e;(void)o;pthread_mutex_lock(&g.file_lock);if(g.dry_file){fclose(g.dry_file);g.dry_file=NULL;}if(g.wet_file){fclose(g.wet_file);g.wet_file=NULL;}g.dry_file=fdopen(dup(dryFd),"wb+");g.wet_file=fdopen(dup(wetFd),"wb+");g.dry_bytes=g.wet_bytes=0;if(g.dry_file){uint8_t z[44]={0};fwrite(z,1,44,g.dry_file);}if(g.wet_file){uint8_t z[44]={0};fwrite(z,1,44,g.wet_file);}int ok=g.dry_file&&g.wet_file;if(!ok){if(g.dry_file){fclose(g.dry_file);g.dry_file=NULL;}if(g.wet_file){fclose(g.wet_file);g.wet_file=NULL;}}pthread_mutex_unlock(&g.file_lock);atomic_store(&g.recording,ok);return ok?JNI_TRUE:JNI_FALSE;}
JNIEXPORT void JNICALL Java_com_llawsxx_audioprocess_NativeAudio_stopRecording(JNIEnv*e,jobject o){(void)e;(void)o;atomic_store(&g.recording,0);pthread_mutex_lock(&g.file_lock);if(g.dry_file){wav_header(g.dry_file,g.dry_bytes,g.rate);fclose(g.dry_file);g.dry_file=NULL;}if(g.wet_file){wav_header(g.wet_file,g.wet_bytes,g.rate);fclose(g.wet_file);g.wet_file=NULL;}pthread_mutex_unlock(&g.file_lock);}
