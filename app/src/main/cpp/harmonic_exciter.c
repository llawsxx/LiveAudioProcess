#include "harmonic_exciter.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

static int finite_float(float value) {
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return (bits & 0x7f800000u) != 0x7f800000u;
}

static double parameter(float value, double fallback, double minimum, double maximum) {
    double result = finite_float(value) ? value : fallback;
    return result < minimum ? minimum : (result > maximum ? maximum : result);
}

static void clear_signal(HarmonicExciter *exciter) {
    memset(exciter->input_history, 0, sizeof(exciter->input_history));
    memset(exciter->output_history, 0, sizeof(exciter->output_history));
    memset(exciter->input_high_pass.state, 0, sizeof(exciter->input_high_pass.state));
    memset(exciter->output_high_pass.state, 0, sizeof(exciter->output_high_pass.state));
    memset(exciter->output_low_pass.state, 0, sizeof(exciter->output_low_pass.state));
    exciter->input_position = exciter->output_position = 0;
    exciter->processing = 0;
}

void harmonic_exciter_init(HarmonicExciter *exciter, int rate) {
    memset(exciter, 0, sizeof(*exciter));
    exciter->rate = rate > 0 ? rate : 48000;
    exciter->current[0] = exciter->target[0] = 1.0;
    exciter->current[2] = exciter->target[2] = 0.25;
    exciter->current[3] = exciter->target[3] = 1.0;
    double total = 0.0;
    for (int index = 0; index < EXCITER_FIR_TAPS; index++) {
        int offset = index - (EXCITER_FIR_TAPS - 1) / 2;
        double phase = 6.28318530717958647692 * index / (EXCITER_FIR_TAPS - 1);
        double window = 0.42 - 0.5 * cos(phase) + 0.08 * cos(2.0 * phase);
        double sinc = offset == 0 ? 0.2 : sin(0.2 * 3.14159265358979323846 * offset) /
                (3.14159265358979323846 * offset);
        exciter->coefficients[index] = sinc * window;
        total += exciter->coefficients[index];
    }
    for (int index = 0; index < EXCITER_FIR_TAPS; index++)
        exciter->coefficients[index] /= total;
}

void harmonic_exciter_prepare(HarmonicExciter *exciter, int rate, int enabled,
        const float parameters[EXCITER_PARAMETER_COUNT]) {
    if (rate <= 0) rate = 48000;
    if (exciter->rate != rate) harmonic_exciter_init(exciter, rate);
    double frequency = parameter(parameters[0], 3000.0, 20.0, fmin(12000.0, rate * 0.3));
    double drive = parameter(parameters[1], 12.0, 0.0, 24.0);
    double amount = parameter(parameters[2], 15.0, 0.0, 100.0);
    double tone = parameter(parameters[3], 16000.0, fmax(500.0, frequency), rate * 0.45);
    double even = parameter(parameters[4], 25.0, 0.0, 100.0);
    double output = parameter(parameters[5], 0.0, -12.0, 6.0);
    double targets[] = {pow(10.0, drive / 20.0), enabled ? amount / 100.0 : 0.0,
            even / 100.0, enabled ? pow(10.0, output / 20.0) : 1.0};
    if (memcmp(exciter->target, targets, sizeof(targets)) != 0) {
        exciter->remaining = rate / 200;
        if (exciter->remaining < 1) exciter->remaining = 1;
        for (int index = 0; index < 4; index++) {
            exciter->target[index] = targets[index];
            exciter->step[index] = (targets[index] - exciter->current[index]) / exciter->remaining;
        }
    }
    parametric_eq_prepare(&exciter->input_high_pass, EQ_HIGH_PASS, 1, rate, (float)frequency, 0.f, .70710677f);
    parametric_eq_prepare(&exciter->output_high_pass, EQ_HIGH_PASS, 1, rate, (float)frequency, 0.f, .70710677f);
    parametric_eq_prepare(&exciter->output_low_pass, EQ_LOW_PASS, 1, rate, (float)tone, 0.f, .70710677f);
}

void harmonic_exciter_process(HarmonicExciter *exciter, float *left, float *right) {
    if (exciter->remaining > 0) {
        for (int index = 0; index < 4; index++) exciter->current[index] += exciter->step[index];
        if (--exciter->remaining == 0) memcpy(exciter->current, exciter->target, sizeof(exciter->current));
    }
    if (exciter->current[1] == 0.0 && exciter->target[1] == 0.0) {
        if (exciter->processing) clear_signal(exciter);
        *left = (float)(*left * exciter->current[3]);
        *right = (float)(*right * exciter->current[3]);
        return;
    }
    exciter->processing = 1;
    float band_left = finite_float(*left) ? *left : 0.f;
    float band_right = finite_float(*right) ? *right : 0.f;
    parametric_eq_process(&exciter->input_high_pass, &band_left, &band_right);
    exciter->input_history[0][exciter->input_position] = band_left;
    exciter->input_history[1][exciter->input_position] = band_right;
    double wet[2] = {0.0, 0.0};
    double drive = exciter->current[0], even = exciter->current[2];
    for (int phase = 0; phase < EXCITER_OVERSAMPLING; phase++) {
        for (int channel = 0; channel < 2; channel++) {
            double sample = 0.0;
            int position = exciter->input_position;
            for (int tap = phase; tap < EXCITER_FIR_TAPS; tap += EXCITER_OVERSAMPLING) {
                sample += exciter->coefficients[tap] * exciter->input_history[channel][position];
                if (--position < 0) position = EXCITER_INPUT_HISTORY - 1;
            }
            sample *= EXCITER_OVERSAMPLING;
            double saturated = tanh(drive * sample);
            double odd_harmonics = sample - saturated / drive;
            double even_harmonics = saturated * saturated / drive;
            exciter->output_history[channel][exciter->output_position] =
                    (1.0 - even) * odd_harmonics + even * even_harmonics;
            if (phase == 0) {
                position = exciter->output_position;
                for (int tap = 0; tap < EXCITER_FIR_TAPS; tap++) {
                    wet[channel] += exciter->coefficients[tap] * exciter->output_history[channel][position];
                    if (--position < 0) position = EXCITER_FIR_TAPS - 1;
                }
            }
        }
        if (++exciter->output_position == EXCITER_FIR_TAPS) exciter->output_position = 0;
    }
    if (++exciter->input_position == EXCITER_INPUT_HISTORY) exciter->input_position = 0;
    float wet_left = (float)wet[0], wet_right = (float)wet[1];
    parametric_eq_process(&exciter->output_high_pass, &wet_left, &wet_right);
    parametric_eq_process(&exciter->output_low_pass, &wet_left, &wet_right);
    *left = (float)((*left + exciter->current[1] * wet_left) * exciter->current[3]);
    *right = (float)((*right + exciter->current[1] * wet_right) * exciter->current[3]);
}
