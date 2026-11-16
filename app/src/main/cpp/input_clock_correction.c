#include "input_clock_correction.h"

#include <math.h>
#include <string.h>

static double clock_clamp(double value, double minimum, double maximum) {
    return value < minimum ? minimum : (value > maximum ? maximum : value);
}

void input_clock_correction_reset(InputClockCorrection *state) {
    memset(state, 0, sizeof(*state));
    state->ratio = 1.0;
}

static int clock_history_add(InputClockHistory *history, uint64_t frames, uint64_t time_ns) {
    if (history->count > 0) {
        int previous = (history->next + INPUT_CLOCK_HISTORY - 1) % INPUT_CLOCK_HISTORY;
        if (time_ns == history->times_ns[previous] && frames == history->frames[previous]) return 0;
        if (time_ns <= history->times_ns[previous] || frames < history->frames[previous]) return -1;
    }
    history->frames[history->next] = frames;
    history->times_ns[history->next] = time_ns;
    history->next = (history->next + 1) % INPUT_CLOCK_HISTORY;
    if (history->count < INPUT_CLOCK_HISTORY) history->count++;
    return 1;
}

static double clock_history_rate(const InputClockHistory *history, double *span) {
    if (history->count < 8) return 0.0;
    int first = (history->next + INPUT_CLOCK_HISTORY - history->count) % INPUT_CLOCK_HISTORY;
    int last = (history->next + INPUT_CLOCK_HISTORY - 1) % INPUT_CLOCK_HISTORY;
    *span = (double)(history->times_ns[last] - history->times_ns[first]) * 1e-9;
    if (*span < 5.0) return 0.0;
    double sum_time = 0.0, sum_frames = 0.0, sum_squared_time = 0.0, sum_product = 0.0;
    for (int offset = 0; offset < history->count; offset++) {
        int index = (first + offset) % INPUT_CLOCK_HISTORY;
        double seconds = (double)(history->times_ns[index] - history->times_ns[first]) * 1e-9;
        double frames = (double)(history->frames[index] - history->frames[first]);
        sum_time += seconds;
        sum_frames += frames;
        sum_squared_time += seconds * seconds;
        sum_product += seconds * frames;
    }
    double denominator = history->count * sum_squared_time - sum_time * sum_time;
    return denominator > 0.0
            ? (history->count * sum_product - sum_time * sum_frames) / denominator : 0.0;
}

int input_clock_correction_update(InputClockCorrection *state,
        uint64_t input_frames, uint64_t input_time_ns,
        uint64_t output_frames, uint64_t output_time_ns,
        int input_rate, int output_rate, double queued_frames,
        double target_frames, uint64_t now_ns, int enabled) {
    if (input_rate <= 0 || output_rate <= 0 || input_time_ns == 0 || output_time_ns == 0 ||
            now_ns < input_time_ns || now_ns < output_time_ns ||
            now_ns - input_time_ns > 2000000000ull || now_ns - output_time_ns > 2000000000ull ||
            (state->last_update_ns && (now_ns <= state->last_update_ns ||
                now_ns - state->last_update_ns > 2000000000ull))) {
        input_clock_correction_reset(state);
        return 0;
    }
    double elapsed = state->last_update_ns ? (double)(now_ns - state->last_update_ns) * 1e-9 : 0.25;
    state->last_update_ns = now_ns;
    int input_added = clock_history_add(&state->input, input_frames, input_time_ns);
    int output_added = clock_history_add(&state->output, output_frames, output_time_ns);
    if (input_added < 0 || output_added < 0) {
        input_clock_correction_reset(state);
        clock_history_add(&state->input, input_frames, input_time_ns);
        clock_history_add(&state->output, output_frames, output_time_ns);
        state->last_update_ns = now_ns;
        return 0;
    }
    double queue_alpha = 1.0 - exp(-elapsed / 2.0);
    if (!state->queue_initialized) {
        state->queue_average = queued_frames;
        state->queue_initialized = 1;
    } else {
        state->queue_average += queue_alpha * (queued_frames - state->queue_average);
    }
    double input_span = 0.0, output_span = 0.0;
    double measured_input = clock_history_rate(&state->input, &input_span);
    double measured_output = clock_history_rate(&state->output, &output_span);
    state->span_seconds = fmin(input_span, output_span);
    if (measured_input <= 0.0 || measured_output <= 0.0) {
        state->ratio = 1.0;
        state->ready = 0;
        return 0;
    }
    double relative_rate = measured_input / measured_output * output_rate;
    double relative_ratio = relative_rate / input_rate;
    if (relative_ratio < 0.98 || relative_ratio > 1.02 ||
            measured_input < input_rate * 0.95 || measured_input > input_rate * 1.05 ||
            measured_output < output_rate * 0.95 || measured_output > output_rate * 1.05) {
        input_clock_correction_reset(state);
        return 0;
    }
    if (!state->ready) state->estimated_input_rate = relative_rate;
    else if (input_added || output_added) {
        double rate_alpha = 1.0 - exp(-elapsed / 8.0);
        state->estimated_input_rate += rate_alpha * (relative_rate - state->estimated_input_rate);
    }
    state->ready = 1;
    state->drift_ppm = (state->estimated_input_rate / input_rate - 1.0) * 1e6;
    double queue_trim_ppm = clock_clamp((state->queue_average - target_frames) / input_rate * 20000.0, -1000.0, 1000.0);
    double desired_ratio = 1.0 + clock_clamp(state->drift_ppm + queue_trim_ppm, -5000.0, 5000.0) * 1e-6;
    double maximum_step = elapsed * 500e-6;
    state->ratio = enabled ? state->ratio + clock_clamp(desired_ratio - state->ratio, -maximum_step, maximum_step) : 1.0;
    return 1;
}
