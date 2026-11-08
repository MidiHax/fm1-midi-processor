/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * Build Message Rule: a scratchpad for message remap rules, filled from
 * monitor rows, Live Keys and Knobs Config (ENV = IN, LFO = OUT; on the
 * monitor also F5 = IN, G5 = OUT). Design: design/rule-builder.md; layout:
 * design/fm1-screens.json, "Build Message Rule".
 *
 *   row 0   BUILD MESSAGE RULE
 *   row 1   IN
 *   row 2   P:USB CH:01 TYP:NOTE
 *   row 3   VALUE:060             note name below for NOTE (row 4)
 *   row 5   OUT
 *   row 6   CH:01 VALUE:062       note name below for NOTE (row 7)
 *   row 9   status
 *   row 11  REC AGAIN TO SAVE
 *   row 13  hint
 *
 * Each half is empty (---) or set. OUT's type follows IN; OUT remembers the
 * type it came from, and a different type shows TYPE MISMATCH and blocks
 * REC. REC saves (two presses) through mp_msg_rule_save, replacing a rule
 * with the same input, then clears both halves. Kept in RAM only, across
 * page changes and patch loads. Control task only. */
#include "midiproc.h"
#include <string.h>

enum { F_IN_PORT, F_IN_CH, F_IN_TYPE, F_IN_VALUE, F_OUT_CH, F_OUT_VALUE, FIELDS };
enum { ADDED_MS = 3000, DEFAULT_NOTE = 60 };
enum { ROW_STATUS = 9, ROW_SAVE = 11, ROW_HINT = 13 };

static const char *const type_names[REMAP_TYPES] = {"NOTE", "CC  ", "PC  "};

static struct {
    uint8_t in_set, out_set, out_type;
    struct mp_msg_rule rule;   /* IN: port, channel, type, in; OUT: out, out_channel */
    unsigned focus;
    int failed;                /* last save was refused */
    struct ui_confirm save;
    uint8_t added;             /* show ADDED for ADDED_MS */
    uint32_t added_at;
    struct mp_msg_rule last;   /* the rule last added */
} rb;

static void start_in(void)
{
    if (rb.in_set) return;
    rb.rule.port = FM1_MIDI_USB;
    rb.rule.channel = 0;
    rb.rule.type = REMAP_NOTE;
    rb.rule.in = DEFAULT_NOTE;
    rb.in_set = 1;
}

static void start_out(void)
{
    if (rb.out_set) return;
    rb.rule.out = rb.in_set ? rb.rule.in : DEFAULT_NOTE;
    rb.rule.out_channel = rb.in_set ? rb.rule.channel : 0;
    rb.out_type = rb.in_set ? rb.rule.type : REMAP_NOTE;
    rb.out_set = 1;
}

static void changed(void)
{
    rb.save.armed = 0;
    rb.failed = 0;
    rb.added = 0;
}

void mp_builder_set_in(unsigned port, unsigned channel, unsigned type, unsigned value)
{
    rb.rule.port = (uint8_t)port;
    rb.rule.channel = (uint8_t)(channel & 15U);
    rb.rule.type = (uint8_t)type;
    rb.rule.in = (uint8_t)(value & 127U);
    rb.in_set = 1;
    changed();
}

void mp_builder_set_out(unsigned type, unsigned channel, unsigned value)
{
    rb.out_type = (uint8_t)type;
    rb.rule.out_channel = (uint8_t)(channel & 15U);
    rb.rule.out = (uint8_t)(value & 127U);
    rb.out_set = 1;
    changed();
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

static void edit(const struct ui_input *in)
{
    rb.focus = ui_focus_move(rb.focus, FIELDS, in);
    int d = ui_step(in);
    int key = ui_white_key_pressed(in);
    int in_field = rb.focus <= F_IN_VALUE;
    if (key >= 0 && rb.focus != F_IN_CH && rb.focus != F_OUT_CH) key = -1;
    if (!d && key < 0) return;
    changed();
    if (in_field) start_in();
    else start_out();
    switch (rb.focus) {
    case F_IN_PORT: rb.rule.port = (uint8_t)cycle(rb.rule.port, d, MP_PORTS); break;
    case F_IN_CH:
        rb.rule.channel = (uint8_t)(key >= 0 ? (unsigned)key : cycle(rb.rule.channel, d, 16));
        break;
    case F_IN_TYPE: rb.rule.type = (uint8_t)cycle(rb.rule.type, d, REMAP_TYPES); break;
    case F_IN_VALUE: rb.rule.in = (uint8_t)clamp127(rb.rule.in, d); break;
    case F_OUT_CH:
        rb.rule.out_channel = (uint8_t)(key >= 0 ? (unsigned)key : cycle(rb.rule.out_channel, d, 16));
        break;
    default: rb.rule.out = (uint8_t)clamp127(rb.rule.out, d); break;
    }
}

static void enter(void)
{
    ui_clear();
}

/* A value field: "---" in dim while its half is empty (blue when focused). */
static void value_field(unsigned row, unsigned column, const char *text, int set, unsigned f)
{
    if (set || rb.focus == f) ui_field(row, column, set ? text : "---", rb.focus == f);
    else ui_print(row, column, "---", UI_DIM);
}

static void note_name(unsigned row, unsigned column, int show, unsigned value)
{
    char text[UI_COLUMNS + 1];
    unsigned n = show ? mp_format_note(text, (uint8_t)value) : 0;
    while (n < 6U) text[n++] = ' ';
    text[n] = 0;
    ui_print(row, column, text, UI_DIM);
}

static void update(const struct ui_input *in, int redraw)
{
    if (any_control(in)) rb.save.armed = 0;
    edit(in);
    if (rb.added && in->now - rb.added_at >= ADDED_MS) rb.added = 0;

    int ready = rb.in_set && rb.out_set;
    int mismatch = ready && rb.out_type != rb.rule.type;
    int duplicate = rb.in_set ? mp_msg_rule_find(rb.rule.port, rb.rule.channel, rb.rule.type, rb.rule.in) : -1;
    int list_full = !mismatch && ready && duplicate < 0 &&
                    mp_msg_rules_in_list(rb.rule.port, rb.rule.channel, rb.rule.type) >= MP_MSG_RULES_PER_LIST;
    int patch_full = !mismatch && ready && duplicate < 0 && !list_full &&
                     (rb.failed || mp_msg_rule_count() >= MP_MSG_RULES_TOTAL);
    int can_save = ready && !mismatch && !list_full && !patch_full;

    if (can_save && ui_confirm_press(&rb.save, (in->pressed & UI_BUTTON(BTN_REC)) != 0, in->now)) {
        if (mp_msg_rule_save(&rb.rule, duplicate) == 0) {
            rb.last = rb.rule;
            rb.in_set = rb.out_set = 0;
            rb.focus = F_IN_PORT;
            rb.added = 1;
            rb.added_at = in->now;
            redraw = 1;
        } else {
            rb.failed = 1;
        }
    }
    if (!rb.in_set || !rb.out_set || rb.failed) can_save = 0; /* just saved, or refused */
    if (!can_save) rb.save.armed = 0;

    /* LEDs: REC when it would save; the focused CH field's key; OCT for steps. */
    ui_led(BTN_REC, ui_confirm_led(&rb.save, in->now, can_save));
    ui_led(BTN_OCT_DOWN, 1);
    ui_led(BTN_OCT_UP, 1);
    int lit = -1;
    if (rb.focus == F_IN_CH && rb.in_set) lit = rb.rule.channel;
    if (rb.focus == F_OUT_CH && rb.out_set) lit = rb.rule.out_channel;
    unsigned lit_key = lit >= 0 ? ui_white_key((unsigned)lit) : UI_KEYS;
    for (unsigned k = 0; k < UI_KEYS; ++k) ui_led(UI_LED_KEY(k), k == lit_key);
    if (!redraw) return;

    ui_header("BUILD MESSAGE RULE", UI_COLUMNS);
    char text[UI_COLUMNS + 1];
    ui_print(1, 0, "IN", UI_ACCENT);
    ui_print(2, 0, "P:", UI_ORANGE);
    value_field(2, 2, mp_port_names[rb.rule.port], rb.in_set, F_IN_PORT);
    ui_print(2, 5, " CH:", UI_ORANGE);
    mp_format_padded(text, rb.rule.channel + 1U, 2);
    value_field(2, 9, text, rb.in_set, F_IN_CH);
    ui_print(2, 11, " TYP:", UI_ORANGE);
    if (rb.in_set || rb.focus == F_IN_TYPE) value_field(2, 16, type_names[rb.rule.type], rb.in_set, F_IN_TYPE);
    else ui_print(2, 16, "--- ", UI_DIM);
    ui_print(3, 0, "VALUE:", UI_ORANGE);
    mp_format_padded(text, rb.rule.in, 3);
    value_field(3, 6, text, rb.in_set, F_IN_VALUE);
    ui_clear_to_end(3, 9);
    unsigned type = rb.in_set ? rb.rule.type : rb.out_type;
    note_name(4, 6, rb.in_set && rb.rule.type == REMAP_NOTE, rb.rule.in);

    ui_print(5, 0, "OUT", UI_ACCENT);
    ui_print(6, 0, "CH:", UI_ORANGE);
    mp_format_padded(text, rb.rule.out_channel + 1U, 2);
    value_field(6, 3, text, rb.out_set, F_OUT_CH);
    ui_print(6, 5, " VALUE:", UI_ORANGE);
    mp_format_padded(text, rb.rule.out, 3);
    value_field(6, 12, text, rb.out_set, F_OUT_VALUE);
    ui_clear_to_end(6, 15);
    note_name(7, 12, rb.out_set && !mismatch && type == REMAP_NOTE && rb.out_type == REMAP_NOTE, rb.rule.out);

    if (rb.added) {
        memcpy(text, "ADDED: ", 7);
        memcpy(text + 7, mp_port_names[rb.last.port], 3);
        text[10] = ' ';
        mp_format_padded(text + 11, rb.last.channel + 1U, 2);
        text[13] = ' ';
        memcpy(text + 14, type_names[rb.last.type], 5);
        ui_print(ROW_STATUS, 0, text, UI_GREEN);
        ui_clear_to_end(ROW_STATUS, 18);
    } else if (mismatch) {
        ui_print(ROW_STATUS, 0, "TYPE MISMATCH       ", UI_RED);
    } else if (list_full) {
        ui_print(ROW_STATUS, 0, "LIST FULL (32)      ", UI_RED);
    } else if (patch_full) {
        ui_print(ROW_STATUS, 0, "PATCH FULL (256)    ", UI_RED);
    } else if (ready && duplicate >= 0) {
        ui_print(ROW_STATUS, 0, "REPLACES A RULE     ", UI_AMBER);
    } else if (ready) {
        ui_print(ROW_STATUS, 0, "REC TO ADD          ", UI_DIM);
    } else if (rb.in_set) {
        ui_print(ROW_STATUS, 0, "NEEDS OUT           ", UI_DIM);
    } else if (rb.out_set) {
        ui_print(ROW_STATUS, 0, "NEEDS IN            ", UI_DIM);
    } else {
        ui_clear_to_end(ROW_STATUS, 0);
    }
    if (rb.save.armed) ui_print(ROW_SAVE, 0, "REC AGAIN TO SAVE   ", UI_DIM);
    else ui_clear_to_end(ROW_SAVE, 0);
    ui_print(ROW_HINT, 0, "F5/ENV:IN G5/LFO:OUT", UI_DIM);
}

const struct ui_page rule_builder_page = {enter, update, 0, 0};
