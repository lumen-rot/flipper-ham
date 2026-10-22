#include "../ham/engine.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static const char text[] = "This is a sample sentence with several words. Here is another thought to try out. ";
static unsigned burst_bands[3];

static uint64_t run(unsigned activity, unsigned seed, unsigned radius, bool export) {
    HamProfile p = ham_defaults();
    p.activity = activity;
    p.radius = radius;
    HamEngine e;
    ham_init(&e, p, text, seed);
    assert(!e.running);
    assert(ham_tick(&e, 100).kind == HamNone);
    ham_start(&e, 0);
    bool key = false, click = false;
    char held = 0;
    uint32_t key_time = 0, click_time = 0;
    unsigned key_count = 0, clicks = 0;
    uint64_t fast_gaps = 0;
    unsigned fast_count = 0;
    uint32_t previous_press = 0;
    char previous_character = 0;
    uint64_t count = 0;
    float x = 0, y = 0;
    FILE* csv = export ? fopen("tests/session.csv", "w") : NULL;
    if(csv) fprintf(csv, "ms,episode,event,character,x,y\n");
    for(uint32_t now = 0; now <= p.duration_seconds * 1000; now += 10) {
        unsigned before = e.episodes;
        HamEvent ev = ham_tick(&e, now);
        if(e.episodes != before && e.episode == HamType) {
            assert(e.words >= 3 && e.words <= 50);
            ++burst_bands[e.words <= 20 ? 0 : e.words <= 35 ? 1 : 2];
        }
        if(ev.kind == HamKeyDown) {
            assert(!key && !click);
            assert(ev.character == '\b' || (ev.character >= 32 && ev.character <= 126));
            key = true; held = ev.character; key_time = now; ++key_count;
            if(e.index > 1 && previous_character >= 'a' && previous_character <= 'z' &&
               ev.character >= 'a' && ev.character <= 'z') {
                fast_gaps += now - previous_press;
                ++fast_count;
            }
            previous_character = ev.character;
            previous_press = now;
        } else if(ev.kind == HamKeyUp) {
            assert(key && ev.character == held);
            assert(now - key_time >= 22 && now - key_time <= 58);
            key = false;
        } else if(ev.kind == HamClickDown) {
            assert(!key && !click);
            click = true; click_time = now; ++clicks;
        } else if(ev.kind == HamClickUp) {
            assert(click);
            assert(now - click_time >= 60 && now - click_time <= 150);
            click = false;
        } else if(ev.kind == HamMouseMove) {
            assert(!key && !click);
            x += ev.dx; y += ev.dy;
            assert(fabsf(x - e.x) < .01f && fabsf(y - e.y) < .01f);
            assert(sqrtf(x*x+y*y) <= radius + 1.0f);
        }
        if(ev.kind != HamNone) {
            ++count;
            if(csv) fprintf(csv, "%u,%s,%d,%d,%.0f,%.0f\n", now,
                            ham_episode_name(&e), ev.kind, (unsigned char)ev.character, x, y);
        }
        if(!e.running) key = click = false; /* Same release-all contract as the platform. */
    }
    if(csv) fclose(csv);
    assert(e.complete && !e.running);
    assert(e.typed > 0 && e.moves > 0 && e.episodes > 0);
    assert(key_count >= e.typed);
    assert(clicks == e.clicks);
    if(export) printf("Sample 10-minute session: %u keys, %u moves, %u clicks, %u episodes\n",
                      key_count, e.moves, e.clicks, e.episodes);
    if(export && fast_count) printf("Measured within-word pace: %.1f WPM\n",
                                   12000.0 * fast_count / fast_gaps);
    return count;
}

static void controls(void) {
    HamProfile p = ham_defaults();
    HamEngine e;
    ham_init(&e, p, text, 3);
    ham_start(&e, 0);
    for(uint32_t t=0; t<5000; t+=10) ham_tick(&e,t);
    ham_pause(&e);
    assert(!e.running && !e.key_down && !e.click_down);
    uint32_t elapsed = e.elapsed;
    for(uint32_t t=5000; t<15000; t+=10) assert(ham_tick(&e,t).kind == HamNone);
    assert(e.elapsed == elapsed);
    ham_start(&e,15000);
    ham_activity(&e,0,15000);
    assert(!e.running);
    ham_start(&e,16000);
    assert(!e.running);
    ham_activity(&e,50,16000);
    ham_start(&e,16000);
    assert(e.running);
    assert(ham_tick(&e,16000).kind == HamNone);
    assert(e.elapsed == elapsed);
    /* Monotonic tick wraparound must preserve scheduling and session limits. */
    ham_init(&e,p,text,4);
    uint32_t base=UINT32_MAX-1000;
    ham_start(&e,base);
    for(uint32_t t=0;t<=600000;t+=10) ham_tick(&e,base+t);
    assert(e.complete);
    ham_init(&e,p,text,4);
    ham_start(&e,0);
    ham_tick(&e,601999);
    assert(e.complete && e.elapsed == 600000);
    /* Repeatable seed => identical events. */
    HamEngine other;
    ham_init(&e,p,text,42); ham_init(&other,p,text,42);
    ham_start(&e,0); ham_start(&other,0);
    for(uint32_t t=0;t<600000;t+=10) {
        HamEvent a=ham_tick(&e,t), b=ham_tick(&other,t);
        assert(a.kind==b.kind && a.character==b.character && a.dx==b.dx && a.dy==b.dy);
    }
}
static void durations(void) {
    unsigned seconds = 300;
    const unsigned expected[] = {600, 1800, 3600, 7200, 0, 0};
    for(unsigned i = 0; i < sizeof(expected)/sizeof(expected[0]); ++i) {
        seconds = ham_duration_step(seconds, 1);
        assert(seconds == expected[i]);
    }
    const unsigned reverse[] = {7200, 3600, 1800, 600, 300, 300};
    for(unsigned i = 0; i < sizeof(reverse)/sizeof(reverse[0]); ++i) {
        seconds = ham_duration_step(seconds, -1);
        assert(seconds == reverse[i]);
    }
    assert(ham_duration_step(900, 1) == 1800);
    assert(ham_duration_step(900, -1) == 600);
    HamProfile p = ham_defaults();
    p.duration_seconds = 0;
    HamEngine e;
    ham_init(&e, p, text, 7);
    ham_start(&e, 0);
    for(uint32_t now = 0; now <= 10800000; now += 10) ham_tick(&e, now);
    assert(e.running && !e.complete && e.elapsed == 10800000);
    ham_pause(&e);
    ham_tick(&e, 10805000);
    assert(e.elapsed == 10800000);
    ham_start(&e, 10805000);
    ham_tick(&e, 10806000);
    assert(e.elapsed == 10801000);
    e.profile.duration_seconds = 7200; /* Shortening to less than elapsed ends cleanly. */
    ham_tick(&e, 10806010);
    assert(e.complete && !e.running && !e.key_down && !e.click_down);
    p.duration_seconds = 7200;
    ham_init(&e, p, text, 8);
    ham_start(&e, 0);
    ham_tick(&e, 7199999);
    assert(e.running);
    ham_tick(&e, 7200000);
    assert(e.complete);
    p.duration_seconds = 0;
    ham_init(&e, p, text, 9);
    ham_start(&e, UINT32_MAX - 20);
    e.elapsed = UINT32_MAX - 10ULL;
    ham_tick(&e, 39);
    assert(e.running && e.elapsed > UINT32_MAX); /* Infinite elapsed doesn't wrap at 49 days. */
}
int main(void) {
    controls();
    durations();
    uint64_t low=0,mid=0,high=0;
    for(unsigned seed=1; seed<=200; ++seed) {
        low += run(25,seed,100,false);
        mid += run(50,seed,100,seed==42);
        high += run(100,seed,100,false);
        run(50,seed,seed%2 ? 5 : 1200,false);
    }
    assert(low < mid && mid < high);
    unsigned bursts = burst_bands[0] + burst_bands[1] + burst_bands[2];
    assert(burst_bands[0] > bursts * .82 && burst_bands[0] < bursts * .88);
    assert(burst_bands[1] > bursts * .10 && burst_bands[1] < bursts * .14);
    assert(burst_bands[2] > bursts * .02 && burst_bands[2] < bursts * .04);
    printf("Burst distribution (3-20 / 21-35 / 36-50 words): %.1f%% / %.1f%% / %.1f%%\n",
           100.0*burst_bands[0]/bursts,100.0*burst_bands[1]/bursts,100.0*burst_bands[2]/bursts);
    printf("800 simulated sessions passed; events at 25/50/100%%: %llu / %llu / %llu\n",
           (unsigned long long)low,(unsigned long long)mid,(unsigned long long)high);
    return 0;
}
