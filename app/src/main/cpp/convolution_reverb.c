#include "convolution_reverb.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define DIRECT_TAPS 128
#define SEGMENT_COUNT 3

/* Independent implementation of the non-uniform partitioning used by FFmpeg afir:
   short partitions cover early reflections and large partitions cover the long tail. */

typedef struct { float re, im; } Complex;

typedef struct {
    int offset;
    int part_size;
    int fft_size;
    int partitions;
    int fill;
    int head;
    float *input[2];
    Complex *coeff[2];
    Complex *history[2];
    Complex *work[2];
    Complex *sum[2];
} ConvolutionSegment;

struct ConvolutionReverb {
    int direct_pos;
    float direct_ir[2][DIRECT_TAPS];
    float direct_history[2][DIRECT_TAPS];
    ConvolutionSegment segments[SEGMENT_COUNT];
    float *output[2];
    int output_size;
    int output_mask;
    uint64_t sample_index;
};

static float clamp_float(float value, float minimum, float maximum) {
    return value < minimum ? minimum : (value > maximum ? maximum : value);
}

static void fft(Complex *values, int size, int inverse) {
    for (int i = 1, j = 0; i < size; ++i) {
        int bit = size >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            Complex temporary = values[i];
            values[i] = values[j];
            values[j] = temporary;
        }
    }

    for (int length = 2; length <= size; length <<= 1) {
        float angle = (inverse ? 2.f : -2.f) * (float)M_PI / (float)length;
        Complex step = { cosf(angle), sinf(angle) };
        for (int start = 0; start < size; start += length) {
            Complex phase = { 1.f, 0.f };
            int half = length >> 1;
            for (int i = 0; i < half; ++i) {
                Complex even = values[start + i];
                Complex odd_value = values[start + i + half];
                Complex odd = {
                    odd_value.re * phase.re - odd_value.im * phase.im,
                    odd_value.re * phase.im + odd_value.im * phase.re
                };
                values[start + i].re = even.re + odd.re;
                values[start + i].im = even.im + odd.im;
                values[start + i + half].re = even.re - odd.re;
                values[start + i + half].im = even.im - odd.im;
                float next_re = phase.re * step.re - phase.im * step.im;
                phase.im = phase.re * step.im + phase.im * step.re;
                phase.re = next_re;
            }
        }
    }

    if (inverse) {
        float scale = 1.f / (float)size;
        for (int i = 0; i < size; ++i) {
            values[i].re *= scale;
            values[i].im *= scale;
        }
    }
}

static void segment_destroy(ConvolutionSegment *segment) {
    for (int channel = 0; channel < 2; ++channel) {
        free(segment->input[channel]);
        free(segment->coeff[channel]);
        free(segment->history[channel]);
        free(segment->work[channel]);
        free(segment->sum[channel]);
    }
    memset(segment, 0, sizeof(*segment));
}

static int segment_create(ConvolutionSegment *segment, int offset, int end,
                          int part_size, float *ir[2]) {
    if (end <= offset) return 1;
    segment->offset = offset;
    segment->part_size = part_size;
    segment->fft_size = part_size * 2;
    segment->partitions = (end - offset + part_size - 1) / part_size;

    size_t spectra_count = (size_t)segment->partitions * (size_t)segment->fft_size;
    for (int channel = 0; channel < 2; ++channel) {
        segment->input[channel] = calloc((size_t)part_size, sizeof(float));
        segment->coeff[channel] = calloc(spectra_count, sizeof(Complex));
        segment->history[channel] = calloc(spectra_count, sizeof(Complex));
        segment->work[channel] = calloc((size_t)segment->fft_size, sizeof(Complex));
        segment->sum[channel] = calloc((size_t)segment->fft_size, sizeof(Complex));
        if (!segment->input[channel] || !segment->coeff[channel] ||
            !segment->history[channel] || !segment->work[channel] || !segment->sum[channel]) {
            segment_destroy(segment);
            return 0;
        }

        for (int partition = 0; partition < segment->partitions; ++partition) {
            Complex *work = segment->work[channel];
            memset(work, 0, (size_t)segment->fft_size * sizeof(Complex));
            int source = offset + partition * part_size;
            int count = end - source;
            if (count > part_size) count = part_size;
            for (int i = 0; i < count; ++i) work[i].re = ir[channel][source + i];
            fft(work, segment->fft_size, 0);
            memcpy(segment->coeff[channel] + (size_t)partition * segment->fft_size,
                   work, (size_t)segment->fft_size * sizeof(Complex));
        }
    }
    return 1;
}

static void segment_render(ConvolutionReverb *reverb, ConvolutionSegment *segment,
                           uint64_t block_start) {
    int fft_size = segment->fft_size;
    for (int channel = 0; channel < 2; ++channel) {
        Complex *work = segment->work[channel];
        memset(work, 0, (size_t)fft_size * sizeof(Complex));
        for (int i = 0; i < segment->part_size; ++i) work[i].re = segment->input[channel][i];
        fft(work, fft_size, 0);
        memcpy(segment->history[channel] + (size_t)segment->head * fft_size,
               work, (size_t)fft_size * sizeof(Complex));

        Complex *sum = segment->sum[channel];
        memset(sum, 0, (size_t)fft_size * sizeof(Complex));
        for (int partition = 0; partition < segment->partitions; ++partition) {
            int history_index = segment->head - partition;
            if (history_index < 0) history_index += segment->partitions;
            const Complex *input = segment->history[channel] + (size_t)history_index * fft_size;
            const Complex *coeff = segment->coeff[channel] + (size_t)partition * fft_size;
            for (int bin = 0; bin < fft_size; ++bin) {
                sum[bin].re += input[bin].re * coeff[bin].re - input[bin].im * coeff[bin].im;
                sum[bin].im += input[bin].re * coeff[bin].im + input[bin].im * coeff[bin].re;
            }
        }
        fft(sum, fft_size, 1);

        uint64_t target = block_start + (uint64_t)segment->offset;
        for (int i = 0; i < fft_size; ++i) {
            int output_index = (int)((target + (uint64_t)i) & (uint64_t)reverb->output_mask);
            reverb->output[channel][output_index] += sum[i].re;
        }
    }
    segment->head = (segment->head + 1) % segment->partitions;
}

static uint32_t random_next(uint32_t *state) {
    *state = *state * 1664525u + 1013904223u;
    return *state;
}

static void generate_ir(float *ir[2], int length, int sample_rate,
                        float room, float decay, float damping) {
    float room_scale = .55f + room * 1.25f;
    int predelay = (int)((.003f + room * .009f) * sample_rate);
    uint32_t seeds[2] = { 0x243F6A88u, 0xB7E15162u };

    for (int channel = 0; channel < 2; ++channel) {
        float lowpass = 0.f;
        float lowpass_coefficient = .08f + (1.f - damping) * .72f;
        for (int i = predelay; i < length; ++i) {
            float seconds = (float)(i - predelay) / (float)sample_rate;
            float noise = (float)((random_next(&seeds[channel]) >> 8) & 0xFFFFu) / 32767.5f - 1.f;
            lowpass += lowpass_coefficient * (noise - lowpass);
            float density = fminf(1.f, seconds * (55.f + room * 90.f));
            float envelope = expf(-6.907755f * seconds / decay);
            ir[channel][i] = lowpass * density * envelope;
        }

        static const float reflections_ms[] = { 5.3f, 8.1f, 11.7f, 16.9f, 23.4f,
                                                31.1f, 39.8f, 51.7f, 67.3f, 83.9f };
        for (int reflection = 0; reflection < 10; ++reflection) {
            float stereo_offset = channel == 0 ? -.37f : .43f;
            int position = predelay + (int)((reflections_ms[reflection] * room_scale +
                                             stereo_offset * reflection) * sample_rate / 1000.f);
            if (position >= 0 && position < length) {
                float sign = ((reflection + channel) & 1) ? -1.f : 1.f;
                ir[channel][position] += sign * (.52f / (1.f + reflection * .38f));
            }
        }

        double energy = 0.0;
        for (int i = 0; i < length; ++i) energy += (double)ir[channel][i] * ir[channel][i];
        float scale = .62f / sqrtf((float)fmax(energy, 1e-9));
        for (int i = 0; i < length; ++i) ir[channel][i] *= scale;
    }
}

ConvolutionReverb *convolution_reverb_create(
        int sample_rate, float room_percent, float decay_seconds, float damping_percent) {
    if (sample_rate <= 0) return NULL;
    float room = clamp_float(room_percent / 100.f, 0.f, 1.f);
    float decay = clamp_float(decay_seconds, .2f, 6.f);
    float damping = clamp_float(damping_percent / 100.f, 0.f, 1.f);
    int ir_length = (int)ceilf(decay * sample_rate);

    ConvolutionReverb *reverb = calloc(1, sizeof(*reverb));
    float *ir[2] = { calloc((size_t)ir_length, sizeof(float)),
                     calloc((size_t)ir_length, sizeof(float)) };
    if (!reverb || !ir[0] || !ir[1]) goto fail;
    generate_ir(ir, ir_length, sample_rate, room, decay, damping);

    int direct_count = ir_length < DIRECT_TAPS ? ir_length : DIRECT_TAPS;
    for (int channel = 0; channel < 2; ++channel) {
        memcpy(reverb->direct_ir[channel], ir[channel], (size_t)direct_count * sizeof(float));
    }

    int first_end = ir_length < 2048 ? ir_length : 2048;
    int second_end = ir_length < 24576 ? ir_length : 24576;
    if (!segment_create(&reverb->segments[0], DIRECT_TAPS, first_end, 128, ir) ||
        !segment_create(&reverb->segments[1], 2048, second_end, 1024, ir) ||
        !segment_create(&reverb->segments[2], 24576, ir_length, 4096, ir)) goto fail;

    int required_output = 24576 + 8192 + 1;
    reverb->output_size = 1;
    while (reverb->output_size < required_output) reverb->output_size <<= 1;
    reverb->output_mask = reverb->output_size - 1;
    reverb->output[0] = calloc((size_t)reverb->output_size, sizeof(float));
    reverb->output[1] = calloc((size_t)reverb->output_size, sizeof(float));
    if (!reverb->output[0] || !reverb->output[1]) goto fail;

    free(ir[0]);
    free(ir[1]);
    return reverb;

fail:
    free(ir[0]);
    free(ir[1]);
    convolution_reverb_destroy(reverb);
    return NULL;
}

void convolution_reverb_destroy(ConvolutionReverb *reverb) {
    if (!reverb) return;
    for (int i = 0; i < SEGMENT_COUNT; ++i) segment_destroy(&reverb->segments[i]);
    free(reverb->output[0]);
    free(reverb->output[1]);
    free(reverb);
}

void convolution_reverb_process(ConvolutionReverb *reverb, float input_l, float input_r,
                                float *wet_l, float *wet_r) {
    if (!reverb || !wet_l || !wet_r) return;
    int output_index = (int)(reverb->sample_index & (uint64_t)reverb->output_mask);
    float wet[2] = { reverb->output[0][output_index], reverb->output[1][output_index] };
    reverb->output[0][output_index] = 0.f;
    reverb->output[1][output_index] = 0.f;

    float input[2] = { input_l, input_r };
    for (int channel = 0; channel < 2; ++channel) {
        reverb->direct_history[channel][reverb->direct_pos] = input[channel];
        int history_position = reverb->direct_pos;
        for (int tap = 0; tap < DIRECT_TAPS; ++tap) {
            wet[channel] += reverb->direct_history[channel][history_position] *
                            reverb->direct_ir[channel][tap];
            if (--history_position < 0) history_position = DIRECT_TAPS - 1;
        }
    }

    for (int index = 0; index < SEGMENT_COUNT; ++index) {
        ConvolutionSegment *segment = &reverb->segments[index];
        if (segment->partitions == 0) continue;
        segment->input[0][segment->fill] = input_l;
        segment->input[1][segment->fill] = input_r;
        if (++segment->fill == segment->part_size) {
            uint64_t block_start = reverb->sample_index + 1u - (uint64_t)segment->part_size;
            segment_render(reverb, segment, block_start);
            segment->fill = 0;
        }
    }

    reverb->direct_pos = (reverb->direct_pos + 1) % DIRECT_TAPS;
    ++reverb->sample_index;
    *wet_l = wet[0];
    *wet_r = wet[1];
}
