/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * Event Triggers, Edit Trigger and Trigger Messages (SEL button).
 * Layouts: design/fm1-screens.json; behaviour: design/event-triggers.md.
 * Triggers live in triggers.c.
 *
 * Edits apply at once (no draft, no save step): the trigger fires as it is
 * while being edited, and is stored with the patch on Patch Select. SEL on
 * a sub-page goes back one level (screen.c). REC deletes with a second press
 * within 5 s, except on Edit Trigger, where it sends the trigger's messages
 * as a test. Control task only. */
#include "midiproc.h"
#include <string.h>

static const char *const type_names[TRIG_TYPES] = {"NON", "NOF", "CC ", "PC "};
static const char port_letters[MP_PORTS] = {'T', 'U', 'B'};

/* ---- Shared helpers ------------------------------------------------------- */
static void put_padded(char *line, unsigned at, unsigned value, unsigned width)
{
    char text[12];
    unsigned n = mp_format_padded(text, value, width);
    memcpy(line + at, text, n);
}

/* A message value: 0..127, or IN. */
static void put_value(char *line, unsigned at, uint8_t value)
{
    if (value == MP_TRIG_IN) memcpy(line + at, "IN ", 3);
    else put_padded(line, at, value, 3);
}

static void blank_line(char line[UI_COLUMNS + 1])
{
    memset(line, ' ', UI_COLUMNS);
    line[UI_COLUMNS] = 0;
}

static void list_row(unsigned row, const char *line, int selected, uint16_t color)
{
    if (selected) ui_print_colors(row, 0, line, UI_BLACK, UI_BLUE);
    else ui_print(row, 0, line, color);
}

static void scroll_to(unsigned *sel, unsigned *top, unsigned last, unsigned rows, int d)
{
    int s = (int)*sel + d;
    if (s < 0) s = 0;
    if (s > (int)last) s = (int)last;
    *sel = (unsigned)s;
    if (*sel < *top) *top = *sel;
    if (*sel >= *top + rows) *top = *sel - rows + 1U;
    if (*top > last) *top = 0;
}

static unsigned cycle(unsigned value, int d, unsigned count)
{
    return (unsigned)(((int)value + d % (int)count + (int)count) % (int)count);
}

static uint8_t clamp127(unsigned value, int d)
{
    int v = (int)value + d;
    return (uint8_t)(v < 0 ? 0 : v > 127 ? 127 : v);
}

/* IN, 0, 1 ... 127, with IN below 0. */
static uint8_t step_value(uint8_t value, int d)
{
    int v = (value == MP_TRIG_IN ? -1 : (int)value) + d;
    if (v < 0) return MP_TRIG_IN;
    return (uint8_t)(v > 127 ? 127 : v);
}

static int any_control(const struct ui_input *in)
{
    if (in->keys_pressed || (in->pressed & ~UI_BUTTON(BTN_REC))) return 1;
    for (unsigned e = 0; e < 7U; ++e)
        if (in->encoders[e] && (e < ENC_KNOB1 || e > ENC_KNOB4)) return 1;
    return 0;
}

static void key_leds(int lit_channel)
{
    unsigned lit = lit_channel >= 0 ? ui_white_key((unsigned)lit_channel) : UI_KEYS;
    for (unsigned k = 0; k < UI_KEYS; ++k) ui_led(UI_LED_KEY(k), k == lit);
}

static void common_leds(void)
{
    ui_led(BTN_OCT_DOWN, 1);
    ui_led(BTN_OCT_UP, 1);
    ui_led(BTN_PLAY, 1);
}

static int may_be_cut(unsigned index)
{
    return mp_trigger_worst(index) > MP_TRIG_EMIT_MAX;
}

/* The trigger the sub-pages work on. */
static unsigned current;

/* ==== Event Triggers (list) =================================================
 *   row 1   P CH TYP D1      #
 *   rows 2-13  one per trigger, then + NEW TRIGGER
 *   row 14  TRIGS:04 MSGS:019, or TRIGGERS FULL (32) */
enum { TL_TOP = 2, TL_ROWS = 12 };
static struct {
    unsigned sel, top;
    int full;
    struct ui_confirm del;
} tl;

static void open_trigger(unsigned index);

static void tl_enter(void)
{
    ui_clear();
    tl.del.armed = 0;
    tl.full = 0;
}

static void tl_update(const struct ui_input *in, int redraw)
{
    unsigned n = mp_trigger_count();
    if (any_control(in)) tl.del.armed = 0, tl.full = 0;
    scroll_to(&tl.sel, &tl.top, n, TL_ROWS, in->encoders[ENC_SELECT] + ui_step(in));

    int on_trigger = tl.sel < n;
    if (on_trigger && ui_confirm_press(&tl.del, (in->pressed & UI_BUTTON(BTN_REC)) != 0, in->now)) {
        mp_trigger_delete(tl.sel);
        n = mp_trigger_count();
        scroll_to(&tl.sel, &tl.top, n, TL_ROWS, 0);
        on_trigger = tl.sel < n;
    }
    if (in->pressed & UI_BUTTON(BTN_PLAY)) {
        if (on_trigger) {
            open_trigger(tl.sel);
            return;
        }
        /* New trigger: USB, channel 1, PC 0-127, Pass Input YES, Fire ENTER. */
        const struct mp_trigger t = {FM1_MIDI_USB, 0, TRIG_PC, 0, 127, 0, 127, TRIG_PASS | TRIG_ENTER,
                                     0, 0, 0};
        int index = mp_trigger_add(&t);
        if (index >= 0) {
            tl.sel = (unsigned)index;
            open_trigger((unsigned)index);
            return;
        }
        tl.full = 1;
    }

    key_leds(-1);
    common_leds();
    ui_led(BTN_REC, ui_confirm_led(&tl.del, in->now, on_trigger));
    if (!redraw) return;

    ui_header("EVENT TRIGGERS", UI_COLUMNS);
    ui_print(1, 0, "P CH TYP D1      #  ", UI_ACCENT);
    for (unsigned row = 0; row < TL_ROWS; ++row) {
        unsigned i = tl.top + row;
        int selected = i == tl.sel;
        char line[UI_COLUMNS + 1];
        blank_line(line);
        if (i < n) {
            const struct mp_trigger *t = mp_trigger(i);
            line[0] = port_letters[t->port];
            put_padded(line, 2, t->channel + 1U, 2);
            memcpy(line + 5, type_names[t->type], 3);
            put_padded(line, 9, t->d1_lo, 3);
            line[12] = '-';
            put_padded(line, 13, t->d1_hi, 3);
            put_padded(line, 17, t->count, 2);
            list_row(TL_TOP + row, line, selected, UI_WHITE);
            if (!selected && may_be_cut(i)) ui_print(TL_TOP + row, 17, line + 17, UI_RED);
        } else if (i == n) {
            memcpy(line, "+ NEW TRIGGER", 13);
            list_row(TL_TOP + row, line, selected, UI_DIM);
        } else {
            ui_clear_to_end(TL_TOP + row, 0);
        }
    }
    if (tl.full) {
        ui_print(14, 0, "TRIGGERS FULL (32)  ", UI_RED);
    } else {
        char line[UI_COLUMNS + 1];
        blank_line(line);
        memcpy(line, "TRIGS:", 6);
        put_padded(line, 6, n, 2);
        memcpy(line + 9, "MSGS:", 5);
        put_padded(line, 14, mp_trig_msg_total(), 3);
        ui_print(14, 0, line, UI_DIM);
    }
}

const struct ui_page event_triggers_page = {tl_enter, tl_update, 0, 0};

void mp_new_trigger_for(unsigned port, unsigned channel, unsigned type, unsigned d1)
{
    /* D1 is the value seen; D2 is any velocity (NON: 1-127) or value, with
     * Fire ENTER so a fader or pedal fires once per entry. */
    const struct mp_trigger t = {(uint8_t)port, (uint8_t)channel, (uint8_t)type, (uint8_t)d1, (uint8_t)d1,
                                 type == TRIG_NON ? 1U : 0U, 127, TRIG_PASS | TRIG_ENTER, 0, 0, 0};
    int index = mp_trigger_add(&t);
    if (index < 0) {
        ui_show(&event_triggers_page);
        tl.full = 1; /* after enter, which clears it */
        return;
    }
    tl.sel = (unsigned)index;
    open_trigger((unsigned)index);
}

/* ==== Edit Trigger ==========================================================
 *   row 2   PORT:USB   CH:01
 *   row 3   TYPE:PC
 *   row 5   D1 FROM:005 TO:005     row 6: note names for NON/NOF
 *   row 7   D2 FROM:--- TO:---     (--- and skipped for PC)
 *   row 9   PASS INPUT:YES
 *   row 10  FIRE:ENTER
 *   row 12  MESSAGES:12            (red if some may be cut by the cap)
 *   row 14  PLAY:MSGS  REC:TEST */
enum { ET_PORT, ET_CH, ET_TYPE, ET_D1_LO, ET_D1_HI, ET_D2_LO, ET_D2_HI, ET_PASS, ET_FIRE, ET_FIELDS };
enum { TEST_BLINK_MS = 150 };
static struct {
    unsigned focus;
    uint32_t tested_at;
    int tested;
} et;

static void open_trigger(unsigned index)
{
    current = index;
    et.focus = ET_PORT;
    et.tested = 0;
    ui_show(&edit_trigger_page);
}

static int is_d2(unsigned field) { return field == ET_D2_LO || field == ET_D2_HI; }

static void et_enter(void)
{
    ui_clear();
}

static void range_field(unsigned row, unsigned column, uint8_t value, int focused)
{
    char text[4];
    mp_format_padded(text, value, 3);
    ui_field(row, column, text, focused);
}

static void et_update(const struct ui_input *in, int redraw)
{
    const struct mp_trigger *live = mp_trigger(current);
    if (!live) {
        ui_show(&event_triggers_page);
        return;
    }
    struct mp_trigger t = *live;

    /* SELECT: the fields in order, D2 skipped for PC. */
    int select = in->encoders[ENC_SELECT];
    for (; select; select += select > 0 ? -1 : 1) {
        do et.focus = cycle(et.focus, select > 0 ? 1 : -1, ET_FIELDS);
        while (t.type == TRIG_PC && is_d2(et.focus));
    }
    int d = ui_step(in);
    switch (et.focus) {
    case ET_PORT: t.port = (uint8_t)cycle(t.port, d, MP_PORTS); break;
    case ET_CH: t.channel = (uint8_t)cycle(t.channel, d, 16); break;
    case ET_TYPE: t.type = (uint8_t)cycle(t.type, d, TRIG_TYPES); break;
    case ET_D1_LO:
        t.d1_lo = clamp127(t.d1_lo, d);
        if (t.d1_hi < t.d1_lo) t.d1_hi = t.d1_lo;
        break;
    case ET_D1_HI:
        t.d1_hi = clamp127(t.d1_hi, d);
        if (t.d1_lo > t.d1_hi) t.d1_lo = t.d1_hi;
        break;
    case ET_D2_LO:
        t.d2_lo = clamp127(t.d2_lo, d);
        if (t.d2_hi < t.d2_lo) t.d2_hi = t.d2_lo;
        break;
    case ET_D2_HI:
        t.d2_hi = clamp127(t.d2_hi, d);
        if (t.d2_lo > t.d2_hi) t.d2_lo = t.d2_hi;
        break;
    case ET_PASS: if (d) t.flags ^= TRIG_PASS; break;
    default: if (d) t.flags ^= TRIG_ENTER; break;
    }
    int key = ui_white_key_pressed(in);
    if (key >= 0 && et.focus == ET_CH) t.channel = (uint8_t)key;
    if (memcmp(&t, live, sizeof t) != 0) mp_trigger_set(current, &t);

    if (in->pressed & UI_BUTTON(BTN_REC)) {
        mp_trigger_test(current);
        et.tested = 1;
        et.tested_at = in->now;
    }
    if (in->pressed & UI_BUTTON(BTN_PLAY)) {
        ui_show(&trigger_messages_page);
        return;
    }

    key_leds(et.focus == ET_CH ? t.channel : -1);
    common_leds();
    /* REC is lit while it can test; it blinks off when it sends. */
    if (et.tested && in->now - et.tested_at >= TEST_BLINK_MS) et.tested = 0;
    ui_led(BTN_REC, t.count && !et.tested);
    if (!redraw) return;

    char text[UI_COLUMNS + 1];
    memcpy(text, "EDIT TRIGGER ", 13);
    mp_format_padded(text + 13, current + 1U, 2);
    ui_header(text, UI_COLUMNS);

    ui_print(2, 0, "PORT:", UI_ORANGE);
    ui_field(2, 5, mp_port_names[t.port], et.focus == ET_PORT);
    ui_print(2, 8, "   CH:", UI_ORANGE);
    mp_format_padded(text, t.channel + 1U, 2);
    ui_field(2, 14, text, et.focus == ET_CH);
    ui_print(3, 0, "TYPE:", UI_ORANGE);
    ui_field(3, 5, type_names[t.type], et.focus == ET_TYPE);

    ui_print(5, 0, "D1 FROM:", UI_ORANGE);
    range_field(5, 8, t.d1_lo, et.focus == ET_D1_LO);
    ui_print(5, 11, " TO:", UI_ORANGE);
    range_field(5, 15, t.d1_hi, et.focus == ET_D1_HI);
    ui_clear_to_end(6, 0);
    if (t.type == TRIG_NON || t.type == TRIG_NOF) {
        unsigned n = mp_format_note(text, t.d1_lo);
        text[n++] = '-';
        n += mp_format_note(text + n, t.d1_hi);
        text[n] = 0;
        ui_print(6, 8, text, UI_DIM);
    }
    ui_print(7, 0, "D2 FROM:", UI_ORANGE);
    ui_print(7, 11, " TO:", UI_ORANGE);
    if (t.type == TRIG_PC) {
        ui_print(7, 8, "---", UI_DIM);
        ui_print(7, 15, "---", UI_DIM);
    } else {
        range_field(7, 8, t.d2_lo, et.focus == ET_D2_LO);
        range_field(7, 15, t.d2_hi, et.focus == ET_D2_HI);
    }

    ui_print(9, 0, "PASS INPUT:", UI_ORANGE);
    ui_field(9, 11, t.flags & TRIG_PASS ? "YES" : "NO ", et.focus == ET_PASS);
    ui_print(10, 0, "FIRE:", UI_ORANGE);
    ui_field(10, 5, t.flags & TRIG_ENTER ? "ENTER" : "EVERY", et.focus == ET_FIRE);

    ui_print(12, 0, "MESSAGES:", UI_WHITE);
    mp_format_padded(text, t.count, 2);
    ui_print(12, 9, text, may_be_cut(current) ? UI_RED : UI_WHITE);
    ui_print(14, 0, "PLAY:MSGS  REC:TEST ", UI_DIM);
}

const struct ui_page edit_trigger_page = {et_enter, et_update, 0, &event_triggers_page};

/* ==== Trigger Messages ======================================================
 *   row 1   ## TYP CH D1  D2
 *   rows 2-12  one per message (sent top to bottom), then + ADD MESSAGE
 *   row 13  LIST FULL / PATCH FULL in red, or NON WITHOUT NOF in amber
 *   row 14  MSGS:04/32 ALL:019
 * SELECT moves cell by cell; D2 is skipped for PC rows. */
enum { TM_TOP = 2, TM_ROWS = 11 };
enum { CELL_TYPE, CELL_CH, CELL_D1, CELL_D2 };
static const uint8_t cell_column[4] = {3, 7, 10, 14};
static const uint8_t cell_width[4] = {3, 2, 3, 3};
static struct {
    unsigned row, cell, top; /* row == count: + ADD MESSAGE */
    int refused;             /* 1 trigger full, 2 patch full */
    struct ui_confirm del;
} tm;

static void tm_enter(void)
{
    ui_clear();
    tm.row = tm.cell = tm.top = 0;
    tm.refused = 0;
    tm.del.armed = 0;
}

static unsigned last_cell(unsigned row)
{
    const struct mp_trig_msg *m = mp_trig_msg(current, row);
    return m && m->type == TRIG_PC ? CELL_D1 : CELL_D2;
}

static void step_focus(unsigned n, int forward)
{
    if (forward) {
        if (tm.row == n) tm.row = tm.cell = 0;
        else if (tm.cell < last_cell(tm.row)) ++tm.cell;
        else ++tm.row, tm.cell = 0;
    } else if (tm.cell > 0 && tm.row < n) {
        --tm.cell;
    } else {
        tm.row = tm.row == 0 ? n : tm.row - 1U;
        tm.cell = tm.row < n ? last_cell(tm.row) : 0;
    }
}

static void format_message(char line[UI_COLUMNS + 1], unsigned row, const struct mp_trig_msg *m)
{
    blank_line(line);
    put_padded(line, 0, row + 1U, 2);
    memcpy(line + 3, type_names[m->type], 3);
    put_padded(line, 7, m->channel + 1U, 2);
    put_value(line, 10, m->d1);
    if (m->type == TRIG_PC) memcpy(line + 14, "---", 3);
    else put_value(line, 14, m->d2);
}

static void tm_update(const struct ui_input *in, int redraw)
{
    const struct mp_trigger *t = mp_trigger(current);
    if (!t) {
        ui_show(&event_triggers_page);
        return;
    }
    unsigned n = t->count;
    if (any_control(in)) tm.del.armed = 0, tm.refused = 0;
    if (tm.row > n) tm.row = n, tm.cell = 0;

    for (int s = in->encoders[ENC_SELECT]; s; s += s > 0 ? -1 : 1) step_focus(n, s > 0);

    int d = ui_step(in);
    int key = ui_white_key_pressed(in);
    if (tm.row < n && (d || (key >= 0 && tm.cell == CELL_CH))) {
        struct mp_trig_msg m = *mp_trig_msg(current, tm.row);
        switch (tm.cell) {
        case CELL_TYPE: {
            unsigned was = m.type;
            m.type = (uint8_t)cycle(m.type, d, TRIG_TYPES);
            /* A Note On with velocity 0 is a Note Off: start new ones at 100. */
            if (m.type == TRIG_NON && was != TRIG_NON && m.d2 == 0) m.d2 = 100;
            break;
        }
        case CELL_CH: m.channel = (uint8_t)(key >= 0 ? (unsigned)key : cycle(m.channel, d, 16)); break;
        case CELL_D1: m.d1 = step_value(m.d1, d); break;
        default: m.d2 = step_value(m.d2, d); break;
        }
        mp_trig_msg_set(current, tm.row, &m);
    }

    if (in->pressed & UI_BUTTON(BTN_PLAY)) {
        /* On a row: insert a copy below it. On + ADD MESSAGE: CC 0 on channel 1. */
        struct mp_trig_msg m = {TRIG_CC, 0, 0, 0};
        if (tm.row < n) m = *mp_trig_msg(current, tm.row);
        if (mp_trig_msg_insert(current, tm.row < n ? tm.row + 1U : n, &m) == 0) {
            if (tm.row < n) ++tm.row;
            else tm.cell = 0;
        } else {
            tm.refused = n >= MP_TRIG_MSGS ? 1 : 2;
        }
        n = t->count;
    }
    int on_row = tm.row < n;
    if (on_row && ui_confirm_press(&tm.del, (in->pressed & UI_BUTTON(BTN_REC)) != 0, in->now)) {
        mp_trig_msg_delete(current, tm.row);
        n = t->count;
        if (tm.row < n && tm.cell > last_cell(tm.row)) tm.cell = last_cell(tm.row);
        if (tm.row >= n) tm.cell = 0;
        on_row = tm.row < n;
    }
    if (tm.row < tm.top) tm.top = tm.row;
    if (tm.row >= tm.top + TM_ROWS) tm.top = tm.row - TM_ROWS + 1U;

    const struct mp_trig_msg *focused = on_row ? mp_trig_msg(current, tm.row) : 0;
    key_leds(focused && tm.cell == CELL_CH ? focused->channel : -1);
    common_leds();
    ui_led(BTN_REC, ui_confirm_led(&tm.del, in->now, on_row));
    if (!redraw) return;

    char line[UI_COLUMNS + 1];
    memcpy(line, "TRIGGER ", 8);
    put_padded(line, 8, current + 1U, 2);
    memcpy(line + 10, " MESSAGES", 10);
    ui_header(line, UI_COLUMNS);
    ui_print(1, 0, "## TYP CH D1  D2    ", UI_ACCENT);

    unsigned notes_on = 0, notes_off = 0;
    for (unsigned i = 0; i < n; ++i) {
        const struct mp_trig_msg *m = mp_trig_msg(current, i);
        if (m->type == TRIG_NON) ++notes_on;
        if (m->type == TRIG_NOF) ++notes_off;
    }
    for (unsigned row = 0; row < TM_ROWS; ++row) {
        unsigned i = tm.top + row;
        if (i < n) {
            format_message(line, i, mp_trig_msg(current, i));
            ui_print(TM_TOP + row, 0, line, UI_WHITE);
            if (i == tm.row) {
                char cell[4];
                memcpy(cell, line + cell_column[tm.cell], cell_width[tm.cell]);
                cell[cell_width[tm.cell]] = 0;
                ui_print_colors(TM_TOP + row, cell_column[tm.cell], cell, UI_BLACK, UI_BLUE);
            }
        } else if (i == n) {
            blank_line(line);
            memcpy(line, "+ ADD MESSAGE", 13);
            list_row(TM_TOP + row, line, tm.row == n, UI_DIM);
        } else {
            ui_clear_to_end(TM_TOP + row, 0);
        }
    }
    if (tm.refused == 1) ui_print(13, 0, "LIST FULL (32)      ", UI_RED);
    else if (tm.refused == 2) ui_print(13, 0, "PATCH FULL (256)    ", UI_RED);
    else if (notes_on > notes_off) ui_print(13, 0, "NON WITHOUT NOF     ", UI_AMBER);
    else ui_clear_to_end(13, 0);

    blank_line(line);
    memcpy(line, "MSGS:", 5);
    put_padded(line, 5, n, 2);
    memcpy(line + 7, "/32 ALL:", 8);
    put_padded(line, 15, mp_trig_msg_total(), 3);
    ui_print(14, 0, line, may_be_cut(current) ? UI_RED : UI_DIM);
}

const struct ui_page trigger_messages_page = {tm_enter, tm_update, 0, &edit_trigger_page};
