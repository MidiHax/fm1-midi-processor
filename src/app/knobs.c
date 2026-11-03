/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * KNOB 1-4 and the Knobs Config page. Layout: design/fm1-screens.json,
 * "Knobs Config"; answers: design/open-questions.md 10-13.
 *
 * Each knob keeps a value 0..127 and sends it as an absolute CC on its own
 * channel whenever it is turned, from any page. Knob messages go straight
 * to the output, not through the pipeline. CC 120-127 (channel mode
 * messages) are not offered.
 *
 *   row 0   KNOBS CONFIG
 *   row 2   KNOB 1           64      value in dim
 *   row 3   CH:01  CC NUMBER:020     blue fields; focused = black on blue
 *   row 4   ==========-------        the value as a bar
 *   ... the same for KNOB 2-4 on rows 5-7, 8-10, 11-13.
 *
 * Controls: SELECT moves the focus through the 8 fields. ALGORITHM and
 * OCT-/OCT+ change the focused value. On a channel field, the lower 16
 * white keys pick channel 1-16, and the key for the current channel is lit.
 * Control task only. */
#include "midiproc.h"

enum { KNOBS = MP_KNOBS, MAX_CC = MP_KNOB_MAX_CC, FIELDS = KNOBS * 2 };
struct mp_knob mp_knobs[MP_KNOBS];
#define knobs mp_knobs

void knobs_defaults(void)
{
    /* CC 20-23 are undefined in the MIDI spec, so safe defaults. */
    for (unsigned k = 0; k < KNOBS; ++k) {
        knobs[k].channel = 0;
        knobs[k].cc = (uint8_t)(20U + k);
    }
}

void knobs_init(void)
{
    knobs_defaults();
    for (unsigned k = 0; k < KNOBS; ++k) knobs[k].value = 0;
}

void knobs_service(const struct ui_input *in)
{
    for (unsigned k = 0; k < KNOBS; ++k) {
        int turn = in->encoders[ENC_KNOB1 + k];
        if (!turn) continue;
        int v = knobs[k].value + turn;
        if (v < 0) v = 0;
        if (v > 127) v = 127;
        if (v == knobs[k].value) continue;
        knobs[k].value = (uint8_t)v;
        uint8_t m[3] = {(uint8_t)(0xb0U | knobs[k].channel), knobs[k].cc, (uint8_t)v};
        mp_send_local(m, 3);
    }
}

/* ---- Page ------------------------------------------------------------------ */
static unsigned focus; /* field: knob * 2 + (0 channel, 1 CC number) */

static void enter(void)
{
    ui_clear();
}

static int step(const struct ui_input *in)
{
    int d = in->encoders[ENC_ALGORITHM];
    if (in->repeat & UI_BUTTON(BTN_OCT_UP)) ++d;
    if (in->repeat & UI_BUTTON(BTN_OCT_DOWN)) --d;
    return d;
}

static void edit(const struct ui_input *in)
{
    int select = in->encoders[ENC_SELECT];
    if (select) focus = (unsigned)(((int)focus + select % FIELDS + FIELDS) % FIELDS);

    struct mp_knob *k = &knobs[focus / 2U];
    int d = step(in);
    if (focus % 2U == 0) {
        if (d) k->channel = (uint8_t)(((int)k->channel + d % 16 + 16) % 16); /* cycles */
        for (unsigned key = 0; key < UI_KEYS; ++key) {
            int n = ui_white_key_index(key);
            if (n >= 0 && (in->keys_pressed & (1UL << key))) k->channel = (uint8_t)n;
        }
    } else if (d) {
        int cc = (int)k->cc + d;
        k->cc = (uint8_t)(cc < 0 ? 0 : cc > MAX_CC ? MAX_CC : cc);
    }

    /* Light the white key of the focused channel. */
    unsigned lit = focus % 2U == 0 ? ui_white_key(k->channel) : UI_KEYS;
    for (unsigned key = 0; key < UI_KEYS; ++key) ui_led(UI_LED_KEY(key), key == lit);
    ui_led(BTN_OCT_DOWN, 1); /* they step the focused value */
    ui_led(BTN_OCT_UP, 1);
}

/* ENV / LFO: the focused knob's CC into Build Message Rule's IN / OUT
 * (design/rule-builder.md). Returns 1 if the builder was opened. */
static int build(const struct ui_input *in)
{
    const struct mp_knob *k = &knobs[focus / 2U];
    ui_led(BTN_ENV, 1);
    ui_led(BTN_LFO, 1);
    if (in->pressed & UI_BUTTON(BTN_ENV))
        mp_builder_set_in(FM1_MIDI_USB, k->channel, REMAP_CC, k->cc);
    else if (in->pressed & UI_BUTTON(BTN_LFO))
        mp_builder_set_out(REMAP_CC, k->channel, k->cc);
    else
        return 0;
    ui_open_builder();
    return 1;
}

static void update(const struct ui_input *in, int redraw)
{
    if (build(in)) return;
    edit(in);
    if (!redraw) return;

    ui_header("KNOBS CONFIG", UI_COLUMNS);
    for (unsigned i = 0; i < KNOBS; ++i) {
        unsigned row = 2U + 3U * i;
        char text[12];
        ui_print(row, 0, "KNOB ", UI_WHITE);
        text[0] = (char)('1' + i);
        text[1] = 0;
        ui_print(row, 5, text, UI_WHITE);
        char value[4] = "   ";
        char digits[11];
        unsigned n = mp_format_uint(digits, knobs[i].value);
        for (unsigned c = 0; c < n; ++c) value[3U - n + c] = digits[c];
        ui_print(row, 17, value, UI_DIM);

        ui_print(row + 1U, 0, "CH:", UI_ORANGE);
        mp_format_padded(text, knobs[i].channel + 1U, 2);
        ui_field(row + 1U, 3, text, focus == 2U * i);
        ui_print(row + 1U, 7, "CC NUMBER:", UI_ORANGE);
        mp_format_padded(text, knobs[i].cc, 3);
        ui_field(row + 1U, 17, text, focus == 2U * i + 1U);

        ui_row_bar(row + 2U, knobs[i].value * FM1_SCREEN_WIDTH / 127U, UI_ACCENT, UI_PANEL);
    }
}

const struct ui_page knobs_page = {enter, update, 0};
