/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * MIDI clock lockout, transport state, metronome timing and BPM, as in
 * design/clock-and-metronome.md (all questions settled).
 *
 * Lockout (after the filters):
 *   - One input is the clock owner. While unlocked, only the owner's clock
 *     messages pass; with no owner, the first input to send any clock
 *     message becomes the owner. Clocks are never merged (Q1).
 *   - START or CONTINUE from any input takes the lock (and ownership) when
 *     unlocked (Q2). While locked, clock messages and Song Position from
 *     other inputs are dropped (Q4).
 *   - STOP from the owner releases the lock; the owner keeps passing ticks
 *     until another input takes over.
 *   - An owner silent for 2 s is forgotten: lock released, no owner (Q3).
 *
 * Metronome: clicks on the owner's ticks while running (Q8), every 24, 12, 6
 * or 3 ticks. START resets the position; CONTINUE resumes it, or starts
 * from a Song Position Pointer received while stopped.
 *
 * Click timing: each tick carries its arrival time in microseconds. The
 * click is scheduled LEAD_US after that, converted to the audio ISR's frame
 * count with a time base tracked here (see track_audio), so its position in
 * the output does not depend on when the control task got to it.
 *
 * Control task only. */
#include "midiproc.h"
#include <string.h>

enum {
    TIMEOUT_MS = 2000,
    LEAD_US = 4000,        /* tick arrival to click: covers control-task delay */
    BPM_TICKS = 48,        /* average over two beats (Q13) */
    BPM_STALE_US = 1000000 /* no tick for 1 s: show ---.- */
};
static const uint8_t ticks_per_click[MP_TIME_BASES] = {24, 12, 6, 3};

struct mp_metronome mp_metronome;

static struct {
    uint8_t owner, locked, running;
    uint32_t owner_ms;      /* last clock message from the owner */
    uint32_t elapsed_ms;    /* accumulated while running, before run_ms */
    uint32_t run_ms;        /* when the current run started or resumed */
    uint32_t ticks;         /* ticks since START, while running */
    uint32_t spp_ticks;     /* position from a Song Position Pointer */
    uint8_t spp_pending;
    uint8_t have_position;
    uint32_t click_index;   /* last click position, for the display */
} clk;

void mp_metronome_defaults(void)
{
    mp_metronome.enabled = 0;
    mp_metronome.time_base = 0;
    mp_metronome.beats = 4;
    mp_metronome.click_hz = 1000;
    mp_metronome.accent_hz = 1500;
}

int mp_is_clock_message(uint8_t status)
{
    return status == 0xf8 || status == 0xfa || status == 0xfb || status == 0xfc || status == 0xf2;
}

/* ---- Audio time base --------------------------------------------------------
 * The audio ISR adds 128 to mp_audio_frames at the start of each block. An
 * observation (now, frames) is never earlier than that block's start, so
 * the earliest-looking observation gives the block timing. The estimate
 * follows the lowest observations and creeps up slowly, which tracks any
 * drift between the audio and timer clocks. */
static struct {
    uint8_t valid;
    uint32_t frames; /* a frame count ... */
    uint32_t us;     /* ... and when that frame started rendering */
    uint32_t creep;  /* fraction of a microsecond, in 1/4096 */
} tb;

static int32_t frames_to_us(int32_t frames) { return (int32_t)((int64_t)frames * 1000000 / FM1_SAMPLE_RATE); }
static int32_t us_to_frames(int32_t us) { return (int32_t)((int64_t)us * FM1_SAMPLE_RATE / 1000000); }

static void track_audio(void)
{
    uint32_t frames = mp_audio_frames;
    uint32_t now = mp_time_us();
    if (!tb.valid) {
        tb.valid = 1;
        tb.frames = frames;
        tb.us = now;
        tb.creep = 0;
        return;
    }
    /* The anchor frame stays put (moving it every call would round off
     * 0.5 us per block); only its time is corrected. */
    int32_t since = (int32_t)(frames - tb.frames);
    uint32_t predicted = tb.us + (uint32_t)frames_to_us(since);
    int32_t late = (int32_t)(now - predicted);
    if (late < 0 || late > 100000) { /* earlier: adopt; or lost track */
        tb.us += (uint32_t)late;
        tb.creep = 0;
    } else { /* creep towards later by late / 4096, keeping the remainder */
        tb.creep += (uint32_t)late;
        tb.us += tb.creep >> 12;
        tb.creep &= 4095U;
    }
    if (since > FM1_SAMPLE_RATE * 60) { /* re-anchor once a minute */
        tb.us += (uint32_t)frames_to_us(since);
        tb.frames = frames;
    }
}

static void schedule_click(uint32_t stamp, unsigned hz)
{
    if (!tb.valid) return;
    int32_t ahead = (int32_t)(stamp + LEAD_US - tb.us);
    mp_click_post(tb.frames + (uint32_t)us_to_frames(ahead), hz);
}

/* ---- BPM ------------------------------------------------------------------ */
static uint8_t bpm_on;
static uint32_t bpm_stamp[BPM_TICKS + 1];
static unsigned bpm_count, bpm_head;

void mp_clock_bpm_enable(int on)
{
    bpm_on = (uint8_t)!!on;
    bpm_count = bpm_head = 0;
}

int mp_clock_bpm10(void)
{
    if (bpm_count < 2) return -1;
    unsigned last = (bpm_head + BPM_TICKS) % (BPM_TICKS + 1U);
    unsigned first = (bpm_head + BPM_TICKS + 1U - bpm_count) % (BPM_TICKS + 1U);
    if ((int32_t)(mp_time_us() - bpm_stamp[last]) > BPM_STALE_US) return -1;
    uint32_t span = bpm_stamp[last] - bpm_stamp[first];
    if (!span) return -1;
    /* BPM = 60e6 / (24 * span / intervals); x10, rounded. */
    uint64_t bpm10 = ((uint64_t)25000000 * (bpm_count - 1U) * 2U + span) / (2U * (uint64_t)span);
    return bpm10 > 99999U ? 99999 : (int)bpm10;
}

static void bpm_tick(uint32_t stamp)
{
    if (!bpm_on) return;
    bpm_stamp[bpm_head] = stamp;
    bpm_head = (bpm_head + 1U) % (BPM_TICKS + 1U);
    if (bpm_count < BPM_TICKS + 1U) ++bpm_count;
}

/* ---- Transport -------------------------------------------------------------- */
static void run(uint32_t now)
{
    if (!clk.running) {
        clk.running = 1;
        clk.run_ms = now;
    }
}

static void stop(uint32_t now)
{
    if (clk.running) {
        clk.elapsed_ms += now - clk.run_ms;
        clk.running = 0;
    }
}

static void tick(uint32_t stamp)
{
    bpm_tick(stamp);
    if (!clk.running) return;
    const struct mp_metronome *m = &mp_metronome;
    unsigned per = ticks_per_click[m->time_base < MP_TIME_BASES ? m->time_base : 0];
    if (clk.ticks % per == 0) {
        clk.click_index = clk.ticks / per;
        clk.have_position = 1;
        if (m->enabled) {
            unsigned beats = m->beats ? m->beats : 1U;
            schedule_click(stamp, clk.click_index % beats == 0 ? m->accent_hz : m->click_hz);
        }
    }
    ++clk.ticks;
}

int mp_clock_accept(const struct mp_event *in)
{
    unsigned port = in->source;
    uint8_t status = in->data[0];
    uint32_t now = fm1_millis();
    if (clk.owner == MP_CLOCK_NONE) clk.owner = (uint8_t)port;
    if (port != clk.owner) {
        if (clk.locked || (status != 0xfa && status != 0xfb)) return 0;
        /* START or CONTINUE from another input while unlocked: take over.
         * The old owner's run (if any) ends here. */
        stop(now);
        clk.owner = (uint8_t)port;
        clk.spp_pending = 0;
    }
    clk.owner_ms = now;
    switch (status) {
    case 0xfa: /* START */
        clk.locked = 1;
        clk.running = 0;
        clk.elapsed_ms = 0;
        clk.ticks = 0;
        clk.spp_pending = 0;
        clk.have_position = 0;
        run(now);
        break;
    case 0xfb: /* CONTINUE */
        clk.locked = 1;
        if (clk.spp_pending) {
            clk.ticks = clk.spp_ticks;
            clk.spp_pending = 0;
        }
        run(now);
        break;
    case 0xfc: /* STOP */
        clk.locked = 0;
        stop(now);
        break;
    case 0xf2: /* Song Position Pointer: 16th notes, 6 ticks each */
        if (!clk.running) {
            clk.spp_ticks = ((uint32_t)in->data[1] | (uint32_t)in->data[2] << 7) * 6U;
            clk.spp_pending = 1;
        }
        break;
    default: /* 0xf8 tick */
        tick(in->stamp);
        break;
    }
    return 1;
}

void mp_clock_poll(void)
{
    track_audio();
    if (clk.owner != MP_CLOCK_NONE && fm1_millis() - clk.owner_ms >= TIMEOUT_MS) {
        stop(fm1_millis());
        clk.owner = MP_CLOCK_NONE;
        clk.locked = 0;
        clk.spp_pending = 0;
    }
}

void mp_clock_status(struct mp_clock_status *out)
{
    out->owner = clk.owner;
    out->running = clk.running;
    out->elapsed_ms = clk.elapsed_ms + (clk.running ? fm1_millis() - clk.run_ms : 0U);
}

int mp_metronome_position(unsigned *bar, unsigned *beat)
{
    if (!clk.have_position) return 0;
    unsigned beats = mp_metronome.beats ? mp_metronome.beats : 1U;
    *bar = clk.click_index / beats + 1U;
    *beat = clk.click_index % beats + 1U;
    return 1;
}

void mp_clock_init(void)
{
    memset(&clk, 0, sizeof clk);
    clk.owner = MP_CLOCK_NONE;
    tb.valid = 0;
    bpm_on = 0;
    bpm_count = bpm_head = 0;
}
