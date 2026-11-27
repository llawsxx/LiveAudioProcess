#pragma once

#include "parametric_eq.h"

#define EXCITER_PARAMETER_COUNT 6
#define EXCITER_OVERSAMPLING 4
#define EXCITER_FIR_TAPS 129
#define EXCITER_INPUT_HISTORY 33

typedef struct {
    int rate, input_position, output_position, remaining, processing;
    double coefficients[EXCITER_FIR_TAPS];
    double input_history[2][EXCITER_INPUT_HISTORY];
    double output_history[2][EXCITER_FIR_TAPS];
    double current[4], target[4], step[4];
    ParametricEq input_high_pass, output_high_pass, output_low_pass;
} HarmonicExciter;

void harmonic_exciter_init(HarmonicExciter *exciter, int rate);
void harmonic_exciter_prepare(HarmonicExciter *exciter, int rate, int enabled,
        const float parameters[EXCITER_PARAMETER_COUNT]);
void harmonic_exciter_process(HarmonicExciter *exciter, float *left, float *right);
