/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * Monitor search pages OP1-OP5 (design/monitor-search.md; layouts in
 * design/fm1-screens.json, "Search Port" .. "Search D2"). Sub-pages of the
 * MIDI Monitor: HOME goes back to it. The search state and the OP keys are
 * in monitor.c.
 *
 *   row 0   SEARCH PORT  OP1
 *   row 2   STATE:ON
 *   row 4+  the page's fields (INC/EXC items, or FROM/TO/MODE)
 *   row 14  HOLD OP1-5 HOME:BACK
 *
 * SELECT moves the focus through STATE and the fields; ALGORITHM and OCT-/
 * OCT+ change the focused field (ON/OFF and INC/EXC flip, numbers step).
 * On the Channel page the lower 16 white keys toggle their channel, lit
 * while INC. A short press of an OP key toggles that search; holding one
 * switches to its page. Control task only. */
#include "midiproc.h"
#include <string.h>

static unsigned focus;

static unsigned field_count(unsigned page)
{
    switch (page) {
    case MON_SEARCH_PORT: return 1U + MP_PORTS;
    case MON_SEARCH_CHANNEL: return 17U;
    case MON_SEARCH_TYPE: return 1U + MON_TYPES;
    default: return 4U; /* STATE, FROM, TO, MODE */
    }
}

static void flip16(uint16_t *mask, unsigned bit) { *mask ^= (uint16_t)(1U << bit); }

static void edit(unsigned page, const struct ui_input *in)
{
    struct mon_search *s = &mon_search;
    focus = ui_focus_move(focus, field_count(page), in);
    int d = ui_step(in);
    if (focus == 0) {
        if (d % 2) s->on[page] ^= 1U;
    } else if (page == MON_SEARCH_PORT) {
        if (d % 2) s->ports ^= (uint8_t)(1U << (focus - 1U));
    } else if (page == MON_SEARCH_CHANNEL) {
        if (d % 2) flip16(&s->channels, focus - 1U);
    } else if (page == MON_SEARCH_TYPE) {
        if (d % 2) flip16(&s->types, focus - 1U);
    } else if (d) {
        unsigned w = page - MON_SEARCH_D1;
        if (focus == 3U) {
            if (d % 2) s->exc[w] ^= 1U;
        } else {
            uint8_t *v = focus == 1U ? &s->lo[w] : &s->hi[w];
            int n = (int)*v + d;
            *v = (uint8_t)(n < 0 ? 0 : n > 127 ? 127 : n);
            if (s->lo[w] > s->hi[w]) { /* FROM pushes TO, and TO pushes FROM */
                if (focus == 1U) s->hi[w] = s->lo[w];
                else s->lo[w] = s->hi[w];
            }
        }
    }
    if (page == MON_SEARCH_CHANNEL) {
        int key = ui_white_key_pressed(in);
        if (key >= 0) flip16(&s->channels, (unsigned)key);
        for (unsigned ch = 0; ch < 16U; ++ch) ui_led(UI_LED_KEY(ui_white_key(ch)), (s->channels >> ch) & 1U);
    }
    ui_led(BTN_OCT_DOWN, 1);
    ui_led(BTN_OCT_UP, 1);
}

static void inc_exc(unsigned row, unsigned column, const char *label, int inc, int focused)
{
    char text[6] = "   :";
    unsigned n = (unsigned)strlen(label);
    memcpy(text + 3U - n, label, n); /* right-aligned to 3 */
    ui_print(row, column, text, UI_ORANGE);
    ui_field(row, column + 5U, inc ? "INC" : "EXC", focused);
}

static void draw(unsigned page)
{
    static const char *const titles[MON_SEARCHES] = {
        "SEARCH PORT  OP1", "SEARCH CHANNEL  OP2", "SEARCH TYPE  OP3", "SEARCH D1  OP4", "SEARCH D2  OP5"};
    const struct mon_search *s = &mon_search;
    ui_header(titles[page], UI_COLUMNS);
    ui_print(2, 0, "STATE:", UI_ORANGE);
    ui_field(2, 6, s->on[page] ? "ON " : "OFF", focus == 0);

    if (page == MON_SEARCH_PORT) {
        for (unsigned p = 0; p < MP_PORTS; ++p)
            inc_exc(4U + p, 0, mp_port_names[p], (s->ports >> p) & 1U, focus == 1U + p);
    } else if (page == MON_SEARCH_CHANNEL) {
        for (unsigned ch = 0; ch < 16U; ++ch) {
            char label[3];
            mp_format_padded(label, ch + 1U, 2);
            inc_exc(4U + ch % 8U, ch < 8U ? 0U : 12U, label, (s->channels >> ch) & 1U, focus == 1U + ch);
        }
    } else if (page == MON_SEARCH_TYPE) {
        for (unsigned t = 0; t < MON_TYPES; ++t) /* channel types left, system types right */
            inc_exc(t < 7U ? 4U + t : 4U + t - 7U, t < 7U ? 0U : 12U, mon_type_names[t], (s->types >> t) & 1U,
                    focus == 1U + t);
    } else {
        unsigned w = page - MON_SEARCH_D1;
        char text[6];
        ui_print(4, 0, "FROM:", UI_ORANGE);
        mp_format_padded(text, s->lo[w], 3);
        ui_field(4, 5, text, focus == 1U);
        ui_print(4, 10, "TO:", UI_ORANGE);
        mp_format_padded(text, s->hi[w], 3);
        ui_field(4, 13, text, focus == 2U);
        if (w == 0) { /* data 1 is a note number for note messages */
            ui_clear_to_end(5, 0);
            mp_format_note(text, s->lo[w]);
            ui_print(5, 5, text, UI_DIM);
            mp_format_note(text, s->hi[w]);
            ui_print(5, 13, text, UI_DIM);
        }
        ui_print(7, 0, "MODE:", UI_ORANGE);
        ui_field(7, 5, s->exc[w] ? "EXC" : "INC", focus == 3U);
    }
    ui_print(14, 0, "HOLD OP1-5 HOME:BACK", UI_DIM);
}

static void update(unsigned page, const struct ui_input *in, int redraw)
{
    if (mon_search_keys(in)) return; /* another OP key held: its page opened */
    edit(page, in);
    if (redraw) draw(page);
}

static void enter(void)
{
    ui_clear();
    focus = 0;
}

static void update_port(const struct ui_input *in, int redraw) { update(MON_SEARCH_PORT, in, redraw); }
static void update_channel(const struct ui_input *in, int redraw) { update(MON_SEARCH_CHANNEL, in, redraw); }
static void update_type(const struct ui_input *in, int redraw) { update(MON_SEARCH_TYPE, in, redraw); }
static void update_d1(const struct ui_input *in, int redraw) { update(MON_SEARCH_D1, in, redraw); }
static void update_d2(const struct ui_input *in, int redraw) { update(MON_SEARCH_D2, in, redraw); }

const struct ui_page mon_search_pages[MON_SEARCHES] = {
    {enter, update_port, 0, &monitor_page},
    {enter, update_channel, 0, &monitor_page},
    {enter, update_type, 0, &monitor_page},
    {enter, update_d1, 0, &monitor_page},
    {enter, update_d2, 0, &monitor_page},
};
