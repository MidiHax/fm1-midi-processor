/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * Message Remap and Channel Remap: the rule lists (FX cycle) and their Add
 * Rule pages. Layouts: design/fm1-screens.json; behaviour:
 * design/decisions.md, "Remaps". Rules live in remap.c.
 *
 * Lists end with a "+ NEW RULE" row. PLAY on a rule opens it for editing,
 * PLAY on "+ NEW RULE" (or, on Message Remap, with the header focused) adds
 * one. REC deletes the selected rule, or on an Add Rule page saves it; both
 * need a second press within 5 s, with the REC LED flashing in between.
 * Any other control cancels a pending REC. FX on an Add Rule page returns to
 * its list without saving. Control task only. */
#include "midiproc.h"
#include <string.h>

static const char *const type_names[REMAP_TYPES] = {"NOTE", "CC  ", "PC  "};
static const char port_letters[MP_PORTS] = {'T', 'U', 'B'};

static void put_padded(char *line, unsigned at, unsigned value, unsigned width)
{
    char text[12];
    mp_format_padded(text, value, width);
    memcpy(line + at, text, strlen(text));
}

static void blank_line(char line[UI_COLUMNS + 1])
{
    memset(line, ' ', UI_COLUMNS);
    line[UI_COLUMNS] = 0;
}

/* A list row; the selected one is black on blue across the full width. */
static void list_row(unsigned row, const char *line, int selected, uint16_t color)
{
    if (selected) ui_print_colors(row, 0, line, UI_BLACK, UI_BLUE);
    else ui_print(row, 0, line, color);
}

static void new_rule_row(unsigned row, int selected)
{
    char line[UI_COLUMNS + 1];
    blank_line(line);
    memcpy(line, "+ NEW RULE", 10);
    list_row(row, line, selected, UI_DIM);
}

/* Keep `sel` within [0, last] and visible in a window of `rows`. */
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

static unsigned clamp127(unsigned value, int d)
{
    int v = (int)value + d;
    return (unsigned)(v < 0 ? 0 : v > 127 ? 127 : v);
}

static int any_control(const struct ui_input *in)
{
    if (in->keys_pressed || (in->pressed & ~UI_BUTTON(BTN_REC))) return 1;
    for (unsigned e = 0; e < 7U; ++e)
        if (in->encoders[e] && (e < ENC_KNOB1 || e > ENC_KNOB4)) return 1;
    return 0;
}

static void common_leds(void)
{
    ui_led(BTN_OCT_DOWN, 1);
    ui_led(BTN_OCT_UP, 1);
}

static void key_leds(int lit_channel)
{
    unsigned lit = lit_channel >= 0 ? ui_white_key((unsigned)lit_channel) : UI_KEYS;
    for (unsigned k = 0; k < UI_KEYS; ++k) ui_led(UI_LED_KEY(k), k == lit);
}

/* Status line on an Add Rule page. */
static void rule_status(int full, int replacing, const struct ui_confirm *save)
{
    if (full) ui_print(10, 0, "LIST FULL (32)      ", UI_RED);
    else if (replacing) ui_print(10, 0, "REPLACES A RULE     ", UI_AMBER);
    else ui_clear_to_end(10, 0);
    if (save->armed) ui_print(12, 0, "REC AGAIN TO SAVE   ", UI_DIM);
    else ui_clear_to_end(12, 0);
}

/* ==== Message Remap ======================================================== */
enum { MR_PORT, MR_CHANNEL, MR_TYPE, MR_LIST };
enum { MR_TOP = 3, MR_ROWS = UI_ROWS - MR_TOP }; /* 12 rows */
static struct {
    unsigned port, channel, type;    /* the list shown */
    unsigned focus, sel, top;
    int select_in;                   /* after a save: select this input, or -1 */
    struct ui_confirm del;
} mr = {FM1_MIDI_USB, 0, REMAP_NOTE, MR_PORT, 0, 0, -1, {0, 0}};

/* The Add Message Rule draft. */
static struct {
    struct mp_msg_rule rule;
    int index;                       /* rule being edited, or -1 */
    unsigned focus;
    int failed;                      /* last save was refused */
    struct ui_confirm save;
} am;

/* Edit rule `index`, or (-1) start a draft for the list shown with input
 * and output `in`, or a default value if `in` is -1. */
static void open_message_rule(int index, int in)
{
    const struct mp_msg_rule *r = index >= 0 ? mp_msg_rule((unsigned)index) : 0;
    if (r) {
        am.rule = *r;
    } else {
        unsigned start = in >= 0 ? (unsigned)in : mr.type == REMAP_NOTE ? 60U : 0U;
        am.rule = (struct mp_msg_rule){(uint8_t)mr.port, (uint8_t)mr.channel, (uint8_t)mr.type,
                                       (uint8_t)start, (uint8_t)start, (uint8_t)mr.channel};
        index = -1;
    }
    am.index = index;
    am.focus = 0;
    am.failed = 0;
    am.save.armed = 0;
    ui_show(&add_message_rule_page);
}

static void mr_enter(void)
{
    ui_clear();
    mr.del.armed = 0;
}

static void mr_update(const struct ui_input *in, int redraw)
{
    uint8_t list[MP_MSG_RULES_PER_LIST];
    unsigned n = mp_msg_rule_list(mr.port, mr.channel, mr.type, list);
    if (mr.select_in >= 0) {
        mr.focus = MR_LIST;
        for (unsigned i = 0; i < n; ++i)
            if (mp_msg_rule(list[i])->in == (unsigned)mr.select_in) mr.sel = i;
        mr.select_in = -1;
    }
    if (any_control(in)) mr.del.armed = 0;

    /* SELECT steps P -> CH -> TYP -> each list row -> + NEW RULE, then wraps. */
    int select = in->encoders[ENC_SELECT];
    if (select) {
        int positions = MR_LIST + (int)n + 1;
        int p = mr.focus < MR_LIST ? (int)mr.focus : MR_LIST + (int)mr.sel;
        p = (p + select % positions + positions) % positions;
        mr.focus = p < MR_LIST ? (unsigned)p : MR_LIST;
        if (p >= MR_LIST) mr.sel = (unsigned)(p - MR_LIST);
    }
    int d = ui_step(in);
    unsigned port = mr.port, channel = mr.channel, type = mr.type;
    switch (mr.focus) {
    case MR_PORT: mr.port = cycle(mr.port, d, MP_PORTS); break;
    case MR_CHANNEL: mr.channel = cycle(mr.channel, d, 16); break;
    case MR_TYPE: mr.type = cycle(mr.type, d, REMAP_TYPES); break;
    default: break;
    }
    int key = ui_white_key_pressed(in);
    if (key >= 0) mr.channel = (unsigned)key;
    if (port != mr.port || channel != mr.channel || type != mr.type) {
        n = mp_msg_rule_list(mr.port, mr.channel, mr.type, list);
        mr.sel = mr.top = 0;
    }
    scroll_to(&mr.sel, &mr.top, n, MR_ROWS, mr.focus == MR_LIST ? d : 0);

    int on_rule = mr.focus == MR_LIST && mr.sel < n;
    if (on_rule && ui_confirm_press(&mr.del, (in->pressed & UI_BUTTON(BTN_REC)) != 0, in->now)) {
        mp_msg_rule_delete(list[mr.sel]);
        n = mp_msg_rule_list(mr.port, mr.channel, mr.type, list);
        scroll_to(&mr.sel, &mr.top, n, MR_ROWS, 0);
        on_rule = mr.sel < n;
    }
    if (in->pressed & UI_BUTTON(BTN_PLAY)) {
        open_message_rule(on_rule ? list[mr.sel] : -1, -1);
        return;
    }

    key_leds((int)mr.channel);
    common_leds();
    ui_led(BTN_PLAY, 1);
    ui_led(BTN_REC, ui_confirm_led(&mr.del, in->now, on_rule));
    if (!redraw) return;

    ui_header("MESSAGE REMAP", UI_COLUMNS);
    char text[4];
    ui_print(1, 0, "P:", UI_ORANGE);
    ui_field(1, 2, mp_port_names[mr.port], mr.focus == MR_PORT);
    ui_print(1, 5, " CH:", UI_ORANGE);
    mp_format_padded(text, mr.channel + 1U, 2);
    ui_field(1, 9, text, mr.focus == MR_CHANNEL);
    ui_print(1, 11, " TYP:", UI_ORANGE);
    ui_field(1, 16, type_names[mr.type], mr.focus == MR_TYPE);
    ui_print(2, 0, "INP -> MAP TO   CH  ", UI_ACCENT);

    for (unsigned row = 0; row < MR_ROWS; ++row) {
        unsigned i = mr.top + row;
        int selected = mr.focus == MR_LIST && i == mr.sel;
        if (i < n) {
            const struct mp_msg_rule *r = mp_msg_rule(list[i]);
            char line[UI_COLUMNS + 1];
            blank_line(line);
            put_padded(line, 0, r->in, 3);
            put_padded(line, 7, r->out, 3);
            put_padded(line, 16, r->out_channel + 1U, 2);
            list_row(MR_TOP + row, line, selected, UI_WHITE);
        } else if (i == n) {
            new_rule_row(MR_TOP + row, selected);
        } else {
            ui_clear_to_end(MR_TOP + row, 0);
        }
    }
}

const struct ui_page message_remap_page = {mr_enter, mr_update, 0, 0};

void mp_open_message_rule_for(unsigned port, unsigned channel, unsigned type, unsigned in)
{
    mr.port = port;
    mr.channel = channel;
    mr.type = type;
    mr.focus = MR_LIST;
    mr.sel = mr.top = 0;
    mr.select_in = (int)in; /* the list selects this input when shown again */
    open_message_rule(mp_msg_rule_find(port, channel, type, in), (int)in);
}

/* ---- Add Message Rule ------------------------------------------------------
 *   row 2   P:USB CH:16 TYP:NOTE   cyan/white: from the list, not editable
 *   row 4   INPUT VALUE:  060      (note name below for NOTE rules)
 *   row 6   OUTPUT VALUE: 062
 *   row 8   OUT MIDI CHANNEL:16
 *   row 10  status, row 12 save prompt */
enum { AM_IN, AM_OUT, AM_CHANNEL, AM_FIELDS };

static void am_enter(void)
{
    ui_clear();
}

static void note_hint(unsigned row, unsigned value)
{
    if (am.rule.type != REMAP_NOTE) {
        ui_clear_to_end(row, 0);
        return;
    }
    char text[UI_COLUMNS + 1];
    unsigned n = mp_format_note(text, (uint8_t)value);
    while (n < 6U) text[n++] = ' ';
    text[n] = 0;
    ui_print(row, 14, text, UI_DIM);
}

static void am_update(const struct ui_input *in, int redraw)
{
    if (any_control(in)) am.save.armed = 0;
    am.focus = ui_focus_move(am.focus, AM_FIELDS, in);
    int d = ui_step(in);
    if (d) am.failed = 0;
    switch (am.focus) {
    case AM_IN: am.rule.in = (uint8_t)clamp127(am.rule.in, d); break;
    case AM_OUT: am.rule.out = (uint8_t)clamp127(am.rule.out, d); break;
    default: am.rule.out_channel = (uint8_t)cycle(am.rule.out_channel, d, 16); break;
    }
    int key = ui_white_key_pressed(in);
    if (key >= 0) am.rule.out_channel = (uint8_t)key;

    int duplicate = mp_msg_rule_find(am.rule.port, am.rule.channel, am.rule.type, am.rule.in);
    int replacing = duplicate >= 0 && duplicate != am.index;
    int full = am.failed || (am.index < 0 && !replacing &&
                             mp_msg_rules_in_list(am.rule.port, am.rule.channel, am.rule.type) >=
                                 MP_MSG_RULES_PER_LIST);
    if (!full && ui_confirm_press(&am.save, (in->pressed & UI_BUTTON(BTN_REC)) != 0, in->now)) {
        if (mp_msg_rule_save(&am.rule, am.index) == 0) {
            mr.select_in = am.rule.in;
            ui_show(&message_remap_page);
            return;
        }
        am.failed = full = 1;
    }

    key_leds(am.rule.out_channel);
    common_leds();
    ui_led(BTN_REC, ui_confirm_led(&am.save, in->now, !full));
    if (!redraw) return;

    ui_header(am.index >= 0 ? "EDIT MESSAGE RULE" : "ADD MESSAGE RULE", UI_COLUMNS);
    char text[4];
    ui_print(2, 0, "P:", UI_ACCENT);
    ui_print(2, 2, mp_port_names[am.rule.port], UI_WHITE);
    ui_print(2, 6, "CH:", UI_ACCENT);
    mp_format_padded(text, am.rule.channel + 1U, 2);
    ui_print(2, 9, text, UI_WHITE);
    ui_print(2, 12, "TYP:", UI_ACCENT);
    ui_print(2, 16, type_names[am.rule.type], UI_WHITE);

    ui_print(4, 0, "INPUT VALUE:", UI_ORANGE);
    mp_format_padded(text, am.rule.in, 3);
    ui_field(4, 14, text, am.focus == AM_IN);
    note_hint(5, am.rule.in);
    ui_print(6, 0, "OUTPUT VALUE:", UI_ORANGE);
    mp_format_padded(text, am.rule.out, 3);
    ui_field(6, 14, text, am.focus == AM_OUT);
    note_hint(7, am.rule.out);
    ui_print(8, 0, "OUT MIDI CHANNEL:", UI_ORANGE);
    mp_format_padded(text, am.rule.out_channel + 1U, 2);
    ui_field(8, 17, text, am.focus == AM_CHANNEL);
    rule_status(full, replacing, &am.save);
}

const struct ui_page add_message_rule_page = {am_enter, am_update, 0, &message_remap_page};

/* ==== Channel Remap ======================================================== */
enum { CR_TOP = 2, CR_ROWS = UI_ROWS - CR_TOP }; /* 13 rows */
static struct {
    unsigned sel, top;
    int select_port, select_in;      /* after a save: select this rule, or -1 */
    struct ui_confirm del;
} cr = {0, 0, -1, -1, {0, 0}};

static struct {
    struct mp_ch_rule rule;
    int index;
    unsigned focus;
    int failed;
    struct ui_confirm save;
} ac;

/* Edit rule `index`, or (-1) start a draft from `draft`. */
static void open_channel_rule(int index, struct mp_ch_rule draft)
{
    const struct mp_ch_rule *r = index >= 0 ? mp_ch_rule((unsigned)index) : 0;
    ac.rule = r ? *r : draft;
    ac.index = r ? index : -1;
    ac.focus = 0;
    ac.failed = 0;
    ac.save.armed = 0;
    ui_show(&add_channel_rule_page);
}

static void cr_enter(void)
{
    ui_clear();
    cr.del.armed = 0;
}

static void cr_update(const struct ui_input *in, int redraw)
{
    unsigned n = mp_ch_rule_count();
    if (cr.select_in >= 0) {
        int i = mp_ch_rule_find((unsigned)cr.select_port, (unsigned)cr.select_in);
        if (i >= 0) cr.sel = (unsigned)i;
        cr.select_in = -1;
    }
    if (any_control(in)) cr.del.armed = 0;
    /* SELECT, ALGORITHM and OCT-/OCT+ all scroll; OCT+ moves down. */
    scroll_to(&cr.sel, &cr.top, n, CR_ROWS, in->encoders[ENC_SELECT] + ui_step(in));

    int on_rule = cr.sel < n;
    if (on_rule && ui_confirm_press(&cr.del, (in->pressed & UI_BUTTON(BTN_REC)) != 0, in->now)) {
        mp_ch_rule_delete(cr.sel);
        n = mp_ch_rule_count();
        scroll_to(&cr.sel, &cr.top, n, CR_ROWS, 0);
        on_rule = cr.sel < n;
    }
    if (in->pressed & UI_BUTTON(BTN_PLAY)) {
        open_channel_rule(on_rule ? (int)cr.sel : -1, (struct mp_ch_rule){FM1_MIDI_USB, 0, 0});
        return;
    }

    key_leds(-1);
    common_leds();
    ui_led(BTN_PLAY, 1);
    ui_led(BTN_REC, ui_confirm_led(&cr.del, in->now, on_rule));
    if (!redraw) return;

    ui_header("CHANNEL REMAP", UI_COLUMNS);
    ui_print(1, 0, "SRC PORT CH ->OUT CH", UI_ACCENT);
    for (unsigned row = 0; row < CR_ROWS; ++row) {
        unsigned i = cr.top + row;
        if (i < n) {
            const struct mp_ch_rule *r = mp_ch_rule(i);
            char line[UI_COLUMNS + 1];
            blank_line(line);
            line[4] = port_letters[r->port];
            put_padded(line, 9, r->in + 1U, 2);
            put_padded(line, 17, r->out + 1U, 2);
            list_row(CR_TOP + row, line, i == cr.sel, UI_WHITE);
        } else if (i == n) {
            new_rule_row(CR_TOP + row, i == cr.sel);
        } else {
            ui_clear_to_end(CR_TOP + row, 0);
        }
    }
}

const struct ui_page channel_remap_page = {cr_enter, cr_update, 0, 0};

void mp_open_channel_rule_for(unsigned port, unsigned channel)
{
    int index = mp_ch_rule_find(port, channel);
    if (index >= 0) cr.sel = (unsigned)index;
    open_channel_rule(index, (struct mp_ch_rule){(uint8_t)port, (uint8_t)channel, (uint8_t)channel});
}

/* ---- Add Channel Rule ------------------------------------------------------
 *   row 2   SOURCE
 *   row 3    INPUT PORT:USB
 *   row 5    INPUT CHANNEL:10
 *   row 7   DESTINATION
 *   row 8    OUTPUT CHANNEL:01
 *   row 10  status, row 12 save prompt
 * The lower 16 white keys set the focused channel field; its key is lit. */
enum { AC_PORT, AC_IN, AC_OUT, AC_FIELDS };

static void ac_enter(void)
{
    ui_clear();
}

static void ac_update(const struct ui_input *in, int redraw)
{
    if (any_control(in)) ac.save.armed = 0;
    ac.focus = ui_focus_move(ac.focus, AC_FIELDS, in);
    int d = ui_step(in);
    if (d) ac.failed = 0;
    uint8_t *channel = ac.focus == AC_IN ? &ac.rule.in : ac.focus == AC_OUT ? &ac.rule.out : 0;
    if (ac.focus == AC_PORT) ac.rule.port = (uint8_t)cycle(ac.rule.port, d, MP_PORTS);
    else *channel = (uint8_t)cycle(*channel, d, 16);
    int key = ui_white_key_pressed(in);
    if (key >= 0 && channel) *channel = (uint8_t)key;

    int duplicate = mp_ch_rule_find(ac.rule.port, ac.rule.in);
    int replacing = duplicate >= 0 && duplicate != ac.index;
    int full = ac.failed || (ac.index < 0 && !replacing && mp_ch_rule_count() >= MP_CH_RULES);
    if (!full && ui_confirm_press(&ac.save, (in->pressed & UI_BUTTON(BTN_REC)) != 0, in->now)) {
        if (mp_ch_rule_save(&ac.rule, ac.index) == 0) {
            cr.select_port = ac.rule.port;
            cr.select_in = ac.rule.in;
            ui_show(&channel_remap_page);
            return;
        }
        ac.failed = full = 1;
    }

    key_leds(channel ? *channel : -1);
    common_leds();
    ui_led(BTN_REC, ui_confirm_led(&ac.save, in->now, !full));
    if (!redraw) return;

    ui_header(ac.index >= 0 ? "EDIT CHANNEL RULE" : "ADD CHANNEL RULE", UI_COLUMNS);
    char text[4];
    ui_print(2, 0, "SOURCE", UI_WHITE);
    ui_print(3, 1, "INPUT PORT:", UI_ORANGE);
    ui_field(3, 12, mp_port_names[ac.rule.port], ac.focus == AC_PORT);
    ui_print(5, 1, "INPUT CHANNEL:", UI_ORANGE);
    mp_format_padded(text, ac.rule.in + 1U, 2);
    ui_field(5, 15, text, ac.focus == AC_IN);
    ui_print(7, 0, "DESTINATION", UI_WHITE);
    ui_print(8, 1, "OUTPUT CHANNEL:", UI_ORANGE);
    mp_format_padded(text, ac.rule.out + 1U, 2);
    ui_field(8, 16, text, ac.focus == AC_OUT);
    rule_status(full, replacing, &ac.save);
}

const struct ui_page add_channel_rule_page = {ac_enter, ac_update, 0, &channel_remap_page};
