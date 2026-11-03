/* Host stub of the declarations monitor.c, monitor_search.c and
 * rule_builder.c use.
 * Keep in step with src/app/midiproc.h. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
uint32_t fm1_millis(void);
#define MP_PORTS 3U
extern const char *const mp_port_names[MP_PORTS];
enum mp_fate { MP_PASSED, MP_REMAPPED, MP_BLOCKED, MP_TRIGGERED };
struct mp_event {
    uint8_t source, length;
    uint8_t data[3];
    uint8_t sysex_stored;
    uint8_t fate;
    uint16_t sysex_bytes;
    uint32_t stamp;
};
enum mp_remap_type { REMAP_NOTE, REMAP_CC, REMAP_PC, REMAP_TYPES };
#define FM1_MIDI_USB 1U
#define MP_MSG_RULES_PER_LIST 32U
#define MP_MSG_RULES_TOTAL 256U
struct mp_msg_rule {
    uint8_t port, channel, type;
    uint8_t in, out;
    uint8_t out_channel;
};
unsigned mp_msg_rule_count(void);
int mp_msg_rule_find(unsigned port, unsigned channel, unsigned type, unsigned in);
unsigned mp_msg_rules_in_list(unsigned port, unsigned channel, unsigned type);
int mp_msg_rule_save(const struct mp_msg_rule *rule, int replace);
enum mp_trig_type { TRIG_NON, TRIG_NOF, TRIG_CC, TRIG_PC, TRIG_TYPES };
#define MON_EVENTS 100U
void mon_init(void);
void mon_log(const struct mp_event *event);
void mon_log_sysex(uint8_t source, uint16_t bytes, uint8_t fate);
void mon_clock(void);
enum { MON_SEARCH_PORT, MON_SEARCH_CHANNEL, MON_SEARCH_TYPE, MON_SEARCH_D1, MON_SEARCH_D2, MON_SEARCHES };
#define MON_TYPES 16U
struct mon_search {
    uint8_t on[MON_SEARCHES];
    uint8_t ports;
    uint16_t channels;
    uint16_t types;
    uint8_t lo[2], hi[2];
    uint8_t exc[2];
};
extern struct mon_search mon_search;
extern const char *const mon_type_names[MON_TYPES];
int mon_search_active(void);
unsigned mp_format_note(char *out, uint8_t note);
unsigned mp_format_uint(char *out, uint32_t value);
unsigned mp_format_padded(char *out, uint32_t value, unsigned width);
#define UI_COLUMNS 20U
#define UI_ROWS 15U
enum {
    UI_BLACK = 0x0000, UI_WHITE = 0xef5d, UI_DIM = 0x738e, UI_PANEL = 0x18e3, UI_ACCENT = 0x07ff,
    UI_GREEN = 0x07e0, UI_AMBER = 0xfd20, UI_RED = 0xf800, UI_BLUE = 0x3c9f, UI_PURPLE = 0xac9f,
    UI_ORANGE = 0xfba5, UI_PINK = 0xed18, UI_DARK_YELLOW = 0x8c60, UI_DARK_GREY = 0x4a49, UI_TEAL = 0x4eae
};
void ui_clear(void);
void ui_print(unsigned row, unsigned column, const char *text, uint16_t color);
void ui_print_colors(unsigned row, unsigned column, const char *text, uint16_t color, uint16_t background);
void ui_clear_to_end(unsigned row, unsigned column);
void ui_header(const char *title, unsigned keep_from);
void ui_field(unsigned row, unsigned column, const char *text, int focused);
enum ui_button {
    BTN_OCT_DOWN, BTN_OCT_UP, BTN_FX, BTN_SEL, BTN_ENV, BTN_LFO, BTN_EDIT,
    BTN_GLO, BTN_HOME, BTN_SAVE, BTN_ARP, BTN_SEQ, BTN_PLAY, BTN_REC
};
#define UI_BUTTON(b) (1UL << (b))
enum ui_encoder { ENC_SELECT, ENC_ALGORITHM, ENC_KNOB1, ENC_KNOB2, ENC_KNOB3, ENC_KNOB4, ENC_PRESETS };
struct ui_input {
    uint32_t now, held, pressed, repeat, keys, keys_pressed, keys_released;
    int16_t encoders[7];
};
#define UI_KEYS 27U
#define UI_KEY_LOWEST_NOTE 53U
unsigned ui_white_key(unsigned n);
#define UI_LED_KEY(k) (14U + (k))
void ui_led(unsigned led, int on);
unsigned ui_focus_move(unsigned focus, unsigned fields, const struct ui_input *in);
int ui_step(const struct ui_input *in);
int ui_white_key_pressed(const struct ui_input *in);
struct ui_page {
    void (*enter)(void);
    void (*update)(const struct ui_input *in, int redraw);
    void (*leave)(void);
    const struct ui_page *parent;
};
void ui_show(const struct ui_page *page);
extern const struct ui_page monitor_page;
extern const struct ui_page mon_search_pages[MON_SEARCHES];
int mon_search_keys(const struct ui_input *in);
void mp_open_message_rule_for(unsigned port, unsigned channel, unsigned type, unsigned in);
void mp_open_channel_rule_for(unsigned port, unsigned channel);
void mp_new_trigger_for(unsigned port, unsigned channel, unsigned type, unsigned d1);
struct ui_confirm {
    uint8_t armed;
    uint32_t at;
};
int ui_confirm_press(struct ui_confirm *c, int pressed, uint32_t now);
int ui_confirm_led(struct ui_confirm *c, uint32_t now, int idle);
extern const struct ui_page rule_builder_page;
void mp_builder_set_in(unsigned port, unsigned channel, unsigned type, unsigned value);
void mp_builder_set_out(unsigned type, unsigned channel, unsigned value);
void ui_open_builder(void);
int ui_returning_from_builder(void);
