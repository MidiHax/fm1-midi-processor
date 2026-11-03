/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * MIDI processing pipeline. Runs on the control task.
 *
 * Stages (see Spec.txt), in order:
 *   channel filter -> message filter -> [event triggers] -> remap tables
 *   -> note transposer -> channel remap -> USB MIDI OUT
 * Built so far: the filters, event triggers (triggers.c) and both remaps
 * (remap.c). Trigger messages go straight to the output. The note transposer
 * waits on open question 27. Inputs are TRS
 * and USB; BLE input is not connected yet (open question 28). */
#include "midiproc.h"
#include <string.h>

struct mp_filters mp_filters;
const char *const mp_port_names[MP_PORTS] = {"TRS", "USB", "BLE"};

void mp_process_init(void)
{
    mp_remap_init();
    mp_triggers_init();
    mp_clock_init();
    mp_click_init();
    /* Everything passes until filtered. */
    for (unsigned p = 0; p < MP_PORTS; ++p) {
        mp_filters.channels[p] = 0xffffU;
        mp_filters.system[p] = (uint16_t)((1U << SYS_TYPES) - 1U);
        memset(mp_filters.voice[p], (1U << VOICE_TYPES) - 1U, sizeof mp_filters.voice[p]);
    }
}

/* System status to filter type. Active Sensing and the undefined F4, F5,
 * F9, FD are dropped on arrival (midi_router.c) and never get here. */
static int system_type(uint8_t status)
{
    switch (status) {
    case 0xf0: return SYS_SYX;
    case 0xf1: return SYS_MTC;
    case 0xf2: return SYS_SPP;
    case 0xf3: return SYS_SS;
    case 0xf6: return SYS_TR;
    case 0xf8: return SYS_TCK;
    case 0xfa: return SYS_STA;
    case 0xfb: return SYS_CON;
    case 0xfc: return SYS_STP;
    case 0xff: return SYS_RST;
    default:   return -1;
    }
}

static int voice_type(const struct mp_event *in)
{
    switch (in->data[0] & 0xf0U) {
    case 0x80: return VOICE_NOF;
    case 0x90: return in->data[2] ? VOICE_NON : VOICE_NOF; /* velocity 0 is Note Off */
    case 0xa0: return VOICE_PAT;
    case 0xb0: return VOICE_CC;
    case 0xc0: return VOICE_PC;
    case 0xd0: return VOICE_CAT;
    default:   return VOICE_PB;
    }
}

/* Nonzero if the message passes the channel and message filters. */
static int passes_filters(const struct mp_event *in)
{
    unsigned port = in->source;
    uint8_t status = in->data[0];
    if (status >= 0xf0) {
        int type = system_type(status);
        return type < 0 || ((mp_filters.system[port] >> type) & 1U);
    }
    unsigned channel = status & 15U;
    if (!((mp_filters.channels[port] >> channel) & 1U)) return 0;
    return (mp_filters.voice[port][channel] >> voice_type(in)) & 1U;
}

void mp_process(struct mp_event *in)
{
    if (in->source != FM1_MIDI_DIN && in->source != FM1_MIDI_USB) return;
    if (in->length == 0) { /* this device's patch SysEx bypasses the filters (Q9) */
        uint8_t m[MP_SYSEX_FORWARD_MAX];
        unsigned n = mp_event_sysex(in, m);
        if (n && mp_sysex_receive(m, n)) return;
    }
    if (!passes_filters(in)) {
        in->fate = MP_BLOCKED;
        return;
    }
    if (in->length == 0) {
        (void)mp_emit_sysex(in);
        return;
    }
    /* Clock lockout: one input owns the clock (clock.c). */
    if (mp_is_clock_message(in->data[0]) && !mp_clock_accept(in)) {
        in->fate = MP_BLOCKED;
        return;
    }
    /* Triggers match the filtered message as received, before any remap.
     * Order out: the input (unless a trigger stops it), then the trigger
     * messages. */
    int pass = 1;
    uint32_t fired = mp_trigger_match(in->source, in->data, &pass);
    if (pass) {
        uint8_t m[3] = {in->data[0], in->data[1], in->data[2]};
        if (mp_remap(in->source, m)) in->fate = MP_REMAPPED;
        (void)mp_emit(m, in->length);
    }
    if (fired) {
        in->fate = MP_TRIGGERED;
        mp_trigger_send(fired, in->data);
    }
}
