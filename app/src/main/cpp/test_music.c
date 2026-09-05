#include "test_music.h"

#include <math.h>

typedef struct {
    uint8_t midi;
    uint8_t eighths;
} TestMusicNote;

typedef struct {
    const TestMusicNote *notes;
    size_t count;
    int bpm;
} TestMusicTrack;

#define NOTE(midi, eighths) {(midi), (eighths)}
#define REST(eighths) {0, (eighths)}
#define ARRAY_COUNT(values) (sizeof(values) / sizeof((values)[0]))

/* Beethoven: Ode to Joy. */
static const TestMusicNote kOdeToJoy[] = {
    NOTE(64,2),NOTE(64,2),NOTE(65,2),NOTE(67,2), NOTE(67,2),NOTE(65,2),NOTE(64,2),NOTE(62,2),
    NOTE(60,2),NOTE(60,2),NOTE(62,2),NOTE(64,2), NOTE(64,3),NOTE(62,1),NOTE(62,4),
    NOTE(64,2),NOTE(64,2),NOTE(65,2),NOTE(67,2), NOTE(67,2),NOTE(65,2),NOTE(64,2),NOTE(62,2),
    NOTE(60,2),NOTE(60,2),NOTE(62,2),NOTE(64,2), NOTE(62,3),NOTE(60,1),NOTE(60,4),REST(2)
};

/* Beethoven: Fur Elise, opening theme. */
static const TestMusicNote kFurElise[] = {
    NOTE(76,1),NOTE(75,1),NOTE(76,1),NOTE(75,1),NOTE(76,1),NOTE(71,1),NOTE(74,1),NOTE(72,1),NOTE(69,2),
    REST(1),NOTE(60,1),NOTE(64,1),NOTE(69,1),NOTE(71,2),REST(1),NOTE(64,1),NOTE(68,1),NOTE(71,1),NOTE(72,2),
    REST(1),NOTE(64,1),NOTE(76,1),NOTE(75,1),NOTE(76,1),NOTE(75,1),NOTE(76,1),NOTE(71,1),NOTE(74,1),NOTE(72,1),NOTE(69,2),
    REST(1),NOTE(60,1),NOTE(64,1),NOTE(69,1),NOTE(71,2),REST(1),NOTE(64,1),NOTE(72,1),NOTE(71,1),NOTE(69,4),REST(2)
};

/* Mozart: Rondo Alla Turca, compact test arrangement. */
static const TestMusicNote kTurkishMarch[] = {
    NOTE(71,1),NOTE(69,1),NOTE(68,1),NOTE(69,1),NOTE(72,2),NOTE(74,1),NOTE(72,1),NOTE(71,1),NOTE(72,1),NOTE(76,2),
    NOTE(77,1),NOTE(76,1),NOTE(75,1),NOTE(76,1),NOTE(83,1),NOTE(81,1),NOTE(80,1),NOTE(81,1),NOTE(83,1),NOTE(81,1),NOTE(80,1),NOTE(81,1),NOTE(84,2),
    NOTE(81,1),NOTE(84,1),NOTE(88,2),NOTE(86,1),NOTE(84,1),NOTE(83,1),NOTE(81,1),NOTE(80,1),NOTE(78,1),NOTE(76,4),REST(2)
};

/* Pachelbel: Canon in D, bass progression and upper-line excerpt. */
static const TestMusicNote kCanonInD[] = {
    NOTE(62,2),NOTE(57,2),NOTE(59,2),NOTE(54,2),NOTE(55,2),NOTE(50,2),NOTE(55,2),NOTE(57,2),
    NOTE(66,1),NOTE(64,1),NOTE(62,1),NOTE(61,1),NOTE(59,1),NOTE(57,1),NOTE(59,1),NOTE(61,1),
    NOTE(62,1),NOTE(61,1),NOTE(62,1),NOTE(57,1),NOTE(59,1),NOTE(62,1),NOTE(64,1),NOTE(66,1),
    NOTE(67,2),NOTE(66,1),NOTE(64,1),NOTE(62,2),NOTE(61,2),NOTE(62,4),REST(2)
};

/* Traditional French theme used by Mozart for the Twinkle variations. */
static const TestMusicNote kTwinkle[] = {
    NOTE(60,2),NOTE(60,2),NOTE(67,2),NOTE(67,2),NOTE(69,2),NOTE(69,2),NOTE(67,4),
    NOTE(65,2),NOTE(65,2),NOTE(64,2),NOTE(64,2),NOTE(62,2),NOTE(62,2),NOTE(60,4),
    NOTE(67,2),NOTE(67,2),NOTE(65,2),NOTE(65,2),NOTE(64,2),NOTE(64,2),NOTE(62,4),
    NOTE(67,2),NOTE(67,2),NOTE(65,2),NOTE(65,2),NOTE(64,2),NOTE(64,2),NOTE(62,4),
    NOTE(60,2),NOTE(60,2),NOTE(67,2),NOTE(67,2),NOTE(69,2),NOTE(69,2),NOTE(67,4),
    NOTE(65,2),NOTE(65,2),NOTE(64,2),NOTE(64,2),NOTE(62,2),NOTE(62,2),NOTE(60,4),REST(2)
};

/* James Lord Pierpont: Jingle Bells. */
static const TestMusicNote kJingleBells[] = {
    NOTE(64,2),NOTE(64,2),NOTE(64,4), NOTE(64,2),NOTE(64,2),NOTE(64,4),
    NOTE(64,2),NOTE(67,2),NOTE(60,3),NOTE(62,1),NOTE(64,8),
    NOTE(65,2),NOTE(65,2),NOTE(65,3),NOTE(65,1),NOTE(65,2),NOTE(64,2),NOTE(64,2),NOTE(64,1),NOTE(64,1),
    NOTE(64,2),NOTE(62,2),NOTE(62,2),NOTE(64,2),NOTE(62,4),NOTE(67,4),REST(2)
};

static const TestMusicTrack kTracks[] = {
    {kOdeToJoy, ARRAY_COUNT(kOdeToJoy), 112},
    {kFurElise, ARRAY_COUNT(kFurElise), 126},
    {kTurkishMarch, ARRAY_COUNT(kTurkishMarch), 132},
    {kCanonInD, ARRAY_COUNT(kCanonInD), 96},
    {kTwinkle, ARRAY_COUNT(kTwinkle), 108},
    {kJingleBells, ARRAY_COUNT(kJingleBells), 120}
};

int test_music_track_count(void) {
    return (int)ARRAY_COUNT(kTracks);
}

void test_music_reset(TestMusicState *state) {
    if (!state) return;
    state->track = -1;
    state->note_index = 0;
    state->note_frame = 0;
    state->phase = 0.0;
}

float test_music_render(TestMusicState *state, int track, int sample_rate) {
    if (!state || sample_rate <= 0) return 0.0f;
    int count = test_music_track_count();
    if (track < 0 || track >= count) track = 0;
    if (state->track != track) {
        test_music_reset(state);
        state->track = track;
    }

    const TestMusicTrack *music = &kTracks[track];
    const double eighth_frames = (double)sample_rate * 30.0 / (double)music->bpm;
    const TestMusicNote *note = &music->notes[state->note_index];
    uint64_t duration = (uint64_t)fmax(1.0, eighth_frames * (double)note->eighths);
    while (state->note_frame >= duration) {
        state->note_frame -= duration;
        state->note_index = (state->note_index + 1) % music->count;
        state->phase = 0.0;
        note = &music->notes[state->note_index];
        duration = (uint64_t)fmax(1.0, eighth_frames * (double)note->eighths);
    }

    double value = 0.0;
    if (note->midi != 0) {
        double frequency = 440.0 * pow(2.0, ((double)note->midi - 69.0) / 12.0);
        double angle = state->phase * 6.283185307179586;
        double attack_frames = fmax(1.0, (double)sample_rate * 0.008);
        double release_frames = fmax(1.0, fmin((double)duration * 0.22, (double)sample_rate * 0.035));
        double attack = fmin(1.0, ((double)state->note_frame + 1.0) / attack_frames);
        double release = fmin(1.0, (double)(duration - state->note_frame) / release_frames);
        double envelope = fmin(attack, release);
        value = (0.82 * sin(angle) + 0.13 * sin(angle * 2.0) + 0.05 * sin(angle * 3.0)) * envelope;
        state->phase += frequency / (double)sample_rate;
        state->phase -= floor(state->phase);
    } else {
        state->phase = 0.0;
    }
    state->note_frame++;
    return (float)value;
}

