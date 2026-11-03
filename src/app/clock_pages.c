/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * Metronome (EDIT, after Knobs Config) and Clock Monitor (SEQ) pages.
 * Layouts: design/fm1-screens.json; behaviour: design/clock-and-metronome.md.
 *
 * Metronome
 *   row 0   METRONOME
 *   row 2   ENABLED:ON
 *   row 4   TIME BASE:1/4
 *   row 5   BEATS/BAR:4
 *   row 7   CLICK FREQ:  1000HZ
 *   row 8   1ST BEAT:    1500HZ
 *   row 10  VOLUME               the MASTER pot, read-only
 *   row 11  ==========------     its level as a bar
 *   row 12  CLOCK:USB RUN        dim, from the lockout
 *   row 13  BAR:0012 BEAT:3      dim
 *   row 14  REC:ON/OFF           hint
 * SELECT moves through the five fields, ALGORITHM and OCT-/OCT+ change the
 * value (frequencies: ALGORITHM 10 Hz, OCT 100 Hz). REC toggles ENABLED
 * from any field; its LED is lit while enabled. Settings are global and are
 * written to Flash when leaving the page, if they changed.
 *
 * Clock Monitor (read-only)
 *   row 2   SOURCE:USB
 *   row 3   STATE:RUN            green; STOP dim
 *   row 5   BPM
 *   row 6-7 120.0                4x; ---.- with no recent ticks
 *   row 9   ELAPSED
 *   row 10-11 01:23:45           4x
 * BPM is measured only while this page is shown.
 * Control task only. */
#include "midiproc.h"
#include "fm1_engine_control.h"
#include <string.h>

/* ---- Shared ------------------------------------------------------------------ */
static void clock_line(char *out) /* "USB RUN", "NONE STOP" */
{
    struct mp_clock_status s;
    mp_clock_status(&s);
    const char *src = s.owner < MP_PORTS ? mp_port_names[s.owner] : "NONE";
    strcpy(out, src);
    strcat(out, s.running ? " RUN " : " STOP");
}

/* ---- Metronome ----------------------------------------------------------------- */
enum { F_ENABLED, F_TIME_BASE, F_BEATS, F_CLICK, F_ACCENT, FIELDS };
static unsigned focus;
static const char *const time_base_names[MP_TIME_BASES] = {"1/4 ", "1/8 ", "1/16", "1/32"};

static void metronome_enter(void)
{
    ui_clear();
}

static void metronome_leave(void)
{
    (void)mp_global_settings_save();
}

static uint16_t step_hz(uint16_t hz, const struct ui_input *in)
{
    int d = in->encoders[ENC_ALGORITHM] * 10;
    if (in->repeat & UI_BUTTON(BTN_OCT_UP)) d += 100;
    if (in->repeat & UI_BUTTON(BTN_OCT_DOWN)) d -= 100;
    int v = (int)hz + d;
    if (v < (int)MP_CLICK_HZ_MIN) v = MP_CLICK_HZ_MIN;
    if (v > (int)MP_CLICK_HZ_MAX) v = MP_CLICK_HZ_MAX;
    return (uint16_t)v;
}

static void metronome_edit(const struct ui_input *in)
{
    struct mp_metronome *m = &mp_metronome;
    focus = ui_focus_move(focus, FIELDS, in);
    int d = ui_step(in);
    switch (focus) {
    case F_ENABLED:
        if (d % 2) m->enabled ^= 1U;
        break;
    case F_TIME_BASE:
        if (d) m->time_base = (uint8_t)(((int)m->time_base + d % MP_TIME_BASES + MP_TIME_BASES) % MP_TIME_BASES);
        break;
    case F_BEATS: {
        int b = (int)m->beats + d;
        m->beats = (uint8_t)(b < 1 ? 1 : b > 16 ? 16 : b);
        break;
    }
    case F_CLICK: m->click_hz = step_hz(m->click_hz, in); break;
    default: m->accent_hz = step_hz(m->accent_hz, in); break;
    }
    if (in->pressed & UI_BUTTON(BTN_REC)) m->enabled ^= 1U;
    ui_led(BTN_REC, m->enabled);
    ui_led(BTN_OCT_DOWN, 1);
    ui_led(BTN_OCT_UP, 1);
}

static void hz_text(char *out, unsigned hz) /* " 200HZ" .. "4000HZ" */
{
    char digits[11];
    unsigned n = mp_format_uint(digits, hz);
    unsigned pad = n < 4U ? 4U - n : 0U;
    memset(out, ' ', pad);
    memcpy(out + pad, digits, n);
    memcpy(out + pad + n, "HZ", 3);
}

static void metronome_update(const struct ui_input *in, int redraw)
{
    metronome_edit(in);
    if (!redraw) return;
    const struct mp_metronome *m = &mp_metronome;
    char text[24];

    ui_header("METRONOME", UI_COLUMNS);
    ui_print(2, 0, "ENABLED:", UI_ORANGE);
    ui_field(2, 8, m->enabled ? "ON " : "OFF", focus == F_ENABLED);
    ui_print(4, 0, "TIME BASE:", UI_ORANGE);
    ui_field(4, 10, time_base_names[m->time_base % MP_TIME_BASES], focus == F_TIME_BASE);
    ui_print(5, 0, "BEATS/BAR:", UI_ORANGE);
    mp_format_uint(text, m->beats);
    if (m->beats < 10U) strcat(text, " ");
    ui_field(5, 10, text, focus == F_BEATS);
    ui_print(7, 0, "CLICK FREQ:", UI_ORANGE);
    hz_text(text, m->click_hz);
    ui_field(7, 13, text, focus == F_CLICK);
    ui_print(8, 0, "1ST BEAT:", UI_ORANGE);
    hz_text(text, m->accent_hz);
    ui_field(8, 13, text, focus == F_ACCENT);

    ui_print(10, 0, "VOLUME", UI_WHITE);
    unsigned level = fm1_engine_output_level();
    if (level > 127U) level = 127U;
    ui_row_bar(11, level * FM1_SCREEN_WIDTH / 127U, UI_ACCENT, UI_PANEL);

    strcpy(text, "CLOCK:");
    clock_line(text + 6);
    ui_print(12, 0, text, UI_DIM);
    ui_clear_to_end(12, (unsigned)strlen(text));

    unsigned bar, beat;
    if (mp_metronome_position(&bar, &beat)) {
        strcpy(text, "BAR:");
        mp_format_padded(text + 4, bar > 9999U ? 9999U : bar, 4);
        strcat(text, " BEAT:");
        mp_format_uint(text + strlen(text), beat);
    } else {
        strcpy(text, "BAR:---- BEAT:-");
    }
    ui_print(13, 0, text, UI_DIM);
    ui_clear_to_end(13, (unsigned)strlen(text));
    ui_print(14, 0, "REC:ON/OFF", UI_DIM);
}

const struct ui_page metronome_page = {metronome_enter, metronome_update, metronome_leave, 0};

/* ---- Clock Monitor --------------------------------------------------------------- */
static void monitor_enter(void)
{
    ui_clear();
    mp_clock_bpm_enable(1);
}

static void monitor_leave(void)
{
    mp_clock_bpm_enable(0);
}

static void monitor_update(const struct ui_input *in, int redraw)
{
    (void)in;
    if (!redraw) return;
    struct mp_clock_status s;
    mp_clock_status(&s);
    char text[16];

    ui_header("CLOCK MONITOR", UI_COLUMNS);
    ui_print(2, 0, "SOURCE:", UI_ORANGE);
    ui_print(2, 7, s.owner < MP_PORTS ? mp_port_names[s.owner] : "NONE", UI_WHITE);
    ui_clear_to_end(2, s.owner < MP_PORTS ? 10U : 11U);
    ui_print(3, 0, "STATE:", UI_ORANGE);
    ui_print(3, 7, s.running ? "RUN " : "STOP", s.running ? UI_GREEN : UI_DIM);

    ui_print(5, 0, "BPM", UI_ORANGE);
    int bpm10 = mp_clock_bpm10();
    if (bpm10 < 0) {
        strcpy(text, "---.-");
    } else {
        unsigned n = mp_format_uint(text, (unsigned)bpm10 / 10U);
        text[n] = '.';
        text[n + 1U] = (char)('0' + bpm10 % 10);
        text[n + 2U] = 0;
    }
    ui_big(6, text, UI_WHITE);

    ui_print(9, 0, "ELAPSED", UI_ORANGE);
    uint32_t secs = s.elapsed_ms / 1000U;
    if (secs > 99U * 3600U + 59U * 60U + 59U) secs = 99U * 3600U + 59U * 60U + 59U;
    mp_format_padded(text, secs / 3600U, 2);
    text[2] = ':';
    mp_format_padded(text + 3, secs / 60U % 60U, 2);
    text[5] = ':';
    mp_format_padded(text + 6, secs % 60U, 2);
    ui_big(10, text, UI_WHITE);
}

const struct ui_page clock_monitor_page = {monitor_enter, monitor_update, monitor_leave, 0};
