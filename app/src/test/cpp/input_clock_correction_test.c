#include "input_clock_correction.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>

static void check_close(const char *name, double actual, double expected, double tolerance) {
    if (fabs(actual - expected) > tolerance) {
        fprintf(stderr, "%s: actual=%.9f expected=%.9f tolerance=%.9f\n", name, actual, expected, tolerance);
        assert(0);
    }
}

static void drive(InputClockCorrection *state, double input_actual, double output_actual,
        int input_nominal, int output_nominal, int enabled, double queued, double target) {
    input_clock_correction_reset(state);
    for (int sample = 0; sample <= 400; sample++) {
        double seconds = sample * 0.25;
        uint64_t timestamp = 1000000000ull + (uint64_t)sample * 250000000ull;
        input_clock_correction_update(state,
                1099511627776ull + (uint64_t)llround(input_actual * seconds), timestamp,
                2199023255552ull + (uint64_t)llround(output_actual * seconds), timestamp,
                input_nominal, output_nominal, queued, target, timestamp, enabled);
    }
}

static void test_equal_clocks(void) {
    InputClockCorrection state;
    drive(&state, 48000.0, 48000.0, 48000, 48000, 1, 480, 480);
    assert(state.ready);
    check_close("equal rate", state.estimated_input_rate, 48000.0, 0.001);
    check_close("equal ratio", state.ratio, 1.0, 1e-9);
    assert(state.input.count == 256);
    assert(state.output.count == 256);
    check_close("full history window", state.span_seconds, 63.75, 1e-9);
}

static void test_fast_and_slow_inputs(void) {
    InputClockCorrection state;
    drive(&state, 48048.0, 48000.0, 48000, 48000, 1, 480, 480);
    check_close("fast input estimate", state.estimated_input_rate, 48048.0, 0.001);
    check_close("fast input ratio", state.ratio, 1.001, 1e-8);
    drive(&state, 47976.0, 48000.0, 48000, 48000, 1, 480, 480);
    check_close("slow input estimate", state.estimated_input_rate, 47976.0, 0.001);
    check_close("slow input ratio", state.ratio, 0.9995, 1e-8);
}

static void test_output_is_the_reference(void) {
    InputClockCorrection state;
    drive(&state, 48000.0, 48004.8, 48000, 48000, 1, 480, 480);
    check_close("output referenced rate", state.estimated_input_rate, 48000.0 / 48004.8 * 48000.0, 0.002);
    assert(state.drift_ppm < 0.0);
    drive(&state, 48004.8, 48004.8, 48000, 48000, 1, 480, 480);
    check_close("shared drift cancels", state.estimated_input_rate, 48000.0, 0.001);
}

static void test_different_nominal_rates(void) {
    InputClockCorrection state;
    drive(&state, 44144.1, 96048.0, 44100, 96000, 1, 441, 441);
    double expected = 44144.1 / 96048.0 * 96000.0;
    check_close("different nominal estimate", state.estimated_input_rate, expected, 0.01);
    check_close("different nominal ratio", state.ratio, expected / 44100.0, 1e-6);
}

static void test_warmup_and_slew_limit(void) {
    InputClockCorrection state;
    input_clock_correction_reset(&state);
    for (int sample = 0; sample <= 20; sample++) {
        uint64_t timestamp = 1000000000ull + (uint64_t)sample * 250000000ull;
        double previous_ratio = state.ratio;
        input_clock_correction_update(&state, (uint64_t)sample * 12012u, timestamp,
                (uint64_t)sample * 12000u, timestamp, 48000, 48000, 480, 480, timestamp, 1);
        if (sample < 20) {
            assert(!state.ready);
            check_close("warmup unity", state.ratio, 1.0, 1e-9);
        }
        assert(fabs(state.ratio - previous_ratio) <= 0.000125001);
    }
    assert(state.ready);
    assert(state.ratio > 1.0);
}

static void test_disabled_and_missing_output(void) {
    InputClockCorrection state;
    drive(&state, 48048.0, 48000.0, 48000, 48000, 0, 480, 480);
    assert(state.ready);
    check_close("disabled still measures", state.estimated_input_rate, 48048.0, 0.001);
    check_close("disabled unity", state.ratio, 1.0, 1e-9);
    int ready = input_clock_correction_update(&state, 5000000, 102000000000ull,
            0, 102000000000ull, 48000, 0, 480, 480, 102000000000ull, 1);
    assert(!ready && !state.ready);
    check_close("missing output unity", state.ratio, 1.0, 1e-9);
    check_close("missing output no estimate", state.estimated_input_rate, 0.0, 1e-9);
}

static void test_discontinuities_and_stale_timestamps(void) {
    InputClockCorrection state;
    drive(&state, 48048.0, 48000.0, 48000, 48000, 1, 480, 480);
    int ready = input_clock_correction_update(&state, 0, 101250000000ull,
            0, 101250000000ull, 48000, 48000, 480, 480, 101250000000ull, 1);
    assert(!ready && !state.ready);
    check_close("restart unity", state.ratio, 1.0, 1e-9);
    drive(&state, 48048.0, 48000.0, 48000, 48000, 1, 480, 480);
    ready = input_clock_correction_update(&state, 5000000, 101000000000ull,
            5000000, 101000000000ull, 48000, 48000, 480, 480, 104000000000ull, 1);
    assert(!ready && !state.ready);
    check_close("stale unity", state.ratio, 1.0, 1e-9);
}

static void test_compensation_limit_and_queue_trim(void) {
    InputClockCorrection state;
    drive(&state, 48480.0, 48000.0, 48000, 48000, 1, 480, 480);
    check_close("positive limit", state.ratio, 1.005, 1e-9);
    drive(&state, 47520.0, 48000.0, 48000, 48000, 1, 480, 480);
    check_close("negative limit", state.ratio, 0.995, 1e-9);
    drive(&state, 48000.0, 48000.0, 48000, 48000, 1, 800, 480);
    assert(state.ratio > 1.0);
    check_close("queue trim does not alter measurement", state.estimated_input_rate, 48000.0, 0.001);
    drive(&state, 48000.0, 48000.0, 48000, 48000, 1, 200, 480);
    assert(state.ratio < 1.0);
}

static void test_callback_jitter(void) {
    InputClockCorrection state;
    input_clock_correction_reset(&state);
    for (int sample = 0; sample < 800; sample++) {
        uint64_t timestamp = 1000000000ull + (uint64_t)sample * 250000000ull;
        double seconds = sample * 0.25;
        int input_jitter = (sample * 17 % 7 - 3) * 32;
        int output_jitter = (sample * 13 % 11 - 5) * 24;
        input_clock_correction_update(&state,
                (uint64_t)llround(1000000.0 + 48004.8 * seconds + input_jitter), timestamp,
                (uint64_t)llround(2000000.0 + 48000.0 * seconds + output_jitter), timestamp,
                48000, 48000, 480, 480, timestamp, 1);
    }
    assert(state.ready);
    check_close("jitter filtered estimate", state.estimated_input_rate, 48004.8, 2.0);
}

static void test_long_running_buffer(double drift_ppm) {
    InputClockCorrection state;
    input_clock_correction_reset(&state);
    double queued = 480.0;
    double input_rate = 48000.0 * (1.0 + drift_ppm * 1e-6);
    for (int sample = 0; sample < 3600; sample++) {
        uint64_t timestamp = 1000000000ull + (uint64_t)sample * 250000000ull;
        double seconds = sample * 0.25;
        input_clock_correction_update(&state,
                (uint64_t)llround(input_rate * seconds), timestamp,
                (uint64_t)sample * 12000ull, timestamp,
                48000, 48000, queued, 480, timestamp, 1);
        queued += 0.25 * (input_rate - 48000.0 * state.ratio);
        assert(queued > 0.0 && queued < 960.0);
    }
    check_close("long running buffer target", queued, 480.0, 1.0);
}

int main(void) {
    test_equal_clocks();
    test_fast_and_slow_inputs();
    test_output_is_the_reference();
    test_different_nominal_rates();
    test_warmup_and_slew_limit();
    test_disabled_and_missing_output();
    test_discontinuities_and_stale_timestamps();
    test_compensation_limit_and_queue_trim();
    test_callback_jitter();
    test_long_running_buffer(1000.0);
    test_long_running_buffer(-1000.0);
    puts("Input clock correction: 11 checks passed");
    return 0;
}
