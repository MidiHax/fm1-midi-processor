/* Host tests of src/app/clock.c: lockout (Q1-Q4), elapsed time, metronome
 * clicks and placement, SPP, BPM (Q13) and audio clock drift. Run run.bat. */
#include "midiproc.h"
#include <stdio.h>
#include <stdlib.h>

/* ---- Simulated time and audio ---- */
static uint64_t now_us = 1000000;
uint32_t fm1_millis(void) { return (uint32_t)(now_us / 1000); }
uint32_t mp_time_us(void) { return (uint32_t)now_us; }
volatile uint32_t mp_audio_frames;
static uint64_t audio_origin_us = 777; /* when frame 0 started */
static double audio_rate = 44100.0;     /* true audio clock, for drift tests */
static void audio_sync(void) /* frames of all blocks started by now */
{
    uint64_t f = (uint64_t)((double)(now_us - audio_origin_us) * audio_rate / 1e6);
    mp_audio_frames = (uint32_t)(f / 128 * 128);
}
static void advance(uint64_t us)
{
    /* Control task observes often (as mp_service does), at uneven times. */
    while (us) {
        uint64_t step = us < 700 ? us : 300 + (uint64_t)(rand() % 400);
        now_us += step;
        us -= step;
        audio_sync();
        mp_clock_poll();
    }
}
static struct { uint32_t frame; unsigned hz; } clicks[64];
static unsigned nclicks;
void mp_click_post(uint32_t frame, unsigned hz)
{
    if (nclicks < 64) { clicks[nclicks].frame = frame; clicks[nclicks].hz = hz; }
    ++nclicks;
}

enum { TRS, USB, BLE };
static int msg(unsigned port, uint8_t s, uint8_t d1, uint8_t d2)
{
    struct mp_event e = {0};
    e.source = (uint8_t)port; e.data[0] = s; e.data[1] = d1; e.data[2] = d2;
    e.length = 1; e.stamp = (uint32_t)now_us;
    return mp_clock_accept(&e);
}
#define TICK 0xF8
#define START 0xFA
#define CONT 0xFB
#define STOP 0xFC
#define SPP 0xF2

static int failures, checks;
static void check(int ok, const char *what)
{
    ++checks;
    if (!ok) { ++failures; printf("FAIL %s\n", what); }
}
static struct mp_clock_status st(void) { struct mp_clock_status s; mp_clock_status(&s); return s; }

static void reset(void)
{
    mp_clock_init();
    mp_metronome_defaults();
    nclicks = 0;
    advance(50000);
}

int main(void)
{
    srand(1);
    /* -- Lockout -- */
    reset();
    check(st().owner == MP_CLOCK_NONE, "no owner at start");
    check(msg(TRS, TICK, 0, 0) == 1, "first ticks take ownership");
    check(st().owner == TRS, "TRS owns");
    check(msg(USB, TICK, 0, 0) == 0, "Q1: other input's ticks dropped while unlocked");
    check(msg(USB, STOP, 0, 0) == 0, "STOP from non-owner dropped");
    check(msg(USB, START, 0, 0) == 1, "Q2: START from other input takes the lock");
    check(st().owner == USB && st().running, "USB owns and runs");
    check(msg(TRS, TICK, 0, 0) == 0, "locked: TRS tick dropped");
    check(msg(TRS, START, 0, 0) == 0, "locked: TRS START dropped");
    check(msg(TRS, CONT, 0, 0) == 0, "locked: TRS CONTINUE dropped");
    check(msg(TRS, SPP, 1, 0) == 0, "Q4: locked: TRS SPP dropped");
    check(msg(USB, TICK, 0, 0) == 1, "owner tick passes");
    check(msg(USB, STOP, 0, 0) == 1, "owner STOP passes");
    check(!st().running && st().owner == USB, "stopped, USB still owner");
    check(msg(USB, TICK, 0, 0) == 1, "Q1: stopped owner's ticks still pass");
    check(msg(TRS, TICK, 0, 0) == 0, "Q1: unlocked, non-owner ticks dropped");
    check(msg(TRS, CONT, 0, 0) == 1, "Q2: CONTINUE from other input takes the lock");
    check(st().owner == TRS && st().running, "TRS owns after CONTINUE");
    check(msg(USB, TICK, 0, 0) == 0, "old owner now dropped");

    /* -- Q3 timeout -- */
    advance(1900000);
    check(st().owner == TRS, "owner kept before 2 s");
    advance(200000);
    check(st().owner == MP_CLOCK_NONE && !st().running, "Q3: silent owner released after 2 s");
    check(msg(BLE, TICK, 0, 0) == 1 && st().owner == BLE, "any input can take over after timeout");

    /* -- Elapsed -- */
    reset();
    msg(USB, START, 0, 0);
    advance(1000000);
    msg(USB, STOP, 0, 0);
    advance(500000);
    check(st().elapsed_ms >= 999 && st().elapsed_ms <= 1001, "elapsed counts while running only");
    msg(USB, CONT, 0, 0);
    advance(500000);
    check(st().elapsed_ms >= 1499 && st().elapsed_ms <= 1501, "CONTINUE resumes elapsed");
    msg(USB, START, 0, 0);
    advance(10000);
    check(st().elapsed_ms <= 11, "START resets elapsed");

    /* -- Metronome -- */
    reset();
    mp_metronome.enabled = 1;
    msg(USB, START, 0, 0);
    uint64_t start_us = now_us;
    for (int i = 0; i < 96; ++i) { msg(USB, TICK, 0, 0); advance(20833); }
    check(nclicks == 4, "1/4: 4 clicks in 96 ticks");
    check(clicks[0].hz == 1500 && clicks[1].hz == 1000 && clicks[3].hz == 1000, "first beat accented");
    /* Click frame vs. when the tick arrived + 4 ms lead, in true audio frames. */
    double worst = 0;
    for (unsigned i = 0; i < nclicks && i < 4; ++i) {
        double due_us = (double)(start_us + i * 24 * 20833ULL + 4000 - audio_origin_us);
        double frame_us = clicks[i].frame * 1e6 / 44100.0;
        double err = frame_us - due_us;
        printf("  click %u error %+.0f us\n", i, err);
        if (err < 0) err = -err;
        if (err > worst) worst = err;
    }
    printf("click placement error: %.0f us\n", worst);
    check(worst < 200, "click frames within 200 us of tick + lead");
    unsigned bar, beat;
    check(mp_metronome_position(&bar, &beat) && bar == 1 && beat == 4, "position bar 1 beat 4");
    msg(USB, TICK, 0, 0);
    check(mp_metronome_position(&bar, &beat) && bar == 2 && beat == 1, "bar 2 beat 1");

    reset();
    mp_metronome.enabled = 1; mp_metronome.time_base = 2; mp_metronome.beats = 4;
    msg(USB, START, 0, 0);
    for (int i = 0; i < 24; ++i) msg(USB, TICK, 0, 0);
    check(nclicks == 4, "1/16: a click every 6 ticks");

    reset();
    mp_metronome.enabled = 1;
    for (int i = 0; i < 48; ++i) msg(USB, TICK, 0, 0);
    check(nclicks == 0, "Q8: no clicks while stopped");
    msg(USB, START, 0, 0); msg(USB, TICK, 0, 0);
    check(nclicks == 1, "clicks once running");
    mp_metronome.enabled = 0;
    for (int i = 0; i < 48; ++i) msg(USB, TICK, 0, 0);
    check(nclicks == 1, "disabled: no clicks");

    /* SPP then CONTINUE resumes from the pointer: 16th 8 = tick 48 = beat 3. */
    reset();
    mp_metronome.enabled = 1;
    msg(USB, START, 0, 0); msg(USB, TICK, 0, 0); msg(USB, STOP, 0, 0);
    nclicks = 0;
    msg(USB, SPP, 8, 0);
    msg(USB, CONT, 0, 0);
    msg(USB, TICK, 0, 0);
    check(mp_metronome_position(&bar, &beat) && bar == 1 && beat == 3 && nclicks == 1 && clicks[0].hz == 1000,
          "SPP + CONTINUE resumes at beat 3");

    /* -- BPM (Q13) -- */
    reset();
    check(mp_clock_bpm10() == -1, "no BPM before enabling");
    mp_clock_bpm_enable(1);
    for (int i = 0; i < 60; ++i) { msg(USB, TICK, 0, 0); advance(20833); } /* 120 BPM */
    printf("bpm10 at 120: %d\n", mp_clock_bpm10());
    check(mp_clock_bpm10() == 1200, "120.0 BPM");
    for (int i = 0; i < 60; ++i) { msg(USB, TICK, 0, 0); advance(25000); } /* 100 BPM */
    check(mp_clock_bpm10() == 1000, "100.0 BPM after 2 beats");
    advance(1100000);
    check(mp_clock_bpm10() == -1, "---.- after 1 s without ticks");
    mp_clock_bpm_enable(0);
    msg(USB, TICK, 0, 0); advance(20833); msg(USB, TICK, 0, 0);
    check(mp_clock_bpm10() == -1, "not measured while the page is closed");

    /* -- Long run with the audio clock 100 ppm fast, across re-anchors -- */
    for (int sign = -1; sign <= 1; sign += 2) {
        audio_rate = 44100.0 * (1 + sign * 100e-6);
        audio_origin_us = now_us - 123;
        reset();
        mp_metronome.enabled = 1;
        advance(150000000ULL); /* 150 s */
        msg(USB, START, 0, 0);
        uint64_t t0 = now_us;
        nclicks = 0;
        for (int i = 0; i < 48; ++i) { msg(USB, TICK, 0, 0); advance(20833); }
        double w = 0;
        for (unsigned i = 0; i < nclicks && i < 2; ++i) {
            double due = (double)(t0 + i * 24 * 20833ULL + 4000 - audio_origin_us);
            double at = clicks[i].frame * 1e6 / audio_rate;
            double e = at - due;
            printf("  drift click %u error %+.0f us\n", i, e);
            if (e < 0) e = -e;
            if (e > w) w = e;
        }
        printf("drift %+d ppm after 150 s: click error %.0f us\n", sign * 100, w);
        check(nclicks == 2 && w < 150, "tracks a 100 ppm drift across re-anchors");
    }
    audio_rate = 44100.0;

    printf("%d/%d passed\n", checks - failures, checks);
    return failures != 0;
}
