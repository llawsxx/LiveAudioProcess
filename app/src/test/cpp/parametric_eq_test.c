#include "parametric_eq.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static double response_db(const double coefficients[5], double frequency, int rate) {
    double omega = 6.28318530717958647692 * frequency / rate;
    double numerator_real = coefficients[0] + coefficients[1] * cos(omega) + coefficients[2] * cos(2.0 * omega);
    double numerator_imaginary = -coefficients[1] * sin(omega) - coefficients[2] * sin(2.0 * omega);
    double denominator_real = 1.0 + coefficients[3] * cos(omega) + coefficients[4] * cos(2.0 * omega);
    double denominator_imaginary = -coefficients[3] * sin(omega) - coefficients[4] * sin(2.0 * omega);
    double numerator_power = numerator_real * numerator_real + numerator_imaginary * numerator_imaginary;
    double denominator_power = denominator_real * denominator_real + denominator_imaginary * denominator_imaginary;
    return 10.0 * log10(fmax(numerator_power, 1e-30) / fmax(denominator_power, 1e-30));
}

static void filter_shapes(void) {
    double coefficients[5];
    parametric_eq_design(EQ_BELL, 48000, 1000.f, 6.f, .70710677f, coefficients);
    assert(fabs(response_db(coefficients, 1000.0, 48000) - 6.0) < 1e-6);
    parametric_eq_design(EQ_BELL, 48000, 1000.f, 0.f, 20.f, coefficients);
    assert(fabs(response_db(coefficients, 12345.0, 48000)) < 1e-6);
    parametric_eq_design(EQ_HIGH_PASS, 48000, 1000.f, 0.f, .70710677f, coefficients);
    assert(response_db(coefficients, 0.0, 48000) < -100.0);
    assert(fabs(response_db(coefficients, 1000.0, 48000) + 3.0103) < 1e-4);
    assert(fabs(response_db(coefficients, 24000.0, 48000)) < 1e-6);
    parametric_eq_design(EQ_LOW_PASS, 48000, 1000.f, 0.f, .70710677f, coefficients);
    assert(fabs(response_db(coefficients, 0.0, 48000)) < 1e-6);
    assert(fabs(response_db(coefficients, 1000.0, 48000) + 3.0103) < 1e-4);
    assert(response_db(coefficients, 24000.0, 48000) < -100.0);
    parametric_eq_design(EQ_NOTCH, 48000, 1000.f, 0.f, 4.f, coefficients);
    assert(response_db(coefficients, 1000.0, 48000) < -100.0);
    assert(fabs(response_db(coefficients, 0.0, 48000)) < 1e-6);
    parametric_eq_design(EQ_BAND_PASS, 48000, 1000.f, 0.f, 4.f, coefficients);
    assert(fabs(response_db(coefficients, 1000.0, 48000)) < 1e-6);
    assert(response_db(coefficients, 0.0, 48000) < -100.0);
    for (int type = EQ_LOW_SHELF; type <= EQ_HIGH_SHELF; type++) {
        parametric_eq_design(type, 48000, 1000.f, 6.f, .70710677f, coefficients);
        assert(fabs(response_db(coefficients, 0.0, 48000) - (type == EQ_LOW_SHELF ? 6.0 : 0.0)) < 1e-6);
        assert(fabs(response_db(coefficients, 24000.0, 48000) - (type == EQ_HIGH_SHELF ? 6.0 : 0.0)) < 1e-6);
    }
}

static void stable_at_extremes(void) {
    const int rates[] = {8000, 16000, 44100, 48000, 96000, 192000, 384000};
    const float frequencies[] = {20.f, 1000.f, 20000.f};
    const float qualities[] = {.1f, .70710677f, 20.f};
    const float gains[] = {-24.f, 0.f, 24.f};
    for (unsigned rate_index = 0; rate_index < sizeof(rates) / sizeof(rates[0]); rate_index++) {
        for (int type = EQ_BELL; type <= EQ_HIGH_SHELF; type++) {
            for (int frequency = 0; frequency < 3; frequency++) {
                for (int quality = 0; quality < 3; quality++) {
                    for (int gain = 0; gain < 3; gain++) {
                        double coefficients[5];
                        parametric_eq_design(type, rates[rate_index], frequencies[frequency], gains[gain], qualities[quality], coefficients);
                        for (int coefficient = 0; coefficient < 5; coefficient++) assert(isfinite(coefficients[coefficient]));
                        assert(fabs(coefficients[4]) < 1.0);
                        assert(1.0 + coefficients[3] + coefficients[4] > 0.0);
                        assert(1.0 - coefficients[3] + coefficients[4] > 0.0);
                    }
                }
            }
        }
    }
}

static void actual_audio_matches_response(void) {
    for (int type = EQ_BELL; type <= EQ_HIGH_SHELF; type++) {
        ParametricEq filter = {0};
        parametric_eq_prepare(&filter, type, 1, 48000, 1000.f, 6.f, .70710677f);
        double input_power = 0.0, output_power = 0.0;
        for (int frame = 0; frame < 48000; frame++) {
            float input = (float)(0.1 * sin(6.28318530717958647692 * frame * 1500.0 / 48000.0));
            float left = input, right = 0.f;
            parametric_eq_process(&filter, &left, &right);
            assert(right == 0.f);
            assert(isfinite(left));
            if (frame >= 24000) {
                input_power += input * input;
                output_power += left * left;
            }
        }
        double measured = 10.0 * log10(output_power / input_power);
        assert(fabs(measured - response_db(filter.current, 1500.0, 48000)) < 1e-4);
    }
}

static void updates_and_bypass(void) {
    ParametricEq filter = {0};
    parametric_eq_prepare(&filter, EQ_BELL, 1, 48000, 1000.f, 6.f, 1.f);
    double previous = filter.current[0];
    parametric_eq_prepare(&filter, EQ_HIGH_PASS, 1, 48000, 100.f, 0.f, .70710677f);
    assert(filter.remaining == 240);
    assert(filter.current[0] == previous);
    float left = .1f, right = -.1f;
    parametric_eq_process(&filter, &left, &right);
    assert(filter.remaining == 239);
    parametric_eq_prepare(&filter, EQ_HIGH_PASS, 1, 48000, 100.f, 0.f, .70710677f);
    assert(filter.remaining == 239);
    parametric_eq_prepare(&filter, EQ_HIGH_PASS, 0, 48000, 100.f, 0.f, .70710677f);
    for (int frame = 0; frame < 240; frame++) {
        left = .1f;
        right = -.1f;
        parametric_eq_process(&filter, &left, &right);
        assert(isfinite(left) && isfinite(right));
    }
    assert(filter.remaining == 0);
    assert(left == .1f && right == -.1f);
    assert(filter.state[0][0] == 0.0 && filter.state[1][0] == 0.0);
    parametric_eq_prepare(&filter, EQ_BELL, 1, 96000, 1000.f, 6.f, 1.f);
    assert(filter.remaining == 480);
    for (int frame = 0; frame < 480; frame++) {
        left = right = 0.f;
        parametric_eq_process(&filter, &left, &right);
    }
    for (int coefficient = 0; coefficient < 5; coefficient++) assert(filter.current[coefficient] == filter.target[coefficient]);
}

static void dynamic_banks(void) {
    assert(parametric_eq_bank_create(SIZE_MAX) == NULL);
    const size_t counts[] = {0, 1, 5, 12, 64, 257};
    for (size_t test = 0; test < sizeof(counts) / sizeof(counts[0]); test++) {
        ParametricEqBank *bank = parametric_eq_bank_create(counts[test]);
        assert(bank && bank->count == counts[test]);
        for (size_t index = 0; index < bank->count; index++) {
            float values[] = {1000.f, 12.f / (float)bank->count, .70710677f, EQ_BELL, 1.f};
            memcpy(bank->bands[index].values, values, sizeof(values));
        }
        parametric_eq_bank_prepare(bank, 48000, 1);
        double energy = 0.0;
        for (int frame = 0; frame < 24000; frame++) {
            float left = .01f * (float)sin(6.28318530717958647692 * 1000.0 * frame / 48000);
            float right = 0.f;
            parametric_eq_bank_process(bank, &left, &right);
            assert(isfinite(left) && right == 0.f);
            if (frame >= 12000) energy += left * left;
        }
        double expected = bank->count == 0 ? 0.0 : 12.0;
        double measured = 20.0 * log10(sqrt(energy / 12000.0) / (.01 / sqrt(2.0)));
        assert(fabs(measured - expected) < .02);
        parametric_eq_bank_prepare(bank, 48000, 0);
        float left = .25f, right = -.5f;
        parametric_eq_bank_process(bank, &left, &right);
        assert(left == .25f && right == -.5f);
        free(bank);
    }

    ParametricEqBank *original = parametric_eq_bank_create(9);
    assert(original);
    for (size_t index = 0; index < original->count; index++) {
        float values[] = {100.f * (index + 1), 3.f, 1.f, EQ_BELL, 1.f};
        memcpy(original->bands[index].values, values, sizeof(values));
    }
    parametric_eq_bank_prepare(original, 48000, 1);
    float left = .1f, right = -.2f;
    parametric_eq_bank_process(original, &left, &right);
    ParametricEqBank *reduced = parametric_eq_bank_create(8);
    assert(reduced);
    for (size_t index = 0; index < reduced->count; index++) {
        size_t source = index < 3 ? index : index + 1;
        memcpy(reduced->bands[index].values, original->bands[source].values, sizeof(reduced->bands[index].values));
    }
    parametric_eq_bank_inherit(reduced, original);
    for (size_t index = 0; index < reduced->count; index++) {
        size_t source = index < 3 ? index : index + 1;
        assert(memcmp(&reduced->bands[index].filter, &original->bands[source].filter, sizeof(ParametricEq)) == 0);
    }
    ParametricEqBank *expanded = parametric_eq_bank_create(9);
    assert(expanded);
    for (size_t index = 0; index < reduced->count; index++) {
        size_t destination = index < 3 ? index : index + 1;
        memcpy(expanded->bands[destination].values, reduced->bands[index].values, sizeof(reduced->bands[index].values));
    }
    parametric_eq_bank_inherit(expanded, reduced);
    assert(!expanded->bands[3].filter.initialized);
    for (size_t index = 0; index < reduced->count; index++) {
        size_t destination = index < 3 ? index : index + 1;
        assert(memcmp(&expanded->bands[destination].filter, &reduced->bands[index].filter, sizeof(ParametricEq)) == 0);
    }
    expanded->bands[0].values[1] = 6.f;
    parametric_eq_bank_inherit(expanded, original);
    assert(memcmp(&expanded->bands[0].filter, &original->bands[0].filter, sizeof(ParametricEq)) == 0);
    free(expanded);
    free(reduced);
    free(original);
}

int main(void) {
    dynamic_banks();
    filter_shapes();
    stable_at_extremes();
    actual_audio_matches_response();
    updates_and_bypass();
    puts("parametric_eq: all checks passed");
    return 0;
}
