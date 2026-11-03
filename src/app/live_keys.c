/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * Live Keys page: play the FM-1's keys as a MIDI controller. Layout:
 * design/fm1-screens.json, "Live keys"; answers: design/open-questions.md
 * 14-16.
 *
 *   row 0   LIVE KEYS
 *   row 2   CH:01
 *   row 4   VELOCITY
 *   row 5   MIN:100    MAX:127
 *   row 7   OCTAVE:+1             pink: set with OCT-/OCT+, not SELECT
 *   row 9   PROGRAM CHANGE:001    shown 1-128, sent 0-127
 *   row 11  BANK SELECT
 *   row 12  MSB:OFF    LSB:OFF
 *
 * Keys play notes on the channel with a random velocity from MIN to MAX,
 * straight to the output (not through the pipeline). At octave 0 the lowest
 * key is F3 (note 53); OCT-/OCT+ shift by 12 within -3..+3, and their LEDs
 * are lit on this page. REC sends Bank Select MSB (CC 0) and LSB (CC 32)
 * when set, then Program Change. SELECT moves the focus, ALGORITHM changes
 * the focused value. ENV and LFO put the last message sent (a note, or the
 * Program Change) into Build Message Rule's IN or OUT
 * (design/rule-builder.md). Control task only. */
#include "midiproc.h"

enum { F_CHANNEL, F_MIN, F_MAX, F_PROGRAM, F_MSB, F_LSB, FIELDS };
enum { OFF = MP_BANK_OFF, OCTAVE_RANGE = 3, REC_FLASH_MS = 150 };

struct mp_live_keys mp_live_keys = {0, 100, 127, 0, 0, OFF, OFF};

void live_keys_defaults(void)
{
    mp_live_keys = (struct mp_live_keys){0, 100, 127, 0, 0, OFF, OFF};
}

static unsigned focus;
static uint8_t sounding[UI_KEYS];  /* note + 1 while a key's note is on, else 0 */
static uint8_t sounding_ch[UI_KEYS];
static uint32_t random_state, rec_flash_at;
static uint8_t rec_flash;
/* The last message sent, for the rule builder. */
static struct {
    uint8_t valid, type, channel, value; /* type: enum mp_remap_type */
} last;

static unsigned random_velocity(void)
{
    if (!random_state) random_state = mp_time_us() | 1U;
    random_state ^= random_state << 13;
    random_state ^= random_state >> 17;
    random_state ^= random_state << 5;
    return mp_live_keys.vel_min + random_state % (mp_live_keys.vel_max - mp_live_keys.vel_min + 1U);
}

static void send3(uint8_t status, uint8_t d1, uint8_t d2)
{
    uint8_t m[3] = {status, d1, d2};
    mp_send_local(m, 3);
}

static void note_off(unsigned key)
{
    if (!sounding[key]) return;
    send3((uint8_t)(0x80U | sounding_ch[key]), (uint8_t)(sounding[key] - 1U), 0);
    sounding[key] = 0;
}

/* Keys first, before any editing or drawing, so notes go out at once. */
static void play(const struct ui_input *in)
{
    for (unsigned key = 0; key < UI_KEYS; ++key) {
        uint32_t bit = 1UL << key;
        if (in->keys_released & bit) note_off(key);
        if ((in->keys_pressed & bit) && !sounding[key]) {
            unsigned note = UI_KEY_LOWEST_NOTE + key + 12 * mp_live_keys.octave; /* 17..115 */
            send3((uint8_t)(0x90U | mp_live_keys.channel), (uint8_t)note, (uint8_t)random_velocity());
            sounding[key] = (uint8_t)(note + 1U);
            sounding_ch[key] = mp_live_keys.channel;
            last.valid = 1;
            last.type = REMAP_NOTE;
            last.channel = mp_live_keys.channel;
            last.value = (uint8_t)note;
        }
    }
    if (in->pressed & UI_BUTTON(BTN_REC)) {
        if (mp_live_keys.msb != OFF) send3((uint8_t)(0xb0U | mp_live_keys.channel), 0, (uint8_t)mp_live_keys.msb);
        if (mp_live_keys.lsb != OFF) send3((uint8_t)(0xb0U | mp_live_keys.channel), 32, (uint8_t)mp_live_keys.lsb);
        uint8_t pc[2] = {(uint8_t)(0xc0U | mp_live_keys.channel), mp_live_keys.program};
        mp_send_local(pc, 2);
        last.valid = 1;
        last.type = REMAP_PC;
        last.channel = mp_live_keys.channel;
        last.value = mp_live_keys.program;
        rec_flash = 1;
        rec_flash_at = in->now;
    }
}

static int clamp(int v, int lo, int hi)
{
    return v < lo ? lo : v > hi ? hi : v;
}

static void edit(const struct ui_input *in)
{
    if (in->pressed & UI_BUTTON(BTN_OCT_UP) && mp_live_keys.octave < OCTAVE_RANGE) ++mp_live_keys.octave;
    if (in->pressed & UI_BUTTON(BTN_OCT_DOWN) && mp_live_keys.octave > -OCTAVE_RANGE) --mp_live_keys.octave;

    int select = in->encoders[ENC_SELECT];
    if (select) focus = (unsigned)(((int)focus + select % FIELDS + FIELDS) % FIELDS);
    int d = in->encoders[ENC_ALGORITHM];
    if (!d) return;
    switch (focus) {
    case F_CHANNEL: mp_live_keys.channel = (uint8_t)(((int)mp_live_keys.channel + d % 16 + 16) % 16); break;
    case F_MIN:
        mp_live_keys.vel_min = (uint8_t)clamp(mp_live_keys.vel_min + d, 1, 127);
        if (mp_live_keys.vel_max < mp_live_keys.vel_min) mp_live_keys.vel_max = mp_live_keys.vel_min;
        break;
    case F_MAX:
        mp_live_keys.vel_max = (uint8_t)clamp(mp_live_keys.vel_max + d, 1, 127);
        if (mp_live_keys.vel_min > mp_live_keys.vel_max) mp_live_keys.vel_min = mp_live_keys.vel_max;
        break;
    case F_PROGRAM: mp_live_keys.program = (uint8_t)clamp(mp_live_keys.program + d, 0, 127); break;
    case F_MSB: mp_live_keys.msb = (int8_t)clamp(mp_live_keys.msb + d, OFF, 127); break;
    case F_LSB: mp_live_keys.lsb = (int8_t)clamp(mp_live_keys.lsb + d, OFF, 127); break;
    default: break;
    }
}

static void leds(const struct ui_input *in)
{
    for (unsigned key = 0; key < UI_KEYS; ++key) ui_led(UI_LED_KEY(key), (in->keys >> key) & 1U);
    ui_led(BTN_OCT_DOWN, 1);
    ui_led(BTN_OCT_UP, 1);
    if (rec_flash && in->now - rec_flash_at >= REC_FLASH_MS) rec_flash = 0;
    ui_led(BTN_REC, !rec_flash); /* lit; blinks off when sent */
}

static void field_number(unsigned row, unsigned column, int value, unsigned width, unsigned f)
{
    char text[12];
    if (value == OFF) {
        text[0] = 'O'; text[1] = 'F'; text[2] = 'F'; text[3] = 0;
    } else {
        mp_format_padded(text, (uint32_t)value, width);
    }
    ui_field(row, column, text, focus == f);
}

static void enter(void)
{
    ui_clear();
}

/* ENV = IN, LFO = OUT. Returns 1 if the builder was opened. */
static int build(const struct ui_input *in)
{
    ui_led(BTN_ENV, last.valid);
    ui_led(BTN_LFO, last.valid);
    if (!last.valid) return 0;
    if (in->pressed & UI_BUTTON(BTN_ENV))
        mp_builder_set_in(FM1_MIDI_USB, last.channel, last.type, last.value);
    else if (in->pressed & UI_BUTTON(BTN_LFO))
        mp_builder_set_out(last.type, last.channel, last.value);
    else
        return 0;
    ui_open_builder();
    return 1;
}

static void update(const struct ui_input *in, int redraw)
{
    play(in);
    if (build(in)) return;
    edit(in);
    leds(in);
    if (!redraw) return;

    ui_header("LIVE KEYS", UI_COLUMNS);
    ui_print(2, 0, "CH:", UI_ORANGE);
    field_number(2, 3, mp_live_keys.channel + 1, 2, F_CHANNEL);

    ui_print(4, 0, "VELOCITY", UI_WHITE);
    ui_print(5, 0, "MIN:", UI_ORANGE);
    field_number(5, 4, mp_live_keys.vel_min, 3, F_MIN);
    ui_print(5, 11, "MAX:", UI_ORANGE);
    field_number(5, 15, mp_live_keys.vel_max, 3, F_MAX);

    ui_print(7, 0, "OCTAVE:", UI_WHITE);
    char octave[3] = {mp_live_keys.octave > 0 ? '+' : mp_live_keys.octave < 0 ? '-' : '0',
                      mp_live_keys.octave ? (char)('0' + (mp_live_keys.octave < 0 ? -mp_live_keys.octave : mp_live_keys.octave)) : ' ', 0};
    ui_print(7, 7, octave, UI_PINK);

    ui_print(9, 0, "PROGRAM CHANGE:", UI_ORANGE);
    field_number(9, 15, mp_live_keys.program + 1, 3, F_PROGRAM);

    ui_print(11, 0, "BANK SELECT", UI_WHITE);
    ui_print(12, 0, "MSB:", UI_ORANGE);
    field_number(12, 4, mp_live_keys.msb, 3, F_MSB);
    ui_print(12, 11, "LSB:", UI_ORANGE);
    field_number(12, 15, mp_live_keys.lsb, 3, F_LSB);
}

/* Leaving with keys held: end their notes so nothing hangs. */
static void leave(void)
{
    for (unsigned key = 0; key < UI_KEYS; ++key) note_off(key);
}

const struct ui_page live_keys_page = {enter, update, leave};
