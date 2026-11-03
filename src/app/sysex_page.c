/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * Sysex Dump page (SAVE, after Patch Select). Layout: design/fm1-screens.json,
 * "Sysex Dump"; format: design/patch-format.md.
 *
 *   row 0   SYSEX DUMP
 *   row 2   SYSEX PAYLOAD
 *   row 3    PATCH NUMBER:16         1-16 or ALL
 *   row 5   status: REC TO SEND, SENDING PATCH 05, SENT ...
 *   row 7   CONFIG
 *   row 8    ALLOW DUMP REQ:YES
 *   row 10   DEVICE ID:017           0-127
 *   row 12  RX OK:3 BAD:0 REQ:1      patches received, rejected, requests
 *   row 13  Flash error of a received patch, if any
 *
 * Controls: SELECT moves the focus; ALGORITHM and OCT-/OCT+ change the
 * focused value. With PATCH NUMBER focused, the lower 16 white keys pick
 * patch 1-16. REC sends the dump to USB MIDI OUT (one press: sending only
 * reads). The patch dumps are the stored patches, not unsaved edits.
 * Device ID and Allow Dump Req take effect at once and are written to Flash
 * when you leave the page (only if changed). Control task only. */
#include "midiproc.h"

enum { F_PATCH, F_ALLOW, F_DEVICE, FIELDS };
static unsigned focus, what; /* what: 0-15 or MP_DUMP_ALL */
static int sent;             /* a dump finished since entering: show SENT */
static unsigned sent_what;
static int was_sending;

static void enter(void)
{
    ui_clear();
    focus = F_PATCH;
    sent = was_sending = 0;
    what = mp_patch_current();
}

static void edit(const struct ui_input *in)
{
    focus = ui_focus_move(focus, FIELDS, in);
    int d = ui_step(in);
    unsigned id = mp_device_id();
    int allow = mp_allow_dump_request();
    switch (focus) {
    case F_PATCH: {
        int n = (int)MP_DUMP_ALL + 1; /* 1-16, ALL */
        if (d) what = (unsigned)(((int)what + d % n + n) % n);
        int key = ui_white_key_pressed(in);
        if (key >= 0) what = (unsigned)key;
        break;
    }
    case F_ALLOW:
        if (d % 2) mp_device_settings_set(id, !allow);
        break;
    default:
        if (d) {
            int v = (int)id + d;
            mp_device_settings_set((unsigned)(v < 0 ? 0 : v > 127 ? 127 : v), allow);
        }
        break;
    }
    if (in->pressed & UI_BUTTON(BTN_REC) && mp_sysex_send(what) == 0) {
        sent = 0;
        sent_what = what;
    }
    int sending = mp_sysex_sending(0, 0);
    if (was_sending && !sending) sent = 1;
    was_sending = sending;
}

static void leds(const struct ui_input *in)
{
    unsigned lit = focus == F_PATCH && what < MP_PATCHES ? ui_white_key(what) : UI_KEYS;
    for (unsigned k = 0; k < UI_KEYS; ++k) ui_led(UI_LED_KEY(k), k == lit);
    ui_led(BTN_OCT_DOWN, 1);
    ui_led(BTN_OCT_UP, 1);
    /* Lit; flashing while a dump is going out. */
    ui_led(BTN_REC, mp_sysex_sending(0, 0) ? (in->now / 100U) % 2U == 0 : 1);
}

static unsigned put_text(char *line, unsigned at, const char *s)
{
    while (*s && at < UI_COLUMNS) line[at++] = *s++;
    return at;
}

static unsigned put_number(char *line, unsigned at, uint32_t v)
{
    char digits[11];
    mp_format_uint(digits, v);
    return put_text(line, at, digits);
}

static unsigned put_patch(char *line, unsigned at, unsigned slot)
{
    if (slot == MP_DUMP_ALL) return put_text(line, at, "ALL");
    if (slot == MP_PATCH_EDIT_BUFFER) return put_text(line, at, "EDIT");
    char text[4];
    mp_format_padded(text, slot + 1U, 2);
    return put_text(line, at, text);
}

static void finish(char *line, unsigned n)
{
    while (n < UI_COLUMNS) line[n++] = ' ';
    line[UI_COLUMNS] = 0;
}

static void draw_status(void)
{
    char line[UI_COLUMNS + 1];
    unsigned slot, last, n;
    if (mp_sysex_sending(&slot, &last)) {
        n = put_text(line, 0, "SENDING PATCH ");
        n = put_patch(line, n, slot);
        finish(line, n);
        ui_print(5, 0, line, UI_GREEN);
    } else if (sent) {
        n = put_text(line, 0, "SENT ");
        n = put_patch(line, n, sent_what);
        finish(line, n);
        ui_print(5, 0, line, UI_GREEN);
    } else {
        ui_print(5, 0, "REC TO SEND         ", UI_DIM);
    }

    struct mp_sysex_counts c;
    mp_sysex_counts(&c);
    n = put_text(line, 0, "RX OK:");
    n = put_number(line, n, c.received);
    n = put_text(line, n, " BAD:");
    n = put_number(line, n, c.rejected);
    n = put_text(line, n, " REQ:");
    n = put_number(line, n, c.requests);
    finish(line, n);
    ui_print(12, 0, line, UI_DIM);
    if (c.last_error) {
        n = put_text(line, 0, "FLASH ERROR -");
        n = put_number(line, n, (uint32_t)-c.last_error);
        finish(line, n);
        ui_print(13, 0, line, UI_RED);
    } else {
        ui_clear_to_end(13, 0);
    }
}

static void update(const struct ui_input *in, int redraw)
{
    edit(in);
    leds(in);
    if (!redraw) return;

    ui_header("SYSEX DUMP", UI_COLUMNS);
    char text[4];
    ui_print(2, 0, "SYSEX PAYLOAD", UI_WHITE);
    ui_print(3, 1, "PATCH NUMBER:", UI_ORANGE);
    if (what == MP_DUMP_ALL) {
        ui_field(3, 14, "ALL", focus == F_PATCH);
    } else {
        mp_format_padded(text, what + 1U, 2);
        ui_field(3, 14, text, focus == F_PATCH);
        ui_clear_to_end(3, 16);
    }
    draw_status();

    ui_print(7, 0, "CONFIG", UI_WHITE);
    ui_print(8, 1, "ALLOW DUMP REQ:", UI_ORANGE);
    ui_field(8, 16, mp_allow_dump_request() ? "YES" : "NO ", focus == F_ALLOW);
    ui_print(10, 1, "DEVICE ID:", UI_ORANGE);
    mp_format_padded(text, mp_device_id(), 3);
    ui_field(10, 11, text, focus == F_DEVICE);
}

static void leave(void)
{
    (void)mp_device_settings_save(); /* Flash only if changed */
}

const struct ui_page sysex_dump_page = {enter, update, leave, 0};
