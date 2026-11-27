#include "harmonic_exciter.h"

#include <assert.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define TEST_RATE 48000
#define TEST_FRAMES 48000

static double amplitude(const float *samples, int frames, double frequency, int rate) {
    double real = 0.0, imaginary = 0.0;
    for (int frame = frames / 2; frame < frames; frame++) {
        double phase = 6.28318530717958647692 * frequency * frame / rate;
        real += samples[frame] * cos(phase);
        imaginary += samples[frame] * sin(phase);
    }
    return 4.0 * sqrt(real * real + imaginary * imaginary) / frames;
}

static void render(const float parameters[EXCITER_PARAMETER_COUNT], double frequency, float *output) {
    HarmonicExciter exciter;
    harmonic_exciter_init(&exciter, TEST_RATE);
    harmonic_exciter_prepare(&exciter, TEST_RATE, 1, parameters);
    for (int frame = 0; frame < TEST_FRAMES; frame++) {
        float left = .3f * (float)sin(6.28318530717958647692 * frequency * frame / TEST_RATE);
        float right = 0.f;
        harmonic_exciter_process(&exciter, &left, &right);
        assert(right == 0.f);
        output[frame] = left;
    }
}

static void bypass_and_gain(void) {
    HarmonicExciter exciter;
    float parameters[] = {3000.f, 12.f, 15.f, 16000.f, 25.f, 0.f};
    harmonic_exciter_init(&exciter, TEST_RATE);
    harmonic_exciter_prepare(&exciter, TEST_RATE, 0, parameters);
    for (int frame = 0; frame < 1000; frame++) {
        float input = (float)sin(frame * .231);
        float left = input, right = -input;
        harmonic_exciter_process(&exciter, &left, &right);
        assert(left == input && right == -input);
    }
    parameters[2] = 0.f;
    harmonic_exciter_prepare(&exciter, TEST_RATE, 1, parameters);
    for (int frame = 0; frame < 1000; frame++) {
        float left = .1234567f, right = -.2345678f;
        harmonic_exciter_process(&exciter, &left, &right);
        assert(left == .1234567f && right == -.2345678f);
    }
    parameters[5] = -6.f;
    harmonic_exciter_prepare(&exciter, TEST_RATE, 1, parameters);
    for (int frame = 0; frame < 1000; frame++) {
        float left = .25f, right = -.5f;
        harmonic_exciter_process(&exciter, &left, &right);
        if (frame >= 240) assert(fabs(left - .25 * pow(10.0, -.3)) < 1e-7);
    }
    harmonic_exciter_prepare(&exciter, TEST_RATE, 0, parameters);
    for (int frame = 0; frame < 1000; frame++) {
        float left = .25f, right = -.5f;
        harmonic_exciter_process(&exciter, &left, &right);
        if (frame >= 240) assert(left == .25f && right == -.5f);
    }
}

static void actual_harmonics(void) {
    float *samples = malloc(TEST_FRAMES * sizeof(float));
    assert(samples);
    float parameters[] = {100.f, 18.f, 100.f, 20000.f, 0.f, 0.f};
    render(parameters, 1000.0, samples);
    double odd_second = amplitude(samples, TEST_FRAMES, 2000.0, TEST_RATE);
    double odd_third = amplitude(samples, TEST_FRAMES, 3000.0, TEST_RATE);
    assert(odd_third > .005);
    assert(odd_second < 1e-6);
    parameters[4] = 100.f;
    render(parameters, 1000.0, samples);
    double even_second = amplitude(samples, TEST_FRAMES, 2000.0, TEST_RATE);
    double even_third = amplitude(samples, TEST_FRAMES, 3000.0, TEST_RATE);
    assert(even_second > .005);
    assert(even_third < 1e-6);
    assert(fabs(amplitude(samples, TEST_FRAMES, 1000.0, TEST_RATE) - .3) < 1e-6);
    double mean = 0.0;
    for (int frame = TEST_FRAMES / 2; frame < TEST_FRAMES; frame++) mean += samples[frame];
    assert(fabs(mean * 2.0 / TEST_FRAMES) < 1e-7);
    printf("harmonics: odd H2=%.8f H3=%.8f, even H2=%.8f H3=%.8f\n",
            odd_second, odd_third, even_second, even_third);
    free(samples);
}

static void anti_aliasing_and_band_selection(void) {
    float *samples = malloc(TEST_FRAMES * sizeof(float));
    float *naive = malloc(TEST_FRAMES * sizeof(float));
    assert(samples && naive);
    float parameters[] = {100.f, 18.f, 100.f, 20000.f, 0.f, 0.f};
    render(parameters, 10000.0, samples);
    double drive = pow(10.0, parameters[1] / 20.0);
    for (int frame = 0; frame < TEST_FRAMES; frame++) {
        double input = .3 * sin(6.28318530717958647692 * 10000.0 * frame / TEST_RATE);
        naive[frame] = (float)(input + input - tanh(drive * input) / drive);
    }
    double filtered_alias = amplitude(samples, TEST_FRAMES, 18000.0, TEST_RATE);
    double naive_alias = amplitude(naive, TEST_FRAMES, 18000.0, TEST_RATE);
    assert(naive_alias > .005);
    assert(filtered_alias < naive_alias * .03);
    parameters[0] = 3000.f;
    parameters[4] = 100.f;
    render(parameters, 100.0, samples);
    assert(amplitude(samples, TEST_FRAMES, 200.0, TEST_RATE) < 1e-6);
    assert(fabs(amplitude(samples, TEST_FRAMES, 100.0, TEST_RATE) - .3) < 1e-5);
    printf("alias: filtered=%.9f naive=%.9f, rejection=%.1f dB\n",
            filtered_alias, naive_alias, 20.0 * log10(filtered_alias / naive_alias));
    free(samples);
    free(naive);
}

static void smoothing_and_reset(void) {
    HarmonicExciter exciter;
    float parameters[] = {100.f, 18.f, 100.f, 20000.f, 25.f, 0.f};
    harmonic_exciter_init(&exciter, TEST_RATE);
    harmonic_exciter_prepare(&exciter, TEST_RATE, 1, parameters);
    assert(exciter.remaining == 240);
    float left = .5f, right = -.5f;
    harmonic_exciter_process(&exciter, &left, &right);
    assert(exciter.remaining == 239);
    harmonic_exciter_prepare(&exciter, TEST_RATE, 1, parameters);
    assert(exciter.remaining == 239);
    for (int frame = 0; frame < 1000; frame++) {
        left = .2f; right = -.3f;
        harmonic_exciter_process(&exciter, &left, &right);
    }
    assert(exciter.processing);
    harmonic_exciter_prepare(&exciter, TEST_RATE, 0, parameters);
    for (int frame = 0; frame < 1000; frame++) {
        left = .2f; right = -.3f;
        harmonic_exciter_process(&exciter, &left, &right);
    }
    assert(!exciter.processing && exciter.remaining == 0);
    assert(left == .2f && right == -.3f);
    for (int channel = 0; channel < 2; channel++) {
        for (int index = 0; index < EXCITER_INPUT_HISTORY; index++) assert(exciter.input_history[channel][index] == 0.0);
        for (int index = 0; index < EXCITER_FIR_TAPS; index++) assert(exciter.output_history[channel][index] == 0.0);
    }
    harmonic_exciter_prepare(&exciter, 96000, 1, parameters);
    assert(exciter.rate == 96000 && exciter.remaining == 480);
}

static void rates_and_extremes(void) {
    const int rates[] = {8000, 16000, 44100, 48000, 96000, 192000, 384000};
    for (size_t rate_index = 0; rate_index < sizeof(rates) / sizeof(rates[0]); rate_index++) {
        for (int setting = 0; setting < 4; setting++) {
            HarmonicExciter exciter;
            float parameters[] = {setting % 2 ? 12000.f : 20.f, setting % 2 ? 24.f : 0.f,
                    100.f, setting % 2 ? 500.f : 20000.f, setting < 2 ? 0.f : 100.f, 6.f};
            harmonic_exciter_init(&exciter, rates[rate_index]);
            harmonic_exciter_prepare(&exciter, rates[rate_index], 1, parameters);
            double coefficient_sum = 0.0;
            for (int tap = 0; tap < EXCITER_FIR_TAPS; tap++) coefficient_sum += exciter.coefficients[tap];
            assert(fabs(coefficient_sum - 1.0) < 1e-12);
            for (int frame = 0; frame < 4096; frame++) {
                float left = (float)(2.0 * sin(frame * .131)), right = (float)cos(frame * .219);
                harmonic_exciter_process(&exciter, &left, &right);
                assert(left > -20.f && left < 20.f && right > -20.f && right < 20.f);
            }
        }
    }
    HarmonicExciter exciter;
    float invalid[] = {NAN, INFINITY, -INFINITY, NAN, NAN, INFINITY};
    harmonic_exciter_init(&exciter, 0);
    harmonic_exciter_prepare(&exciter, 0, 1, invalid);
    assert(exciter.rate == TEST_RATE && exciter.target[0] > 1.0 && exciter.target[1] == .15);
    assert(exciter.target[2] == .25 && exciter.target[3] == 1.0);
}

int main(void) {
    clock_t started = clock();
    bypass_and_gain();
    actual_harmonics();
    anti_aliasing_and_band_selection();
    smoothing_and_reset();
    rates_and_extremes();
    printf("harmonic_exciter: all checks passed (%.3f CPU seconds)\n", (double)(clock() - started) / CLOCKS_PER_SEC);
    return 0;
}
