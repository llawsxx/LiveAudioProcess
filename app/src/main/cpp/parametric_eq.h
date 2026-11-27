#pragma once

#include <stddef.h>

#define PARAMETRIC_EQ_BAND_VALUES 5

typedef enum {
    EQ_BELL = 0,
    EQ_HIGH_PASS = 1,
    EQ_LOW_PASS = 2,
    EQ_NOTCH = 3,
    EQ_BAND_PASS = 4,
    EQ_LOW_SHELF = 5,
    EQ_HIGH_SHELF = 6
} EqFilterType;

typedef struct {
    double current[5], target[5], step[5];
    double state[2][2];
    int remaining, initialized, type, enabled, rate;
    float frequency, gain, quality;
} ParametricEq;

typedef struct {
    float values[PARAMETRIC_EQ_BAND_VALUES];
    ParametricEq filter;
} ParametricEqBand;

typedef struct {
    size_t count;
    ParametricEqBand bands[];
} ParametricEqBank;

void parametric_eq_design(int type, int rate, float frequency, float gain,
        float quality, double coefficients[5]);
void parametric_eq_prepare(ParametricEq *filter, int type, int enabled,
        int rate, float frequency, float gain, float quality);
void parametric_eq_process(ParametricEq *filter, float *left, float *right);
ParametricEqBank *parametric_eq_bank_create(size_t count);
void parametric_eq_bank_inherit(ParametricEqBank *bank, const ParametricEqBank *previous);
void parametric_eq_bank_prepare(ParametricEqBank *bank, int rate, int enabled);
void parametric_eq_bank_process(ParametricEqBank *bank, float *left, float *right);
