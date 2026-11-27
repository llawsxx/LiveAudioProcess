#include "parametric_eq.h"

#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

ParametricEqBank *parametric_eq_bank_create(size_t count) {
    if (count > (SIZE_MAX - sizeof(ParametricEqBank)) / sizeof(ParametricEqBand)) return NULL;
    ParametricEqBank *bank = calloc(1, sizeof(ParametricEqBank) + count * sizeof(ParametricEqBand));
    if (bank) bank->count = count;
    return bank;
}

void parametric_eq_bank_inherit(ParametricEqBank *bank, const ParametricEqBank *previous) {
    if (!bank || !previous) return;
    if (bank->count == previous->count) {
        for (size_t index = 0; index < bank->count; index++)
            bank->bands[index].filter = previous->bands[index].filter;
        return;
    }
    size_t prefix = 0;
    while (prefix < bank->count && prefix < previous->count &&
            memcmp(bank->bands[prefix].values, previous->bands[prefix].values,
                    sizeof(bank->bands[prefix].values)) == 0) {
        bank->bands[prefix].filter = previous->bands[prefix].filter;
        prefix++;
    }
    size_t current_end = bank->count, previous_end = previous->count;
    while (current_end > prefix && previous_end > prefix &&
            memcmp(bank->bands[current_end - 1].values, previous->bands[previous_end - 1].values,
                    sizeof(bank->bands[current_end - 1].values)) == 0) {
        bank->bands[--current_end].filter = previous->bands[--previous_end].filter;
    }
}

void parametric_eq_bank_prepare(ParametricEqBank *bank, int rate, int enabled) {
    if (!bank) return;
    for (size_t index = 0; index < bank->count; index++) {
        ParametricEqBand *band = &bank->bands[index];
        if (enabled) {
            parametric_eq_prepare(&band->filter, (int)band->values[3], band->values[4] >= 0.5f,
                    rate, band->values[0], band->values[1], band->values[2]);
        } else {
            memset(&band->filter, 0, sizeof(band->filter));
        }
    }
}

void parametric_eq_bank_process(ParametricEqBank *bank, float *left, float *right) {
    if (!bank) return;
    for (size_t index = 0; index < bank->count; index++)
        parametric_eq_process(&bank->bands[index].filter, left, right);
}

static double bounded(double value, double minimum, double maximum) {
    return value < minimum ? minimum : (value > maximum ? maximum : value);
}

void parametric_eq_design(int type, int rate, float frequency, float gain,
        float quality, double coefficients[5]) {
    if (rate <= 0) rate = 48000;
    double center = bounded(frequency, 20.0, rate * 0.49);
    double amplitude = pow(10.0, bounded(gain, -24.0, 24.0) / 40.0);
    double omega = 6.28318530717958647692 * center / rate;
    double cosine = cos(omega);
    double alpha = sin(omega) / (2.0 * bounded(quality, 0.1, 20.0));
    double numerator0, numerator1, numerator2;
    double denominator0 = 1.0 + alpha;
    double denominator1 = -2.0 * cosine;
    double denominator2 = 1.0 - alpha;

    switch (type) {
        case EQ_HIGH_PASS:
            numerator0 = (1.0 + cosine) / 2.0;
            numerator1 = -(1.0 + cosine);
            numerator2 = numerator0;
            break;
        case EQ_LOW_PASS:
            numerator0 = (1.0 - cosine) / 2.0;
            numerator1 = 1.0 - cosine;
            numerator2 = numerator0;
            break;
        case EQ_NOTCH:
            numerator0 = 1.0;
            numerator1 = -2.0 * cosine;
            numerator2 = 1.0;
            break;
        case EQ_BAND_PASS:
            numerator0 = alpha;
            numerator1 = 0.0;
            numerator2 = -alpha;
            break;
        case EQ_LOW_SHELF: {
            double shelf = 2.0 * sqrt(amplitude) * alpha;
            numerator0 = amplitude * ((amplitude + 1.0) - (amplitude - 1.0) * cosine + shelf);
            numerator1 = 2.0 * amplitude * ((amplitude - 1.0) - (amplitude + 1.0) * cosine);
            numerator2 = amplitude * ((amplitude + 1.0) - (amplitude - 1.0) * cosine - shelf);
            denominator0 = (amplitude + 1.0) + (amplitude - 1.0) * cosine + shelf;
            denominator1 = -2.0 * ((amplitude - 1.0) + (amplitude + 1.0) * cosine);
            denominator2 = (amplitude + 1.0) + (amplitude - 1.0) * cosine - shelf;
            break;
        }
        case EQ_HIGH_SHELF: {
            double shelf = 2.0 * sqrt(amplitude) * alpha;
            numerator0 = amplitude * ((amplitude + 1.0) + (amplitude - 1.0) * cosine + shelf);
            numerator1 = -2.0 * amplitude * ((amplitude - 1.0) + (amplitude + 1.0) * cosine);
            numerator2 = amplitude * ((amplitude + 1.0) + (amplitude - 1.0) * cosine - shelf);
            denominator0 = (amplitude + 1.0) - (amplitude - 1.0) * cosine + shelf;
            denominator1 = 2.0 * ((amplitude - 1.0) - (amplitude + 1.0) * cosine);
            denominator2 = (amplitude + 1.0) - (amplitude - 1.0) * cosine - shelf;
            break;
        }
        default:
            numerator0 = 1.0 + alpha * amplitude;
            numerator1 = -2.0 * cosine;
            numerator2 = 1.0 - alpha * amplitude;
            denominator0 = 1.0 + alpha / amplitude;
            denominator2 = 1.0 - alpha / amplitude;
            break;
    }
    coefficients[0] = numerator0 / denominator0;
    coefficients[1] = numerator1 / denominator0;
    coefficients[2] = numerator2 / denominator0;
    coefficients[3] = denominator1 / denominator0;
    coefficients[4] = denominator2 / denominator0;
}

void parametric_eq_prepare(ParametricEq *filter, int type, int enabled,
        int rate, float frequency, float gain, float quality) {
    if (rate <= 0) rate = 48000;
    if (type < EQ_BELL || type > EQ_HIGH_SHELF) type = EQ_BELL;
    enabled = enabled != 0;
    frequency = (float)bounded(frequency, 20.0, rate * 0.49);
    gain = (float)bounded(gain, -24.0, 24.0);
    quality = (float)bounded(quality, 0.1, 20.0);
    if (filter->initialized && filter->type == type && filter->enabled == enabled &&
            filter->rate == rate && filter->frequency == frequency &&
            filter->gain == gain && filter->quality == quality) return;
    filter->type = type;
    filter->enabled = enabled;
    filter->rate = rate;
    filter->frequency = frequency;
    filter->gain = gain;
    filter->quality = quality;
    if (enabled) {
        parametric_eq_design(type, rate, frequency, gain, quality, filter->target);
    } else {
        memset(filter->target, 0, sizeof(filter->target));
        filter->target[0] = 1.0;
    }
    if (!filter->initialized) {
        memcpy(filter->current, filter->target, sizeof(filter->current));
        filter->initialized = 1;
        return;
    }
    filter->remaining = rate / 200;
    if (filter->remaining < 1) filter->remaining = 1;
    for (int coefficient = 0; coefficient < 5; coefficient++) {
        filter->step[coefficient] =
                (filter->target[coefficient] - filter->current[coefficient]) / filter->remaining;
    }
}

void parametric_eq_process(ParametricEq *filter, float *left, float *right) {
    if (!filter->enabled && filter->remaining == 0) return;
    if (filter->remaining > 0) {
        for (int coefficient = 0; coefficient < 5; coefficient++)
            filter->current[coefficient] += filter->step[coefficient];
        if (--filter->remaining == 0)
            memcpy(filter->current, filter->target, sizeof(filter->current));
    }
    if (!filter->enabled && filter->remaining == 0) {
        memset(filter->state, 0, sizeof(filter->state));
        return;
    }
    double inputs[2] = {*left, *right};
    double outputs[2];
    for (int channel = 0; channel < 2; channel++) {
        outputs[channel] = filter->current[0] * inputs[channel] + filter->state[channel][0];
        filter->state[channel][0] = filter->current[1] * inputs[channel] -
                filter->current[3] * outputs[channel] + filter->state[channel][1];
        filter->state[channel][1] = filter->current[2] * inputs[channel] -
                filter->current[4] * outputs[channel];
    }
    *left = (float)outputs[0];
    *right = (float)outputs[1];
}
