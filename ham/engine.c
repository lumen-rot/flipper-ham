#include "engine.h"
#include <math.h>
#include <string.h>

/* All scheduling is relative to a monotonic clock. No sleeps or platform dependencies. */
static uint32_t rng(HamEngine* e) {
    uint32_t x = e->random;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return e->random = x;
}
static float unit(HamEngine* e) { return (rng(e) >> 8) / 16777216.0f; }
static unsigned between(HamEngine* e, unsigned lo, unsigned hi) {
    return lo + (unsigned)(unit(e) * (hi - lo + 1));
}
static bool chance(HamEngine* e, unsigned percent) { return unit(e) * 100 < percent; }
static bool ready(uint32_t now, uint32_t deadline) { return (int32_t)(now - deadline) >= 0; }
static float pause_scale(unsigned a) { return powf(3.0f, 1.0f - 2.0f * a / 100.0f); }
static uint32_t skew(HamEngine* e, unsigned lo, unsigned hi) {
    return lo + (uint32_t)((hi - lo) * unit(e) * unit(e));
}

HamProfile ham_defaults(void) {
    return (HamProfile){.activity=50, .radius=600, .duration_seconds=600,
                        .wpm_min=100, .wpm_max=130, .click_percent=25, .seed=0};
}
void ham_init(HamEngine* e, HamProfile profile, const char* text, uint32_t seed) {
    memset(e, 0, sizeof(*e));
    e->profile = profile;
    e->random = seed ? seed : 0x9e3779b9;
    e->text = text;
    e->text_length = strlen(text);
    e->wpm = between(e, profile.wpm_min, profile.wpm_max);
    e->episode = HamRead;
}
void ham_start(HamEngine* e, uint32_t now) {
    if(!e->profile.activity || !e->text_length || e->complete) return;
    e->running = true;
    e->last_tick = now;
    e->episode = HamRead;
    e->read_start = now;
    e->due = now + 1500; /* A short settling interval after explicit start. */
    e->read_move = false;
    e->moves_remaining = 0;
}
void ham_pause(HamEngine* e) {
    e->running = false;
    e->key_down = e->click_down = false;
    e->known_text = false; /* User may reposition the caret while paused. */
    e->recent_count = 0;
}
void ham_activity(HamEngine* e, unsigned activity, uint32_t now) {
    if(activity > 100) activity = 100;
    unsigned old = e->profile.activity;
    if(e->running && e->episode == HamRead && !ready(now, e->due)) {
        e->due = now + (uint32_t)((e->due - now) * pause_scale(activity) / pause_scale(old));
    }
    e->profile.activity = activity;
    if(!activity) ham_pause(e);
}

unsigned ham_duration_step(unsigned seconds, int direction) {
    static const unsigned presets[] = {300, 600, 1800, 3600, 7200};
    if(direction > 0) {
        if(!seconds) return 0;
        for(unsigned i = 0; i < 5; ++i) if(presets[i] > seconds) return presets[i];
        return 0;
    }
    if(!seconds) return 7200;
    for(unsigned i = 5; i > 0; --i) if(presets[i - 1] < seconds) return presets[i - 1];
    return 300;
}

static void read_begin(HamEngine* e, uint32_t now, bool after_typing) {
    unsigned p = between(e, 0, 99);
    unsigned quick = after_typing && e->words >= 20 ? 55 : 80;
    uint32_t duration = p < quick ? between(e, 3000, 12000) : between(e, 15000, 35000);
    e->episode = HamRead;
    e->read_start = now;
    e->due = now + (uint32_t)(duration * pause_scale(e->profile.activity));
    e->read_move = chance(e, 20);
}
static void type_begin(HamEngine* e, uint32_t now, bool edit) {
    e->episode = edit ? HamEdit : HamType;
    e->count = e->index = e->hesitations = e->words = 0;
    if(edit && e->known_text && e->recent_count) {
        unsigned n = between(e, 1, e->recent_count);
        for(unsigned i = 0; i < n; ++i) e->burst[e->count++] = '\b';
        for(unsigned i = e->recent_count - n; i < e->recent_count; ++i)
            e->burst[e->count++] = e->recent[i];
        e->due = now + between(e, 400, 1200);
        return;
    }
    e->episode = HamType;
    unsigned band = between(e, 0, 99);
    unsigned target = band < 85 ? between(e, 3, 20) :
                      band < 97 ? between(e, 21, 35) : between(e, 36, 50);
    bool typo = false;
    while(e->count < sizeof(e->burst) - 5 && e->words < target) {
        char c = e->text[e->text_pos++];
        if(e->text_pos >= e->text_length) e->text_pos = 0;
        if(c < 32 || c > 126) c = ' ';
        if(c == ' ') ++e->words;
        if(!typo && c >= 'a' && c <= 'z' && between(e, 0, 999) < 5) {
            /* QWERTY-adjacent substitutions, then correction before continuing. */
            static const char neighbors[] = "snvfrghjokl;nmopwstuvyxcuzx";
            e->burst[e->count++] = neighbors[c - 'a'];
            e->burst[e->count++] = '\b';
            typo = true;
        }
        e->burst[e->count++] = c;
    }
    e->due = now + between(e, 100, 350);
}
static void point(HamEngine* e, float radius, float* x, float* y) {
    float angle = unit(e) * 6.2831853f;
    float distance = sqrtf(unit(e)) * radius;
    *x = cosf(angle) * distance;
    *y = sinf(angle) * distance;
}
static void bound(HamEngine* e, float* x, float* y) {
    float d = sqrtf(*x * *x + *y * *y);
    float r = e->profile.radius;
    if(d > r && d > 0) { *x *= r / d; *y *= r / d; }
}
static void trajectory(HamEngine* e, uint32_t now, float tx, float ty, unsigned min_ms, unsigned max_ms) {
    e->p0x = e->x; e->p0y = e->y;
    e->p3x = tx; e->p3y = ty;
    float dx = tx - e->x, dy = ty - e->y;
    float bend = (unit(e) - .5f) * .65f;
    e->p1x = e->x + dx * .3f - dy * bend;
    e->p1y = e->y + dy * .3f + dx * bend;
    e->p2x = e->x + dx * .75f - dy * bend * .5f;
    e->p2y = e->y + dy * .75f + dx * bend * .5f;
    bound(e, &e->p1x, &e->p1y);
    bound(e, &e->p2x, &e->p2y);
    e->move_start = now;
    e->move_duration = between(e, min_ms, max_ms);
    e->due = now;
}
static void move_leg(HamEngine* e, uint32_t now) {
    e->episode = HamMove;
    e->move_stage = 0;
    unsigned p = between(e, 0, 99);
    float tx, ty;
    if(p < 70) {
        /* Sharp local movements with gentle recentering, rather than an outward walk. */
        point(e, e->profile.radius * .16f, &tx, &ty);
        tx += e->x * .92f; ty += e->y * .92f;
    } else if(p < 95) {
        point(e, e->profile.radius * .55f, &tx, &ty);
    } else {
        /* Occasional broad sweep; no preference for the outer ring. */
        point(e, e->profile.radius * .9f, &tx, &ty);
    }
    bound(e, &tx, &ty);
    e->final_x = tx; e->final_y = ty;
    e->correcting = chance(e, 10);
    if(e->correcting) {
        tx += (unit(e) - .5f) * 8; ty += (unit(e) - .5f) * 8;
        bound(e, &tx, &ty);
    }
    trajectory(e, now, tx, ty, p < 70 ? 70 : p < 95 ? 100 : 130,
               p < 70 ? 150 : p < 95 ? 200 : 240);
    ++e->moves;
}
static void move_begin(HamEngine* e, uint32_t now) {
    unsigned band = between(e, 0, 99);
    unsigned count = band < 15 ? 1 : band < 75 ? between(e, 2, 3) : between(e, 4, 5);
    e->moves_remaining = count - 1;
    move_leg(e, now);
}
static void next(HamEngine* e, uint32_t now) {
    static const unsigned weights[4][4] = {
        {0, 65, 30, 5}, {55, 5, 37, 3}, {50, 50, 0, 0}, {10, 65, 25, 0}};
    HamEpisode prev = e->episode;
    if(prev == HamMove && e->moves_remaining) {
        --e->moves_remaining;
        uint32_t gap = between(e, 60, 220);
        move_leg(e, now + gap);
        e->due = now + gap;
        return;
    }
    unsigned p = between(e, 0, 99), action = 0;
    while(action < 3 && p >= weights[prev][action]) p -= weights[prev][action++];
    if(action == HamEdit && (!e->known_text || !e->recent_count)) action = HamType;
    ++e->episodes;
    /* Slowly change speed around the session's initial pace. */
    if(chance(e, 12)) {
        int speed = (int)e->wpm + (chance(e, 50) ? 1 : -1);
        if(speed >= (int)e->profile.wpm_min && speed <= (int)e->profile.wpm_max) e->wpm = speed;
    }
    if(action == HamRead) read_begin(e, now, prev == HamType);
    else if(action == HamMove) move_begin(e, now);
    else type_begin(e, now, action == HamEdit);
}
static uint32_t key_interval(HamEngine* e, char c) {
    /* Five characters per word; approximately preserve WPM before extra thinking pauses. */
    float base = 12000.0f / e->wpm;
    float variation = .8f + unit(e) * .4f;
    /* Compensate for average rounding to the main loop's 10 ms tick. */
    uint32_t interval = (uint32_t)(base * variation) - 5;
    if(c == '\b') interval += between(e, 150, 600);
    if(c == ' ') {
        if(chance(e, 8)) interval += between(e, 60, 180);
        if(e->hesitations < 1 && chance(e, 2)) { interval += between(e, 250, 700); ++e->hesitations; }
    }
    if((c == '.' || c == ',' || c == '?' || c == '!') && chance(e, 25))
        interval += between(e, 100, 350);
    if(e->index < e->count && e->burst[e->index] == '\b') interval += between(e, 150, 600);
    return interval;
}
HamEvent ham_tick(HamEngine* e, uint32_t now) {
    HamEvent out = {0};
    if(!e->running) return out;
    e->elapsed += now - e->last_tick;
    e->last_tick = now;
    if(e->profile.duration_seconds && e->elapsed >= e->profile.duration_seconds * 1000ULL) {
        e->elapsed = e->profile.duration_seconds * 1000;
        e->complete = true;
        ham_pause(e); /* Platform must release all held inputs when running becomes false. */
        return out;
    }
    if(e->episode == HamRead && e->read_move &&
       now - e->read_start >= (e->due - e->read_start) / 2) {
        /* At most one tiny incidental movement partway through a reading pause. */
        e->read_move = false;
        int dx = (int)between(e, 0, 4) - 2, dy = (int)between(e, 0, 4) - 2;
        float tx = e->x + dx, ty = e->y + dy;
        if(tx * tx + ty * ty <= e->profile.radius * e->profile.radius) {
            out.kind = HamMouseMove; out.dx = dx; out.dy = dy;
            e->x = tx; e->y = ty;
        }
        return out;
    }
    if(!ready(now, e->due)) return out;
    if(e->key_down) {
        e->key_down = false;
        out.kind = HamKeyUp; out.character = e->held;
        e->due = ready(now, e->next_key) ? now + 1 : e->next_key;
        return out;
    }
    if(e->click_down) {
        e->click_down = false;
        out.kind = HamClickUp;
        e->due = now + between(e, 100, 300);
        e->move_stage = 3;
        return out;
    }
    if(e->episode == HamType || e->episode == HamEdit) {
        if(e->index == e->count) { next(e, now); return out; }
        char c = e->burst[e->index++];
        e->held = c; e->key_down = true;
        e->correction_stage = skew(e, 22, 48);
        e->next_key = now + key_interval(e, c);
        e->due = now + e->correction_stage;
        out.kind = HamKeyDown; out.character = c;
        if(c == '\b') { if(e->recent_count) --e->recent_count; }
        else {
            if(e->recent_count == sizeof(e->recent)) {
                memmove(e->recent, e->recent + 1, sizeof(e->recent) - 1);
                --e->recent_count;
            }
            e->recent[e->recent_count++] = c;
            e->known_text = true;
            ++e->typed;
        }
    } else if(e->episode == HamRead) {
        next(e, now);
    } else if(e->move_stage == 0) {
        float t = (float)(now - e->move_start) / e->move_duration;
        if(t > 1) t = 1;
        float s = t * t * (3 - 2 * t), u = 1 - s;
        float x = u*u*u*e->p0x + 3*u*u*s*e->p1x + 3*u*s*s*e->p2x + s*s*s*e->p3x;
        float y = u*u*u*e->p0y + 3*u*u*s*e->p1y + 3*u*s*s*e->p2y + s*s*s*e->p3y;
        float dx_float = x - e->x, dy_float = y - e->y;
        float largest = fmaxf(fabsf(dx_float), fabsf(dy_float));
        /* Scale the whole vector: independently clipping axes distorts fast paths. */
        if(largest > 120) {
            dx_float *= 120 / largest;
            dy_float *= 120 / largest;
        }
        float next_x = e->x + dx_float, next_y = e->y + dy_float;
        bound(e, &next_x, &next_y);
        /* Rounding can exceed the continuous circular bound by <1 input count. */
        int dx = lroundf(next_x) - (int)e->x, dy = lroundf(next_y) - (int)e->y;
        e->x += dx; e->y += dy;
        out.kind = HamMouseMove; out.dx = dx; out.dy = dy;
        e->due = now + between(e, 10, 20);
        if(t >= 1 && fabsf(e->x-e->p3x) <= .5f && fabsf(e->y-e->p3y) <= .5f) {
            if(e->correcting) {
                e->correcting = false;
                trajectory(e, now + 50, e->final_x, e->final_y, 80, 200);
                e->due = now + 50;
            } else {
                e->move_stage = 1;
                e->due = now + between(e, 80, 220);
            }
        }
    } else if(e->move_stage == 1) {
        if(chance(e, e->profile.click_percent)) {
            out.kind = HamClickDown;
            e->click_down = true;
            e->known_text = false; e->recent_count = 0;
            e->due = now + between(e, 60, 140);
            ++e->clicks;
        } else next(e, now);
    } else next(e, now);
    return out;
}
const char* ham_episode_name(const HamEngine* e) {
    if(e->complete) return "Finished";
    if(!e->running) return "Paused";
    static const char* names[] = {"Typing", "Reading", "Moving", "Editing"};
    return names[e->episode];
}
