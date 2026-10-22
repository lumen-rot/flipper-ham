#include "engine.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

int main(void) {
    double total_distance = 0, total_ms = 0;
    unsigned moves = 0, selected = 0, short_moves = 0, edge_targets = 0;
    for(unsigned seed = 1; seed <= 100; ++seed) {
        HamProfile p = ham_defaults();
        HamEngine e;
        ham_init(&e, p, "A sentence to type quickly before pausing and moving around. ", seed);
        ham_start(&e, 0);
        bool pending = false;
        uint32_t started = 0;
        for(uint32_t now = 0; now < 600000; now += 10) {
            unsigned previous_moves = e.moves;
            ham_tick(&e, now);
            if(e.moves != previous_moves) {
                float dx = e.final_x - e.p0x, dy = e.final_y - e.p0y;
                total_distance += sqrtf(dx * dx + dy * dy);
                ++selected;
                if(sqrtf(dx * dx + dy * dy) < p.radius * .25f) ++short_moves;
                if(sqrtf(e.final_x * e.final_x + e.final_y * e.final_y) > p.radius * .65f)
                    ++edge_targets;
                started = e.move_start;
                pending = true;
            }
            if(pending && e.episode == HamMove && e.move_stage == 1) {
                total_ms += now - started;
                ++moves;
                pending = false;
            }
        }
    }
    printf("Mouse metrics across 100 sessions: %u completed moves, %.0f counts mean distance, %.0f ms mean duration\n",
           moves, total_distance / moves, total_ms / moves);
    printf("Short moves: %.1f%%; targets beyond 65%% of radius: %.1f%%\n",
           100.0 * short_moves / selected, 100.0 * edge_targets / selected);
#ifdef CHECK_CONTROLLED
    assert(total_distance / moves > 70 && total_distance / moves < 250);
    assert(total_ms / moves < 300);
    assert(short_moves > selected * .65);
    assert(edge_targets < selected * .10);
#endif
    return 0;
}
