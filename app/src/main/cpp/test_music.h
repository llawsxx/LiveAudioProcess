#ifndef LIVE_AUDIO_PROCESS_TEST_MUSIC_H
#define LIVE_AUDIO_PROCESS_TEST_MUSIC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int track;
    size_t note_index;
    uint64_t note_frame;
    double phase;
} TestMusicState;

void test_music_reset(TestMusicState *state);
float test_music_render(TestMusicState *state, int track, int sample_rate);
int test_music_track_count(void);

#ifdef __cplusplus
}
#endif

#endif
