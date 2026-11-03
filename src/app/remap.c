/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * Remap rules and the pipeline stages that apply them (design/decisions.md,
 * "Remaps"; answers: design/open-questions.md 17-21).
 *
 * Message rules change data 1 (note, CC or program number) and set the
 * output channel. A rule is keyed by input port, channel, type and input
 * value; a NOTE rule covers Note On, Note Off and Poly Aftertouch, so a
 * remapped note is always released. Each port/channel/type list holds up to
 * 32 rules, from a pool of 256.
 *
 * Channel rules then move a port's channel to another channel, applied to
 * the channel after any message rule. Up to 32.
 *
 * Control task only. */
#include "midiproc.h"
#include <string.h>

static struct mp_msg_rule msg_rules[MP_MSG_RULES_TOTAL];
static unsigned msg_count;
/* Rules per list, so messages with no rules skip the search. */
static uint8_t list_count[MP_PORTS][16][REMAP_TYPES];

static struct mp_ch_rule ch_rules[MP_CH_RULES];
static unsigned ch_count;

void mp_remap_init(void)
{
    msg_count = ch_count = 0;
    memset(list_count, 0, sizeof list_count);
}

/* ---- Pipeline ------------------------------------------------------------- */
static int remap_type(uint8_t status)
{
    switch (status & 0xf0U) {
    case 0x80: case 0x90: case 0xa0: return REMAP_NOTE;
    case 0xb0: return REMAP_CC;
    case 0xc0: return REMAP_PC;
    default:   return -1;
    }
}

int mp_remap(unsigned port, uint8_t m[3])
{
    if (m[0] >= 0xf0 || port >= MP_PORTS) return 0;
    int matched = 0;
    unsigned channel = m[0] & 15U;
    int type = remap_type(m[0]);
    if (type >= 0 && list_count[port][channel][type]) {
        for (unsigned i = 0; i < msg_count; ++i) {
            const struct mp_msg_rule *r = &msg_rules[i];
            if (r->in == m[1] && r->channel == channel && r->type == type && r->port == port) {
                m[1] = r->out;
                channel = r->out_channel;
                matched = 1;
                break;
            }
        }
    }
    for (unsigned i = 0; i < ch_count; ++i)
        if (ch_rules[i].port == port && ch_rules[i].in == channel) {
            channel = ch_rules[i].out;
            matched = 1;
            break;
        }
    m[0] = (uint8_t)((m[0] & 0xf0U) | channel);
    return matched;
}

/* ---- Message rules -------------------------------------------------------- */
unsigned mp_msg_rule_count(void) { return msg_count; }

const struct mp_msg_rule *mp_msg_rule(unsigned index)
{
    return index < msg_count ? &msg_rules[index] : 0;
}

int mp_msg_rule_find(unsigned port, unsigned channel, unsigned type, unsigned in)
{
    for (unsigned i = 0; i < msg_count; ++i) {
        const struct mp_msg_rule *r = &msg_rules[i];
        if (r->port == port && r->channel == channel && r->type == type && r->in == in) return (int)i;
    }
    return -1;
}

unsigned mp_msg_rule_list(unsigned port, unsigned channel, unsigned type,
                          uint8_t index[MP_MSG_RULES_PER_LIST])
{
    unsigned n = 0;
    for (unsigned i = 0; i < msg_count && n < MP_MSG_RULES_PER_LIST; ++i) {
        const struct mp_msg_rule *r = &msg_rules[i];
        if (r->port != port || r->channel != channel || r->type != type) continue;
        unsigned at = n++; /* insertion sort by input value */
        while (at && msg_rules[index[at - 1]].in > r->in) { index[at] = index[at - 1]; --at; }
        index[at] = (uint8_t)i;
    }
    return n;
}

void mp_msg_rule_delete(unsigned index)
{
    if (index >= msg_count) return;
    const struct mp_msg_rule *r = &msg_rules[index];
    --list_count[r->port][r->channel][r->type];
    msg_rules[index] = msg_rules[--msg_count];
}

int mp_msg_rule_save(const struct mp_msg_rule *rule, int replace)
{
    if (rule->port >= MP_PORTS || rule->channel > 15U || rule->type >= REMAP_TYPES ||
        rule->in > 127U || rule->out > 127U || rule->out_channel > 15U)
        return -1;
    const struct mp_msg_rule *old = replace >= 0 ? mp_msg_rule((unsigned)replace) : 0;
    if (replace >= 0 && !old) return -1;
    int duplicate = mp_msg_rule_find(rule->port, rule->channel, rule->type, rule->in);
    /* Rules left in the target list and in the pool once the edited rule and
     * any duplicate (always in the target list) are removed. */
    unsigned in_list = list_count[rule->port][rule->channel][rule->type];
    unsigned total = msg_count;
    if (old) {
        --total;
        if (old->port == rule->port && old->channel == rule->channel && old->type == rule->type) --in_list;
    }
    if (duplicate >= 0 && duplicate != replace) { --in_list; --total; }
    if (in_list >= MP_MSG_RULES_PER_LIST || total >= MP_MSG_RULES_TOTAL) return -1;
    /* Delete the higher index first so the swap-remove can't move the other. */
    int a = replace, b = duplicate != replace ? duplicate : -1;
    if (a < b) { int t = a; a = b; b = t; }
    if (a >= 0) mp_msg_rule_delete((unsigned)a);
    if (b >= 0) mp_msg_rule_delete((unsigned)b);
    msg_rules[msg_count++] = *rule;
    ++list_count[rule->port][rule->channel][rule->type];
    return 0;
}

unsigned mp_msg_rules_in_list(unsigned port, unsigned channel, unsigned type)
{
    return port < MP_PORTS && channel < 16U && type < REMAP_TYPES ? list_count[port][channel][type] : 0;
}

/* ---- Channel rules (kept sorted by port, then input channel) -------------- */
unsigned mp_ch_rule_count(void) { return ch_count; }

const struct mp_ch_rule *mp_ch_rule(unsigned index)
{
    return index < ch_count ? &ch_rules[index] : 0;
}

int mp_ch_rule_find(unsigned port, unsigned in)
{
    for (unsigned i = 0; i < ch_count; ++i)
        if (ch_rules[i].port == port && ch_rules[i].in == in) return (int)i;
    return -1;
}

void mp_ch_rule_delete(unsigned index)
{
    if (index >= ch_count) return;
    memmove(&ch_rules[index], &ch_rules[index + 1U], (ch_count - index - 1U) * sizeof ch_rules[0]);
    --ch_count;
}

int mp_ch_rule_save(const struct mp_ch_rule *rule, int replace)
{
    if (rule->port >= MP_PORTS || rule->in > 15U || rule->out > 15U) return -1;
    if (replace >= (int)ch_count) return -1;
    int duplicate = mp_ch_rule_find(rule->port, rule->in);
    unsigned freed = (replace >= 0) + (duplicate >= 0 && duplicate != replace);
    if (ch_count + 1U > MP_CH_RULES + freed) return -1;
    int a = replace, b = duplicate != replace ? duplicate : -1;
    if (a < b) { int t = a; a = b; b = t; }
    if (a >= 0) mp_ch_rule_delete((unsigned)a);
    if (b >= 0) mp_ch_rule_delete((unsigned)b);
    unsigned at = 0;
    while (at < ch_count && (ch_rules[at].port < rule->port ||
                             (ch_rules[at].port == rule->port && ch_rules[at].in < rule->in)))
        ++at;
    memmove(&ch_rules[at + 1U], &ch_rules[at], (ch_count - at) * sizeof ch_rules[0]);
    ch_rules[at] = *rule;
    ++ch_count;
    return 0;
}
