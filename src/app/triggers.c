/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * Event triggers (design/event-triggers.md).
 *
 * A trigger watches one port, channel and type (NON, NOF, CC, PC) for data 1
 * and data 2 within its ranges. A match sends the trigger's message list
 * straight to the output after the triggering message: not filtered,
 * remapped or matched against triggers again, so there are no loops. All
 * matching triggers fire, in list order, up to MP_TRIG_EMIT_MAX messages
 * per input. Pass Input NO stops the triggering message.
 *
 * Fire ENTER (CC only): fires when data 2 moves into the D2 range from
 * outside it, so a pedal sweep fires once. For NON, NOF and PC it behaves
 * like EVERY.
 *
 * Messages with no trigger on their port, type and channel cost one bit test.
 * Control task only. */
#include "midiproc.h"
#include <string.h>

enum { NONE = 0xff };

static struct mp_trigger trig[MP_TRIGGERS];
static unsigned trig_count, msg_total;
static struct mp_trig_msg pool[MP_TRIG_MSGS_TOTAL];
/* Bit n: some trigger watches channel n+1 for this port and type. */
static uint16_t watch[MP_PORTS][TRIG_TYPES];

/* After any edit: message offsets, the watch masks, and ENTER state. */
static void rebuild(void)
{
    memset(watch, 0, sizeof watch);
    unsigned first = 0;
    for (unsigned i = 0; i < trig_count; ++i) {
        struct mp_trigger *t = &trig[i];
        t->first = (uint16_t)first;
        first += t->count;
        t->last_d2 = NONE;
        watch[t->port][t->type] |= (uint16_t)(1U << t->channel);
    }
}

void mp_triggers_init(void)
{
    trig_count = msg_total = 0;
    rebuild();
}

unsigned mp_emit_reserve(void)
{
    return trig_count ? 1U + MP_TRIG_EMIT_MAX : 4U;
}

/* ---- Pipeline ------------------------------------------------------------- */
static int input_type(const uint8_t m[3])
{
    switch (m[0] & 0xf0U) {
    case 0x80: return TRIG_NOF;
    case 0x90: return m[2] ? TRIG_NON : TRIG_NOF; /* velocity 0 is Note Off */
    case 0xb0: return TRIG_CC;
    case 0xc0: return TRIG_PC;
    default:   return -1;
    }
}

static int within(uint8_t v, uint8_t lo, uint8_t hi) { return v >= lo && v <= hi; }

uint32_t mp_trigger_match(unsigned port, const uint8_t m[3], int *pass)
{
    if (m[0] >= 0xf0 || port >= MP_PORTS) return 0;
    int type = input_type(m);
    unsigned channel = m[0] & 15U;
    if (type < 0 || !((watch[port][type] >> channel) & 1U)) return 0;
    uint32_t fired = 0;
    for (unsigned i = 0; i < trig_count; ++i) {
        struct mp_trigger *t = &trig[i];
        if (t->port != port || t->channel != channel || t->type != (unsigned)type ||
            !within(m[1], t->d1_lo, t->d1_hi))
            continue;
        if (type != TRIG_PC) {
            int inside = within(m[2], t->d2_lo, t->d2_hi);
            if (type == TRIG_CC && (t->flags & TRIG_ENTER)) {
                int was = t->last_d2 != NONE && within(t->last_d2, t->d2_lo, t->d2_hi);
                t->last_d2 = m[2];
                if (was) continue;
            }
            if (!inside) continue;
        }
        fired |= 1UL << i;
        if (!(t->flags & TRIG_PASS)) *pass = 0;
    }
    return fired;
}

static const uint8_t status_of[TRIG_TYPES] = {0x90, 0x80, 0xb0, 0xc0};

/* Build one message; returns its length. in1/in2 replace IN values. */
static unsigned build(const struct mp_trig_msg *r, uint8_t in1, uint8_t in2, uint8_t out[3])
{
    out[0] = (uint8_t)(status_of[r->type] | r->channel);
    out[1] = r->d1 == MP_TRIG_IN ? in1 : r->d1;
    out[2] = r->d2 == MP_TRIG_IN ? in2 : r->d2;
    return r->type == TRIG_PC ? 2U : 3U;
}

/* IN for data 2 of a PC input is its program (it has no data 2). */
static uint8_t second_value(uint8_t status, uint8_t d1, uint8_t d2)
{
    return (status & 0xf0U) == 0xc0U ? d1 : d2;
}

void mp_trigger_send(uint32_t fired, const uint8_t m[3])
{
    uint8_t in2 = second_value(m[0], m[1], m[2]);
    unsigned sent = 0;
    for (unsigned i = 0; fired; ++i, fired >>= 1) {
        if (!(fired & 1U)) continue;
        const struct mp_trigger *t = &trig[i];
        for (unsigned k = 0; k < t->count; ++k) {
            if (sent++ == MP_TRIG_EMIT_MAX) return;
            uint8_t out[3];
            unsigned n = build(&pool[t->first + k], m[1], in2, out);
            (void)mp_emit(out, n);
        }
    }
}

void mp_trigger_test(unsigned index)
{
    if (index >= trig_count) return;
    const struct mp_trigger *t = &trig[index];
    uint8_t in2 = t->type == TRIG_PC ? t->d1_lo : t->d2_lo;
    for (unsigned k = 0; k < t->count; ++k) {
        uint8_t out[3];
        unsigned n = build(&pool[t->first + k], t->d1_lo, in2, out);
        mp_send_local(out, n);
    }
}

/* ---- Editing -------------------------------------------------------------- */
unsigned mp_trigger_count(void) { return trig_count; }
unsigned mp_trig_msg_total(void) { return msg_total; }

const struct mp_trigger *mp_trigger(unsigned index)
{
    return index < trig_count ? &trig[index] : 0;
}

/* Copy and tidy header fields: ranges in order, unknown flags cleared. */
static void set_header(struct mp_trigger *d, const struct mp_trigger *s)
{
    d->port = s->port < MP_PORTS ? s->port : FM1_MIDI_USB;
    d->channel = s->channel & 15U;
    d->type = s->type < TRIG_TYPES ? s->type : TRIG_PC;
    d->d1_lo = s->d1_lo & 0x7fU;
    d->d1_hi = s->d1_hi & 0x7fU;
    d->d2_lo = s->d2_lo & 0x7fU;
    d->d2_hi = s->d2_hi & 0x7fU;
    if (d->d1_lo > d->d1_hi) d->d1_hi = d->d1_lo;
    if (d->d2_lo > d->d2_hi) d->d2_hi = d->d2_lo;
    d->flags = s->flags & (TRIG_PASS | TRIG_ENTER);
}

int mp_trigger_add(const struct mp_trigger *t)
{
    if (trig_count >= MP_TRIGGERS) return -1;
    struct mp_trigger *d = &trig[trig_count];
    set_header(d, t);
    d->count = 0;
    ++trig_count;
    rebuild();
    return (int)trig_count - 1;
}

void mp_trigger_set(unsigned index, const struct mp_trigger *t)
{
    if (index >= trig_count) return;
    set_header(&trig[index], t);
    rebuild();
}

void mp_trigger_delete(unsigned index)
{
    if (index >= trig_count) return;
    const struct mp_trigger *t = &trig[index];
    unsigned first = t->first, count = t->count;
    memmove(&pool[first], &pool[first + count], (msg_total - first - count) * sizeof pool[0]);
    msg_total -= count;
    memmove(&trig[index], &trig[index + 1U], (trig_count - index - 1U) * sizeof trig[0]);
    --trig_count;
    rebuild();
}

const struct mp_trig_msg *mp_trig_msg(unsigned trigger, unsigned row)
{
    if (trigger >= trig_count || row >= trig[trigger].count) return 0;
    return &pool[trig[trigger].first + row];
}

static void tidy_msg(struct mp_trig_msg *d, const struct mp_trig_msg *s)
{
    d->type = s->type < TRIG_TYPES ? s->type : TRIG_CC;
    d->channel = s->channel & 15U;
    d->d1 = s->d1 == MP_TRIG_IN ? MP_TRIG_IN : s->d1 & 0x7fU;
    d->d2 = d->type == TRIG_PC ? 0 : s->d2 == MP_TRIG_IN ? MP_TRIG_IN : s->d2 & 0x7fU;
}

int mp_trig_msg_insert(unsigned trigger, unsigned row, const struct mp_trig_msg *m)
{
    if (trigger >= trig_count) return -1;
    struct mp_trigger *t = &trig[trigger];
    if (t->count >= MP_TRIG_MSGS || msg_total >= MP_TRIG_MSGS_TOTAL) return -1;
    if (row > t->count) row = t->count;
    unsigned at = t->first + row;
    memmove(&pool[at + 1U], &pool[at], (msg_total - at) * sizeof pool[0]);
    tidy_msg(&pool[at], m);
    ++msg_total;
    ++t->count;
    rebuild();
    return 0;
}

void mp_trig_msg_set(unsigned trigger, unsigned row, const struct mp_trig_msg *m)
{
    if (trigger < trig_count && row < trig[trigger].count) tidy_msg(&pool[trig[trigger].first + row], m);
}

void mp_trig_msg_delete(unsigned trigger, unsigned row)
{
    if (trigger >= trig_count || row >= trig[trigger].count) return;
    unsigned at = trig[trigger].first + row;
    memmove(&pool[at], &pool[at + 1U], (msg_total - at - 1U) * sizeof pool[0]);
    --msg_total;
    --trig[trigger].count;
    rebuild();
}

static int overlap(uint8_t a_lo, uint8_t a_hi, uint8_t b_lo, uint8_t b_hi)
{
    return a_lo <= b_hi && b_lo <= a_hi;
}

unsigned mp_trigger_worst(unsigned index)
{
    if (index >= trig_count) return 0;
    const struct mp_trigger *t = &trig[index];
    unsigned total = 0;
    for (unsigned i = 0; i <= index; ++i) {
        const struct mp_trigger *o = &trig[i];
        if (o->port != t->port || o->channel != t->channel || o->type != t->type ||
            !overlap(o->d1_lo, o->d1_hi, t->d1_lo, t->d1_hi))
            continue;
        if (t->type != TRIG_PC && !overlap(o->d2_lo, o->d2_hi, t->d2_lo, t->d2_hi)) continue;
        total += o->count;
    }
    return total;
}
