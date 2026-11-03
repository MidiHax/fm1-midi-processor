/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * Inert implementations of hooks the closed engine library (libfm1_engine.a)
 * requires from the app. The library still contains the synth engine and
 * sequencer, but this firmware never sends them MIDI, never loads projects,
 * samples or voices, and mutes their audio output (audio_output.c).
 *
 * Do not write to the user Flash partition from here: it holds the stock
 * firmware's projects, samples and voice library. */
#include "midiproc.h"
#include "engine.h"
#include <string.h>

/* Project storage. The library may borrow this buffer as scratch space, so
 * it must be a real buffer of the full project size (4064 bytes). */
struct groove_project { uint8_t bytes[4064]; };
static struct groove_project scratch __attribute__((aligned(4)));

void groove_project_init(void) {}
void groove_project_poll(void) {}
struct groove_project *groove_project_scratch(void) { return &scratch; }
void groove_project_name(char name[13])
{
    memcpy(name, "MIDI PROC   ", 12);
    name[12] = 0;
}
void groove_project_metadata(const struct groove_project *p) { (void)p; }

/* Sample storage. */
void groove_samples_init(void) {}
void groove_samples_poll(void) {}

/* DX7 voice library: always hand back a valid INIT VOICE (unpacked 156-byte
 * DX7 format). Audio-safe: no Flash access or libc calls. */
void groove_fm_library_init(void) {}

FM1_DSP_CODE void groove_fm_program(uint8_t p[156], unsigned program)
{
    (void)program;
    for (unsigned op = 0; op < 6; ++op) {
        uint8_t *q = p + op * 21;
        for (unsigned i = 0; i < 21; ++i) q[i] = 0;
        q[0] = q[1] = q[2] = q[3] = 99;       /* EG rates */
        q[4] = q[5] = q[6] = 99;              /* EG levels 1-3; level 4 = 0 */
        q[8] = 39;                            /* break point C3 */
        q[16] = op == 5 ? 99 : 0;             /* output level: OP1 only */
        q[18] = 1;                            /* coarse 1 */
        q[20] = 7;                            /* detune centre */
    }
    for (unsigned i = 126; i < 130; ++i) p[i] = 99; /* pitch EG rates */
    for (unsigned i = 130; i < 134; ++i) p[i] = 50; /* pitch EG levels */
    p[134] = 0;  /* algorithm 1 */
    p[135] = 0;  /* feedback */
    p[136] = 1;  /* oscillator sync */
    p[137] = 35; /* LFO speed */
    p[138] = p[139] = p[140] = 0;
    p[141] = 1;  /* LFO sync */
    p[142] = 0;  /* LFO wave */
    p[143] = 3;  /* pitch mod sensitivity */
    p[144] = 24; /* transpose C3 */
    /* Name "INIT VOICE" as immediates: audio code must not read Flash
     * constants, which are unavailable during Flash programming. */
    p[145] = 'I'; p[146] = 'N'; p[147] = 'I'; p[148] = 'T'; p[149] = ' ';
    p[150] = 'V'; p[151] = 'O'; p[152] = 'I'; p[153] = 'C'; p[154] = 'E';
    p[155] = 63; /* all operators on */
}
