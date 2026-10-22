#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum { HamType, HamRead, HamMove, HamEdit } HamEpisode;
typedef enum { HamNone, HamKeyDown, HamKeyUp, HamMouseMove, HamClickDown, HamClickUp } HamEventKind;
typedef struct {
    HamEventKind kind;
    char character; /* '\b' means backspace; all other characters are printable ASCII. */
    int8_t dx, dy;
} HamEvent;
typedef struct {
    /* duration_seconds == 0 means no time limit. */
    unsigned activity, radius, duration_seconds, wpm_min, wpm_max, click_percent;
    uint32_t seed; /* 0: device chooses a fresh random seed. */
} HamProfile;
typedef struct {
    HamProfile profile;
    uint32_t random, due, last_tick, move_start, move_duration, read_start;
    uint64_t elapsed;
    HamEpisode episode;
    bool running, complete, key_down, click_down, known_text, read_move, correcting;
    unsigned wpm, count, index, text_pos, recent_count, hesitations, words;
    char burst[512], recent[5], held;
    const char* text;
    size_t text_length;
    float x, y, p0x, p0y, p1x, p1y, p2x, p2y, p3x, p3y, final_x, final_y;
    unsigned move_stage, correction_stage, moves_remaining;
    uint32_t next_key;
    uint32_t typed, moves, clicks, episodes;
} HamEngine;

HamProfile ham_defaults(void);
void ham_init(HamEngine* e, HamProfile profile, const char* text, uint32_t seed);
void ham_start(HamEngine* e, uint32_t now);
void ham_pause(HamEngine* e);
void ham_activity(HamEngine* e, unsigned activity, uint32_t now);
unsigned ham_duration_step(unsigned seconds, int direction);
HamEvent ham_tick(HamEngine* e, uint32_t now);
const char* ham_episode_name(const HamEngine* e);
