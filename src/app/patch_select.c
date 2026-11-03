/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * Patch Select page (SAVE). Layout: design/fm1-screens.json, "Patch Select";
 * answers: design/open-questions.md 22-23, design/patch-format.md.
 *
 *   row 0   PATCH SELECT*            * = the edit buffer has unsaved changes
 *   row 2   PATCH NUMBER:16          the slot PLAY loads from / REC saves to
 *   row 3   STORED:MY PATCH 16       that slot's stored name, or EMPTY
 *   row 5   NAME:MY PATCH 16         the edit buffer's name, saved with it
 *   row 7   IN USE:03  UNSAVED       the patch the edit buffer came from
 *   row 12  status (saved, loaded, errors, REC AGAIN TO SAVE)
 *   row 14  PLAY LOAD  REC SAVE
 *
 * Controls: SELECT steps PATCH NUMBER -> each name character -> back.
 * ALGORITHM and OCT-/OCT+ change the patch number, or the character under
 * the cursor (space, A-Z, 0-9, dash). The lower 16 white keys pick patch
 * 1-16, except while the name is focused. PLAY loads the selected patch
 * (unsaved changes are dropped); REC twice within 5 s saves the edit buffer
 * to it. Changing the number only browses: nothing loads until PLAY.
 * Control task only. */
#include "midiproc.h"
#include <string.h>

enum { F_NUMBER, F_NAME, FIELDS = F_NAME + MP_PATCH_NAME };
enum { STATUS_ROW = 12 };
static const char charset[] = " ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-";

static unsigned focus, selected;
static int dirty;
static struct ui_confirm save;
static enum { ST_NONE, ST_LOADED, ST_SAVED, ST_ERROR } status;
static int status_code;

static int any_control(const struct ui_input *in)
{
    if (in->keys_pressed || (in->pressed & ~UI_BUTTON(BTN_REC))) return 1;
    for (unsigned e = 0; e < 7U; ++e)
        if (in->encoders[e] && (e < ENC_KNOB1 || e > ENC_KNOB4)) return 1;
    return 0;
}

static void enter(void)
{
    ui_clear();
    focus = F_NUMBER;
    selected = mp_patch_current();
    dirty = mp_patch_dirty();
    save.armed = 0;
    status = ST_NONE;
}

static void step_char(unsigned at, int d)
{
    int i = 0, n = (int)sizeof charset - 1;
    while (i < n && charset[i] != mp_patch_name[at]) ++i;
    if (i == n) i = 0;
    mp_patch_name[at] = charset[((i + d) % n + n) % n];
}

static void set_status(int result, int ok_status)
{
    status = result == 0 ? ok_status : ST_ERROR;
    status_code = result;
}

static unsigned seen_generation;

static void edit(const struct ui_input *in)
{
    if (seen_generation != mp_patch_generation()) { /* replaced by a SysEx dump */
        seen_generation = mp_patch_generation();
        dirty = mp_patch_dirty();
    }
    if (any_control(in)) {
        save.armed = 0;
        if (in->repeat || in->keys_pressed || in->encoders[ENC_SELECT] || in->encoders[ENC_ALGORITHM])
            status = ST_NONE;
    }
    focus = ui_focus_move(focus, FIELDS, in);
    int d = ui_step(in);
    if (focus == F_NUMBER) {
        if (d) selected = (unsigned)(((int)selected + d % (int)MP_PATCHES + (int)MP_PATCHES) % (int)MP_PATCHES);
        int key = ui_white_key_pressed(in);
        if (key >= 0) selected = (unsigned)key;
    } else if (d) {
        step_char(focus - F_NAME, d);
        dirty = mp_patch_dirty();
    }

    if (in->pressed & UI_BUTTON(BTN_PLAY)) {
        set_status(mp_patch_load(selected), ST_LOADED);
        dirty = mp_patch_dirty();
    }
    if (ui_confirm_press(&save, (in->pressed & UI_BUTTON(BTN_REC)) != 0, in->now)) {
        set_status(mp_patch_save(selected), ST_SAVED);
        dirty = mp_patch_dirty();
    }
}

static void leds(const struct ui_input *in)
{
    unsigned lit = focus == F_NUMBER ? ui_white_key(selected) : UI_KEYS;
    for (unsigned k = 0; k < UI_KEYS; ++k) ui_led(UI_LED_KEY(k), k == lit);
    ui_led(BTN_OCT_DOWN, 1);
    ui_led(BTN_OCT_UP, 1);
    ui_led(BTN_PLAY, 1);
    ui_led(BTN_REC, ui_confirm_led(&save, in->now, 1));
}

static void draw_status(void)
{
    char line[UI_COLUMNS + 1], digits[11];
    unsigned n = 0;
    const char *text = 0;
    uint16_t color = UI_GREEN;
    if (save.armed) {
        ui_print(STATUS_ROW, 0, "REC AGAIN TO SAVE   ", UI_DIM);
        return;
    }
    switch (status) {
    case ST_LOADED: text = "LOADED "; break;
    case ST_SAVED: text = "SAVED TO "; break;
    case ST_ERROR:
        text = status_code == FM1_FLASH_UNAVAILABLE ? "NO STORAGE" : "FLASH ERROR ";
        color = UI_RED;
        break;
    default: ui_clear_to_end(STATUS_ROW, 0); return;
    }
    while (*text) line[n++] = *text++;
    if (status == ST_ERROR) {
        if (status_code != FM1_FLASH_UNAVAILABLE) {
            line[n++] = '-';
            n += mp_format_uint(line + n, (uint32_t)-status_code);
        }
    } else {
        n += mp_format_padded(line + n, selected + 1U, 2);
        line[n++] = ' ';
        line[n++] = '(';
        unsigned d = mp_format_uint(digits, mp_patch_write_ms());
        memcpy(line + n, digits, d);
        n += d;
        memcpy(line + n, "MS)", 3);
        n += 3;
    }
    while (n < UI_COLUMNS) line[n++] = ' ';
    line[UI_COLUMNS] = 0;
    ui_print(STATUS_ROW, 0, line, color);
}

static void update(const struct ui_input *in, int redraw)
{
    edit(in);
    leds(in);
    if (!redraw) return;

    ui_header(dirty ? "PATCH SELECT*" : "PATCH SELECT", UI_COLUMNS);
    char text[UI_COLUMNS + 1];
    ui_print(2, 0, "PATCH NUMBER:", UI_ORANGE);
    mp_format_padded(text, selected + 1U, 2);
    ui_field(2, 13, text, focus == F_NUMBER);

    ui_print(3, 0, "STORED:", UI_DIM);
    const char *stored = mp_patch_stored_name(selected);
    if (stored) {
        memcpy(text, stored, MP_PATCH_NAME);
        text[MP_PATCH_NAME] = 0;
        ui_print(3, 7, text, UI_WHITE);
    } else {
        ui_print(3, 7, "EMPTY       ", UI_DIM);
    }

    ui_print(5, 0, "NAME:", UI_ORANGE);
    for (unsigned i = 0; i < MP_PATCH_NAME; ++i) {
        char c[2] = {mp_patch_name[i], 0};
        ui_field(5, 6 + i, c, focus == F_NAME + i);
    }

    ui_print(7, 0, "IN USE:", UI_DIM);
    mp_format_padded(text, mp_patch_current() + 1U, 2);
    ui_print(7, 7, text, UI_WHITE);
    ui_print(7, 11, dirty ? "UNSAVED" : "       ", UI_AMBER);

    draw_status();
    ui_print(14, 0, "PLAY LOAD  REC SAVE", UI_DIM);
}

const struct ui_page patch_select_page = {enter, update, 0, 0};
