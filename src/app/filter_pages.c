/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * Channel Filter and Message Filter pages, the first two pages of the FX
 * cycle. Layouts: design/fm1-screens.json; answers: design/open-questions.md
 * 6-9. The filters themselves are in processor.c (struct mp_filters).
 *
 * Both pages edit one input port at a time (IN PORT, cycling TRS -> USB ->
 * BLE); the selected port is shared, so moving between the pages keeps it.
 * SELECT moves the focus; ALGORITHM and OCT-/OCT+ change the focused field.
 * An OK/NO field flips on each step. Control task only. */
#include "midiproc.h"

static unsigned port;        /* IN PORT, shared by both pages */
static unsigned effective;   /* Message Filter: channel 0..15 */

static void port_field(unsigned focused)
{
    ui_print(1, 0, "IN PORT:", UI_ORANGE);
    ui_field(1, 8, mp_port_names[port], focused);
}

static void edit_port(int d)
{
    if (d) port = (unsigned)(((int)port + d % (int)MP_PORTS + (int)MP_PORTS) % (int)MP_PORTS);
}

/* Flip `bit` in *mask once per odd number of steps. */
static void toggle16(uint16_t *mask, unsigned bit, int d)
{
    if (d % 2) *mask ^= (uint16_t)(1U << bit);
}

static void ok_no(unsigned row, unsigned column, int pass, int focused)
{
    ui_field(row, column, pass ? "OK" : "NO", focused);
}

static void oct_leds(void)
{
    ui_led(BTN_OCT_DOWN, 1);
    ui_led(BTN_OCT_UP, 1);
}

/* ---- Channel Filter ----------------------------------------------------------
 *   row 1     IN PORT:TRS
 *   rows 3-10 01: OK      09: NO     channels 1-8 left, 9-16 right
 * Fields: IN PORT, then channels 1-16. The lower 16 white keys toggle their
 * channel from any field, and are lit while their channel passes. */
static unsigned cf_focus; /* 0 port, 1..16 channel */

static void cf_enter(void)
{
    ui_clear();
}

static void cf_update(const struct ui_input *in, int redraw)
{
    cf_focus = ui_focus_move(cf_focus, 17, in);
    int d = ui_step(in);
    if (cf_focus == 0) edit_port(d);
    else toggle16(&mp_filters.channels[port], cf_focus - 1U, d);
    int key = ui_white_key_pressed(in);
    if (key >= 0) mp_filters.channels[port] ^= (uint16_t)(1U << key);

    uint16_t pass = mp_filters.channels[port];
    for (unsigned n = 0; n < 16U; ++n) ui_led(UI_LED_KEY(ui_white_key(n)), (pass >> n) & 1U);
    oct_leds();
    if (!redraw) return;

    ui_header("CHANNEL FILTER", UI_COLUMNS);
    port_field(cf_focus == 0);
    for (unsigned n = 0; n < 16U; ++n) {
        unsigned row = 3U + n % 8U, column = n < 8U ? 0U : 12U;
        char label[4];
        mp_format_padded(label, n + 1U, 2);
        label[2] = ':';
        label[3] = 0;
        ui_print(row, column, label, UI_WHITE);
        ok_no(row, column + 4U, (pass >> n) & 1U, cf_focus == n + 1U);
    }
}

const struct ui_page channel_filter_page = {cf_enter, cf_update, 0};

/* ---- Message Filter ----------------------------------------------------------
 *   row 1     IN PORT:TRS
 *   rows 3-7  SYX: NO    SS: NO      system types, per port
 *   row 9     EFFECTIVE CH:03
 *   rows 11-14 NON: OK   PAT: NO     channel types, per port and channel
 * Fields in order: IN PORT, the system types (left column, then right),
 * EFFECTIVE CH, the channel types (left, then right). The lower 16 white
 * keys pick the effective channel from any field; its key is lit. */
static const struct {
    const char *label;
    uint8_t row, column; /* label position; the value is at 15 or 5 */
} system_labels[SYS_TYPES] = {
    [SYS_SYX] = {"SYX:", 3, 0}, [SYS_MTC] = {"MTC:", 4, 0}, [SYS_SPP] = {"SPP:", 5, 0},
    [SYS_STA] = {"STA:", 6, 0}, [SYS_CON] = {"CON:", 7, 0},
    [SYS_SS] = {"SS:", 3, 11},  [SYS_TR] = {"TR:", 4, 11},  [SYS_RST] = {"RST:", 5, 10},
    [SYS_STP] = {"STP:", 6, 10}, [SYS_TCK] = {"TCK:", 7, 10},
}, voice_labels[VOICE_TYPES] = {
    [VOICE_NON] = {"NON:", 11, 0}, [VOICE_NOF] = {"NOF:", 12, 0}, [VOICE_CC] = {"CC:", 13, 1},
    [VOICE_PC] = {"PC:", 14, 1},   [VOICE_PAT] = {"PAT:", 11, 10}, [VOICE_CAT] = {"CAT:", 12, 10},
    [VOICE_PB] = {"PB:", 13, 11},
};
enum {
    MF_PORT = 0,
    MF_SYSTEM = 1,                      /* + enum mp_system_type */
    MF_CHANNEL = MF_SYSTEM + SYS_TYPES, /* EFFECTIVE CH */
    MF_VOICE,                           /* + enum mp_voice_type */
    MF_FIELDS = MF_VOICE + VOICE_TYPES
};
static unsigned mf_focus;

static void mf_enter(void)
{
    ui_clear();
}

static void mf_update(const struct ui_input *in, int redraw)
{
    mf_focus = ui_focus_move(mf_focus, MF_FIELDS, in);
    int d = ui_step(in);
    if (mf_focus == MF_PORT) {
        edit_port(d);
    } else if (mf_focus < MF_CHANNEL) {
        toggle16(&mp_filters.system[port], mf_focus - MF_SYSTEM, d);
    } else if (mf_focus == MF_CHANNEL) {
        if (d) effective = (unsigned)(((int)effective + d % 16 + 16) % 16);
    } else if (d % 2) {
        mp_filters.voice[port][effective] ^= (uint8_t)(1U << (mf_focus - MF_VOICE));
    }
    int key = ui_white_key_pressed(in);
    if (key >= 0) effective = (unsigned)key;

    unsigned lit = ui_white_key(effective);
    for (unsigned k = 0; k < UI_KEYS; ++k) ui_led(UI_LED_KEY(k), k == lit);
    oct_leds();
    if (!redraw) return;

    ui_header("MESSAGE FILTER", UI_COLUMNS);
    port_field(mf_focus == MF_PORT);
    for (unsigned t = 0; t < SYS_TYPES; ++t) {
        ui_print(system_labels[t].row, system_labels[t].column, system_labels[t].label, UI_WHITE);
        ok_no(system_labels[t].row, system_labels[t].column < 10U ? 5U : 15U,
              (mp_filters.system[port] >> t) & 1U, mf_focus == MF_SYSTEM + t);
    }
    ui_print(9, 0, "EFFECTIVE CH:", UI_ORANGE);
    char text[4];
    mp_format_padded(text, effective + 1U, 2);
    ui_field(9, 13, text, mf_focus == MF_CHANNEL);
    for (unsigned t = 0; t < VOICE_TYPES; ++t) {
        ui_print(voice_labels[t].row, voice_labels[t].column, voice_labels[t].label, UI_WHITE);
        ok_no(voice_labels[t].row, voice_labels[t].column < 10U ? 5U : 15U,
              (mp_filters.voice[port][effective] >> t) & 1U, mf_focus == MF_VOICE + t);
    }
}

const struct ui_page message_filter_page = {mf_enter, mf_update, 0};
