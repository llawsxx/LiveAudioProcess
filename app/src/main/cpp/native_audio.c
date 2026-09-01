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

#define TAG "PulseForgeNative"
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define MAX_FRAMES 2048
#define NET_BUFFER_MAX_MS 1000
#define EQ_BANDS 4

typedef struct { float b0,b1,b2,a1,a2,z1,z2,freq,gain,q; } Biquad;
typedef struct {
    AAudioStream *input, *output;
    pthread_t thread;
    atomic_int running, recording, flags;
    _Atomic(float) levels[4];
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
    float limiter_gain;
    FILE *dry_file, *wet_file;
    uint32_t dry_bytes, wet_bytes;
    int net_sock, net_codec, net_port, net_min_ms, net_max_ms;
    atomic_int net_role, use_network_input, net_packet_seen;
    atomic_ullong net_last_packet_ns;
    float *net_buffer;
    int net_capacity, net_read, net_write, net_count;
    int net_started;
    struct sockaddr_in net_addr;
    uint32_t net_seq;
} Engine;

static Engine g = {
    .net_sock = -1,
    .param_lock = PTHREAD_MUTEX_INITIALIZER,
    .file_lock = PTHREAD_MUTEX_INITIALIZER,
    .reverb_lock = PTHREAD_MUTEX_INITIALIZER
};

typedef struct __attribute__((packed)) { uint32_t magic; uint16_t version; uint8_t codec; uint8_t channels; uint32_t rate; uint32_t frames; uint64_t timestamp_ns; uint32_t sequence; } NetHeader;
#define NET_MAGIC 0x50464C58u

enum { DSP_ON=1, EQ_ON=2, REVERB_ON=4, LIMITER_ON=8 };
enum {
    P_EQ1_F,P_EQ1_G,P_EQ1_Q,P_EQ2_F,P_EQ2_G,P_EQ2_Q,P_EQ3_F,P_EQ3_G,P_EQ3_Q,P_EQ4_F,P_EQ4_G,P_EQ4_Q,
    P_ROOM,P_DECAY,P_DAMP,P_MIX,P_LIM_IN,P_LIMIT,P_RELEASE,P_CEILING,P_LOOKAHEAD
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

/* Linked-stereo lookahead limiter based on FFmpeg af_alimiter's ring-buffer approach. */
static void limiter_process(float *l, float *r, const float *p) {
    float in=db_to_linear(p[P_LIM_IN]), limit=db_to_linear(p[P_LIMIT]);
    float ceiling=db_to_linear(p[P_CEILING]), release=fmaxf(p[P_RELEASE],10.f)/1000.f;
    float x0=*l*in, x1=*r*in, peak=fmaxf(fabsf(x0),fabsf(x1));
    int delay=(int)(clampf(p[P_LOOKAHEAD],0.f,5.f)*g.rate/1000.f)*2;
    if(delay<2) delay=2; if(delay>=g.look_size)delay=g.look_size-2;
    int read=g.look_pos-delay; if(read<0)read+=g.look_size;
    float out0=g.lookahead[read], out1=g.lookahead[(read+1)%g.look_size];
    g.lookahead[g.look_pos]=x0; g.lookahead[(g.look_pos+1)%g.look_size]=x1;
    g.look_pos=(g.look_pos+2)%g.look_size;
    float target=peak>limit?limit/peak:1.f;
    if(target<g.limiter_gain)g.limiter_gain=target;
    else g.limiter_gain += (1.f-g.limiter_gain)/(g.rate*release);
    *l=clampf(out0*g.limiter_gain,-ceiling,ceiling);
    *r=clampf(out1*g.limiter_gain,-ceiling,ceiling);
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
static void network_send(const float *data, int frames) {
    if (g.net_sock < 0 || g.net_role != 1) return;
    NetHeader h={NET_MAGIC,1,(uint8_t)g.net_codec,2,(uint32_t)g.rate,(uint32_t)frames,now_ns(),g.net_seq++};
    uint8_t packet[sizeof(NetHeader)+MAX_FRAMES*2*sizeof(float)]; memcpy(packet,&h,sizeof(h)); memcpy(packet+sizeof(h),data,(size_t)frames*2*sizeof(float));
    sendto(g.net_sock,packet,sizeof(h)+(size_t)frames*2*sizeof(float),0,(struct sockaddr*)&g.net_addr,sizeof(g.net_addr));
}
static void network_fill(void) {
    if (g.net_sock < 0 || g.net_role != 2 || !g.net_buffer) return;
    uint8_t packet[sizeof(NetHeader)+MAX_FRAMES*2*sizeof(float)];
    for (;;) {
        ssize_t n=recvfrom(g.net_sock,packet,sizeof(packet),MSG_DONTWAIT,NULL,NULL);
        if(n<(ssize_t)sizeof(NetHeader)) return;
        NetHeader h; memcpy(&h,packet,sizeof(h));
        if(h.magic!=NET_MAGIC || h.channels!=2 || h.rate!=(uint32_t)g.rate || h.codec!=(uint8_t)g.net_codec) continue;
        int count=(int)h.frames; int payload_frames=(int)((n-(ssize_t)sizeof(NetHeader))/(2*sizeof(float)));
        if(count<=0 || payload_frames<=0) continue; if(count>payload_frames) count=payload_frames; if(count>MAX_FRAMES) count=MAX_FRAMES;
        atomic_store(&g.net_last_packet_ns,now_ns());
        if(!atomic_exchange(&g.net_packet_seen,1))LOGI("first valid Wi-Fi packet: rate=%u frames=%d",h.rate,count);
        const float *src=(const float *)(packet+sizeof(h));
        for(int i=0;i<count;i++) {
            if(g.net_count>=g.net_capacity) { g.net_read=(g.net_read+1)%g.net_capacity; g.net_count--; }
            g.net_buffer[g.net_write*2]=src[i*2]; g.net_buffer[g.net_write*2+1]=src[i*2+1];
            g.net_write=(g.net_write+1)%g.net_capacity; g.net_count++;
        }
    }
}
static int network_receive(float *data, int frames) {
    network_fill();
    if (g.net_sock < 0 || g.net_role != 2 || !g.net_buffer) return 0;
    int min_frames=(g.rate*g.net_min_ms)/1000;
    int max_frames=(g.rate*g.net_max_ms)/1000;
    int target_frames=(min_frames+max_frames)/2;
    if (target_frames<1) target_frames=1;
    if (max_frames<target_frames) max_frames=target_frames;
    if (!g.net_started) {
        if (g.net_count>=target_frames) g.net_started=1;
        else { memset(data,0,(size_t)frames*2*sizeof(float)); return frames; }
    }
    if (g.net_count>=max_frames) {
        int drop=g.net_count-target_frames;
        while(drop-->0) { g.net_read=(g.net_read+1)%g.net_capacity; g.net_count--; }
    }
    if (g.net_count<min_frames) {
        g.net_started=0; memset(data,0,(size_t)frames*2*sizeof(float)); return frames;
    }
    int take=g.net_count<frames?g.net_count:frames;
    for(int i=0;i<take;i++) { data[i*2]=g.net_buffer[g.net_read*2]; data[i*2+1]=g.net_buffer[g.net_read*2+1]; g.net_read=(g.net_read+1)%g.net_capacity; }
    g.net_count-=take;
    if(take<frames) memset(data+take*2,0,(size_t)(frames-take)*2*sizeof(float));
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
        else if(g.input){if(atomic_load(&g.net_role)==2)network_fill();got=AAudioStream_read(g.input,input,want,100000000);}
        else { memset(input,0,(size_t)want*4*sizeof(float)); got=want; }
        if(got<=0)continue;
        pthread_mutex_lock(&g.param_lock); memcpy(p,g.values,sizeof(p)); pthread_mutex_unlock(&g.param_lock);
        int flags=atomic_load(&g.flags); float peaks[4]={0.f,0.f,0.f,0.f};
        ConvolutionReverb *reverb=NULL;
        if((flags&REVERB_ON) && (flags&DSP_ON)) {
            pthread_mutex_lock(&g.reverb_lock);
            reverb=g.reverb;
        }
        for(int i=0;i<got;i++) {
            int first=g.pair*2; if(first>=g.in_channels)first=0;
            int second=first+1; if(second>=g.in_channels)second=first;
            float l = using_network ? output[i*2] : input[i*g.in_channels+first];
            float r = using_network ? output[i*2+1] : input[i*g.in_channels+second];
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
        AAudioStream_write(g.output,output,got,100000000); record_samples(dry,output,got);
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

JNIEXPORT jboolean JNICALL Java_com_llawsxx_audioprocess_NativeAudio_start(JNIEnv*e,jobject o,jint rate,jint frames,jint inDev,jint outDev,jint channels,jint pair,jboolean useNetworkInput){
    (void)e;(void)o;if(atomic_load(&g.running))return JNI_TRUE;memset(g.eq,0,sizeof(g.eq));g.rate=rate;g.frames=frames;g.in_channels=channels;g.pair=pair;g.limiter_gain=1.f;atomic_store(&g.use_network_input,useNetworkInput?1:0);if(useNetworkInput)atomic_store(&g.net_last_packet_ns,now_ns());
    if(!atomic_load(&g.use_network_input) && !open_stream(&g.input,AAUDIO_DIRECTION_INPUT,channels,inDev,rate,frames)){LOGE("AAudio input open failed");return JNI_FALSE;}
    if(!open_stream(&g.output,AAUDIO_DIRECTION_OUTPUT,2,outDev,rate,frames)){LOGE("AAudio output open failed");if(g.input){AAudioStream_close(g.input);g.input=NULL;}return JNI_FALSE;}
    g.reverb=convolution_reverb_create(rate,g.values[P_ROOM],g.values[P_DECAY],g.values[P_DAMP]);
    g.net_capacity=(int)((int64_t)rate*NET_BUFFER_MAX_MS/1000)+MAX_FRAMES;
    g.net_buffer=calloc((size_t)g.net_capacity*2,sizeof(float)); g.net_read=g.net_write=g.net_count=0; g.net_started=0;
    g.look_size=(int)(rate*.006f)*2+4;g.lookahead=calloc((size_t)g.look_size,sizeof(float));
    if (!g.reverb || !g.lookahead || !g.net_buffer) { LOGE("DSP or network buffer allocation failed"); goto fail; }
    if(AAudioStream_requestStart(g.output)!=AAUDIO_OK || (g.input && AAudioStream_requestStart(g.input)!=AAUDIO_OK)) {
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
    free(g.lookahead); g.lookahead=NULL;
    free(g.net_buffer); g.net_buffer=NULL;
    return JNI_FALSE;
}
JNIEXPORT void JNICALL Java_com_llawsxx_audioprocess_NativeAudio_stop(JNIEnv*e,jobject o){(void)e;(void)o;if(!atomic_exchange(&g.running,0))return;if(g.input)AAudioStream_requestStop(g.input);pthread_join(g.thread,NULL);atomic_store(&g.reverb_worker_running,0);if(atomic_exchange(&g.reverb_worker_started,0))pthread_join(g.reverb_thread,NULL);if(g.output)AAudioStream_requestStop(g.output);if(g.input)AAudioStream_close(g.input);if(g.output)AAudioStream_close(g.output);g.input=g.output=NULL;pthread_mutex_lock(&g.reverb_lock);ConvolutionReverb *reverb=g.reverb;g.reverb=NULL;pthread_mutex_unlock(&g.reverb_lock);convolution_reverb_destroy(reverb);free(g.lookahead);g.lookahead=NULL;free(g.net_buffer);g.net_buffer=NULL;g.net_capacity=g.net_count=0;}
JNIEXPORT void JNICALL Java_com_llawsxx_audioprocess_NativeAudio_update(JNIEnv*e,jobject o,jint flags,jfloatArray values){(void)o;float incoming[24]={0};jsize n=(*e)->GetArrayLength(e,values);if(n>24)n=24;(*e)->GetFloatArrayRegion(e,values,0,n,incoming);pthread_mutex_lock(&g.param_lock);int reverb_changed=n>P_DAMP&&(g.values[P_ROOM]!=incoming[P_ROOM]||g.values[P_DECAY]!=incoming[P_DECAY]||g.values[P_DAMP]!=incoming[P_DAMP]);memcpy(g.values,incoming,(size_t)n*sizeof(float));pthread_mutex_unlock(&g.param_lock);if(reverb_changed)atomic_fetch_add(&g.reverb_generation,1);atomic_store(&g.flags,flags);}
JNIEXPORT jfloatArray JNICALL Java_com_llawsxx_audioprocess_NativeAudio_levels(JNIEnv*e,jobject o){(void)o;jfloat v[4];for(int i=0;i<4;i++)v[i]=atomic_load(&g.levels[i]);jfloatArray a=(*e)->NewFloatArray(e,4);(*e)->SetFloatArrayRegion(e,a,0,4,v);return a;}
JNIEXPORT jboolean JNICALL Java_com_llawsxx_audioprocess_NativeAudio_configureNetwork(JNIEnv*e,jobject o,jint role,jint codec,jstring host,jint port,jint minMs,jint maxMs){(void)o;if(codec==1)return JNI_FALSE;const char*h=(*e)->GetStringUTFChars(e,host,NULL);if(g.net_sock>=0)close(g.net_sock);g.net_sock=socket(AF_INET,SOCK_DGRAM,0);if(g.net_sock<0){atomic_store(&g.net_role,0);(*e)->ReleaseStringUTFChars(e,host,h);return JNI_FALSE;}fcntl(g.net_sock,F_SETFL,O_NONBLOCK);memset(&g.net_addr,0,sizeof(g.net_addr));g.net_addr.sin_family=AF_INET;g.net_addr.sin_port=htons((uint16_t)port);inet_aton(h,&g.net_addr.sin_addr);atomic_store(&g.net_role,role);atomic_store(&g.net_last_packet_ns,now_ns());atomic_store(&g.net_packet_seen,0);g.net_codec=codec;g.net_port=port;g.net_min_ms=minMs<0?0:(minMs>200?200:minMs);g.net_max_ms=maxMs<50?50:(maxMs>1000?1000:maxMs);if(g.net_max_ms<g.net_min_ms)g.net_min_ms=g.net_max_ms;g.net_read=g.net_write=g.net_count=0;g.net_started=0;if(role==2&&bind(g.net_sock,(struct sockaddr*)&g.net_addr,sizeof(g.net_addr))<0){close(g.net_sock);g.net_sock=-1;atomic_store(&g.net_role,0);(*e)->ReleaseStringUTFChars(e,host,h);return JNI_FALSE;}(*e)->ReleaseStringUTFChars(e,host,h);return JNI_TRUE;}
JNIEXPORT void JNICALL Java_com_llawsxx_audioprocess_NativeAudio_clearNetwork(JNIEnv*e,jobject o){(void)e;(void)o;if(g.net_sock>=0)close(g.net_sock);g.net_sock=-1;atomic_store(&g.net_role,0);g.net_read=g.net_write=g.net_count=0;g.net_started=0;}
JNIEXPORT jboolean JNICALL Java_com_llawsxx_audioprocess_NativeAudio_networkInputTimedOut(JNIEnv*e,jobject o,jint timeoutMs){(void)e;(void)o;if(atomic_load(&g.net_role)!=2||timeoutMs<=0)return JNI_FALSE;uint64_t last=atomic_load(&g.net_last_packet_ns),now=now_ns();return now>last&&(now-last)>=(uint64_t)timeoutMs*1000000ull?JNI_TRUE:JNI_FALSE;}
JNIEXPORT jintArray JNICALL Java_com_llawsxx_audioprocess_NativeAudio_routeInfo(JNIEnv*e,jobject o){(void)o;jint route[2];route[0]=atomic_load(&g.use_network_input)&&atomic_load(&g.net_role)==2?-2:(g.input?AAudioStream_getDeviceId(g.input):-1);route[1]=g.output?AAudioStream_getDeviceId(g.output):-1;jintArray result=(*e)->NewIntArray(e,2);(*e)->SetIntArrayRegion(e,result,0,2,route);return result;}
JNIEXPORT jboolean JNICALL Java_com_llawsxx_audioprocess_NativeAudio_startRecordingFd(JNIEnv*e,jobject o,jint dryFd,jint wetFd){(void)e;(void)o;pthread_mutex_lock(&g.file_lock);if(g.dry_file){fclose(g.dry_file);g.dry_file=NULL;}if(g.wet_file){fclose(g.wet_file);g.wet_file=NULL;}g.dry_file=fdopen(dup(dryFd),"wb+");g.wet_file=fdopen(dup(wetFd),"wb+");g.dry_bytes=g.wet_bytes=0;if(g.dry_file){uint8_t z[44]={0};fwrite(z,1,44,g.dry_file);}if(g.wet_file){uint8_t z[44]={0};fwrite(z,1,44,g.wet_file);}int ok=g.dry_file&&g.wet_file;if(!ok){if(g.dry_file){fclose(g.dry_file);g.dry_file=NULL;}if(g.wet_file){fclose(g.wet_file);g.wet_file=NULL;}}pthread_mutex_unlock(&g.file_lock);atomic_store(&g.recording,ok);return ok?JNI_TRUE:JNI_FALSE;}
JNIEXPORT void JNICALL Java_com_llawsxx_audioprocess_NativeAudio_stopRecording(JNIEnv*e,jobject o){(void)e;(void)o;atomic_store(&g.recording,0);pthread_mutex_lock(&g.file_lock);if(g.dry_file){wav_header(g.dry_file,g.dry_bytes,g.rate);fclose(g.dry_file);g.dry_file=NULL;}if(g.wet_file){wav_header(g.wet_file,g.wet_bytes,g.rate);fclose(g.wet_file);g.wet_file=NULL;}pthread_mutex_unlock(&g.file_lock);}
