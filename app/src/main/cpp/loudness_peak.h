#ifndef AUDIOPROCESS_LOUDNESS_PEAK_H
#define AUDIOPROCESS_LOUDNESS_PEAK_H

#include <math.h>
#include <stdint.h>
#include <string.h>

#define LOUDNESS_LOOKAHEAD_CAPACITY 8192

typedef struct {
    float samples[LOUDNESS_LOOKAHEAD_CAPACITY * 2];
    float scheduled_gain[LOUDNESS_LOOKAHEAD_CAPACITY];
    float required_gain[LOUDNESS_LOOKAHEAD_CAPACITY];
    float base_gain[LOUDNESS_LOOKAHEAD_CAPACITY];
    float future_min_gain[LOUDNESS_LOOKAHEAD_CAPACITY];
    uint64_t future_min_index[LOUDNESS_LOOKAHEAD_CAPACITY];
    int delay_frames;
    int position;
    int future_min_head, future_min_tail, future_min_count;
    uint64_t frame_index;
    double peak_gain;
    double release_coeff;
} LoudnessPeakLookahead;

static void loudness_peak_reset(LoudnessPeakLookahead *state, int delay_frames) {
    if (delay_frames < 1) delay_frames = 1;
    if (delay_frames >= LOUDNESS_LOOKAHEAD_CAPACITY)
        delay_frames = LOUDNESS_LOOKAHEAD_CAPACITY - 1;
    state->delay_frames = delay_frames;
    state->position = 0;
    state->future_min_head = state->future_min_tail = state->future_min_count = 0;
    state->frame_index = 0;
    state->peak_gain = 1.0;
    memset(state->samples, 0, sizeof(state->samples));
    for (int i = 0; i < LOUDNESS_LOOKAHEAD_CAPACITY; ++i) {
        state->scheduled_gain[i] = 1.f;
        state->required_gain[i] = 1.f;
        state->base_gain[i] = 1.f;
    }
}

/* Input is normalized before entering the ring. Each overload writes a linear
 * attack into the delayed frames, so gain reaches its target with the peak. */
static void loudness_peak_process(LoudnessPeakLookahead *state,
                                  float *left, float *right,
                                  double base_gain, double peak_limit,
                                  int boost_only, float *applied_gain) {
    const int delay = state->delay_frames;
    const int ring_size = delay + 1;
    const int write_pos = state->position;
    if (boost_only) base_gain = fmax(base_gain, 1.0);
    float x0 = (float)((double)*left * base_gain);
    float x1 = (float)((double)*right * base_gain);
    state->samples[write_pos * 2] = x0;
    state->samples[write_pos * 2 + 1] = x1;
    state->base_gain[write_pos] = (float)base_gain;
    state->scheduled_gain[write_pos] = 1.f;

    double peak = fmax(fabs((double)x0), fabs((double)x1));
    double required_gain = peak > peak_limit ? peak_limit / peak : 1.0;
    state->required_gain[write_pos] = (float)required_gain;
    while (state->future_min_count > 0 &&
           state->future_min_index[state->future_min_head] + (uint64_t)delay <
                   state->frame_index) {
        state->future_min_head = (state->future_min_head + 1) %
                                 LOUDNESS_LOOKAHEAD_CAPACITY;
        state->future_min_count--;
    }
    while (state->future_min_count > 0) {
        int last = (state->future_min_tail + LOUDNESS_LOOKAHEAD_CAPACITY - 1) %
                   LOUDNESS_LOOKAHEAD_CAPACITY;
        if (state->future_min_gain[last] < required_gain) break;
        state->future_min_tail = last;
        state->future_min_count--;
    }
    state->future_min_gain[state->future_min_tail] = (float)required_gain;
    state->future_min_index[state->future_min_tail] = state->frame_index++;
    state->future_min_tail = (state->future_min_tail + 1) %
                             LOUDNESS_LOOKAHEAD_CAPACITY;
    state->future_min_count++;
    if (peak > peak_limit) {
        if (required_gain < state->peak_gain) {
            double attack = required_gain;
            double attack_step = (1.0 - required_gain) / (double)delay;
            for (int age = 0; age < delay; ++age) {
                int pos = write_pos - age;
                if (pos < 0) pos += ring_size;
                if (attack < state->scheduled_gain[pos])
                    state->scheduled_gain[pos] = (float)attack;
                attack += attack_step;
            }
        }
    }

    int output_pos = write_pos + 1;
    if (output_pos == ring_size) output_pos = 0;
    double released_gain = state->peak_gain;
    double future_min = state->future_min_gain[state->future_min_head];
    if (future_min > released_gain)
        released_gain = fmin(future_min,
                released_gain + (1.0 - released_gain) * state->release_coeff);
    state->peak_gain = fmin(fmin(state->scheduled_gain[output_pos],
                                 state->required_gain[output_pos]), released_gain);
    double total_gain = (double)state->base_gain[output_pos] * state->peak_gain;
    if (boost_only) total_gain = fmax(total_gain, 1.0);
    *applied_gain = (float)total_gain;
    double output_gain = total_gain / (double)state->base_gain[output_pos];
    *left = (float)((double)state->samples[output_pos * 2] * output_gain);
    *right = (float)((double)state->samples[output_pos * 2 + 1] * output_gain);
    state->position = output_pos;
}

#endif
