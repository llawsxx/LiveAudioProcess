#pragma once

#include <stdint.h>

#define INPUT_CLOCK_HISTORY 256

typedef enum {
    INPUT_CLOCK_DISABLED = 0,
    INPUT_CLOCK_NO_INPUT = 1,
    INPUT_CLOCK_NO_OUTPUT = 2,
    INPUT_CLOCK_MEASURING = 3,
    INPUT_CLOCK_ACTIVE = 4,
    INPUT_CLOCK_WAITING = 5
} InputClockStatus;

typedef struct {
    uint64_t frames[INPUT_CLOCK_HISTORY];
    uint64_t times_ns[INPUT_CLOCK_HISTORY];
    int count;
    int next;
} InputClockHistory;

typedef struct {
    InputClockHistory input;
    InputClockHistory output;
    double estimated_input_rate;
    double drift_ppm;
    double ratio;
    double queue_average;
    double span_seconds;
    uint64_t last_update_ns;
    int ready;
    int queue_initialized;
} InputClockCorrection;

void input_clock_correction_reset(InputClockCorrection *state);
int input_clock_correction_update(InputClockCorrection *state,
        uint64_t input_frames, uint64_t input_time_ns,
        uint64_t output_frames, uint64_t output_time_ns,
        int input_rate, int output_rate, double queued_frames,
        double target_frames, uint64_t now_ns, int enabled);
