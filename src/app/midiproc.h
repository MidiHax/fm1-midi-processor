/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * FM-1 MIDI processor: shared declarations. */
#ifndef MIDIPROC_H
#define MIDIPROC_H
#include <stddef.h>
#include <stdint.h>
#include "fm1.h"

/* ------------------------------------------------------------------------
 * MIDI events
 * ------------------------------------------------------------------------ */
/* One complete message. source is FM1_MIDI_DIN (TRS), FM1_MIDI_USB or
 * FM1_MIDI_BLE. A short message has length 1..3. A SysEx frame has length 0,
 * data[0] = F0, its total size in sysex_bytes, and sysex_stored set when its
 * bytes were kept for forwarding (at most MP_SYSEX_FORWARD_MAX). */
/* What the pipeline did with an event, recorded once for the monitor. */
enum mp_fate { MP_PASSED, MP_REMAPPED, MP_BLOCKED, MP_TRIGGERED };
struct mp_event {
    uint8_t source, length;
    uint8_t data[3];
    uint8_t sysex_stored;
    uint8_t fate;       /* enum mp_fate, set by mp_process */
    uint16_t sysex_bytes;
    uint32_t stamp;     /* mp_time_us() on arrival (routing task) */
};
/* fm1_midi_send_port accepts at most 128 bytes per message, so longer SysEx
 * is shown in the monitor but not forwarded. */
#define MP_SYSEX_FORWARD_MAX 128U
/* Bytes in a short message with this status (1..3). */
unsigned mp_message_length(uint8_t status);

/* ------------------------------------------------------------------------
 * Pipeline (processor.c). Control task only.
 * ------------------------------------------------------------------------ */
/* Output-queue room needed before processing one input: the most
 * mp_process may emit for it (more while event triggers exist). */
unsigned mp_emit_reserve(void);
void mp_process_init(void);
/* Process one received message; call mp_emit / mp_emit_sysex for output.
 * Sets in->fate (blocked by a filter, or a remap rule matched), so the
 * monitor can colour it without a second lookup. */
void mp_process(struct mp_event *in);

/* Filters (processor.c), per input port: the Channel Filter, then the
 * Message Filter's system types (per port) and channel types (per port and
 * channel). A set bit passes. Control task only. */
enum mp_system_type {
    SYS_SYX, SYS_MTC, SYS_SPP, SYS_STA, SYS_CON, /* left column on screen */
    SYS_SS, SYS_TR, SYS_RST, SYS_STP, SYS_TCK,   /* right column */
    SYS_TYPES
};
enum mp_voice_type {
    VOICE_NON, VOICE_NOF, VOICE_CC, VOICE_PC,    /* left column */
    VOICE_PAT, VOICE_CAT, VOICE_PB,              /* right column */
    VOICE_TYPES
};
#define MP_PORTS 3U
struct mp_filters {
    uint16_t channels[MP_PORTS];   /* bit n: channel n+1 passes */
    uint16_t system[MP_PORTS];     /* bit = enum mp_system_type */
    uint8_t voice[MP_PORTS][16];   /* bit = enum mp_voice_type */
};
extern struct mp_filters mp_filters;
extern const char *const mp_port_names[MP_PORTS]; /* "TRS", "USB", "BLE" */

/* Remaps (remap.c). Control task only.
 * Message rules change data 1 and set the output channel; a NOTE rule covers
 * Note On, Note Off and Poly Aftertouch. Channel rules move a port's channel
 * (after any message rule). */
enum mp_remap_type { REMAP_NOTE, REMAP_CC, REMAP_PC, REMAP_TYPES };
#define MP_MSG_RULES_PER_LIST 32U  /* per port, channel and type */
#define MP_MSG_RULES_TOTAL 256U
#define MP_CH_RULES 32U
struct mp_msg_rule {
    uint8_t port, channel, type; /* channel 0..15, type enum mp_remap_type */
    uint8_t in, out;             /* data 1, 0..127 */
    uint8_t out_channel;         /* 0..15 */
};
struct mp_ch_rule {
    uint8_t port, in, out;       /* channels 0..15 */
};
void mp_remap_init(void);
/* Apply both remap stages to a short channel message in place. Returns
 * nonzero if a message rule or a channel rule matched. */
int mp_remap(unsigned port, uint8_t message[3]);
unsigned mp_msg_rule_count(void);
const struct mp_msg_rule *mp_msg_rule(unsigned index); /* pool order */
int mp_msg_rule_find(unsigned port, unsigned channel, unsigned type, unsigned in);
/* Pool indexes of one list's rules, sorted by input value; returns count. */
unsigned mp_msg_rule_list(unsigned port, unsigned channel, unsigned type,
                          uint8_t index[MP_MSG_RULES_PER_LIST]);
unsigned mp_msg_rules_in_list(unsigned port, unsigned channel, unsigned type);
/* Save a rule, replacing rule `replace` (or -1) and any rule with the same
 * input. Returns 0, or -1 if the list or pool is full or the rule invalid.
 * Indexes change after a save or delete. */
int mp_msg_rule_save(const struct mp_msg_rule *rule, int replace);
void mp_msg_rule_delete(unsigned index);
unsigned mp_ch_rule_count(void);
const struct mp_ch_rule *mp_ch_rule(unsigned index); /* sorted by port, input */
int mp_ch_rule_find(unsigned port, unsigned in);
int mp_ch_rule_save(const struct mp_ch_rule *rule, int replace);
void mp_ch_rule_delete(unsigned index);

/* Event triggers (triggers.c), design/event-triggers.md. Control task only.
 * A trigger matches input after the filters (before the remaps) and sends
 * its message list straight to the output, unfiltered and unremapped.
 * Messages are kept in one pool, grouped by trigger in list order. */
enum mp_trig_type { TRIG_NON, TRIG_NOF, TRIG_CC, TRIG_PC, TRIG_TYPES };
#define MP_TRIGGERS 32U
#define MP_TRIG_MSGS 32U          /* per trigger */
#define MP_TRIG_MSGS_TOTAL 256U
#define MP_TRIG_EMIT_MAX 64U      /* trigger messages sent for one input */
#define MP_TRIG_IN 0x80U          /* message d1/d2: copy the input's value */
#define TRIG_PASS 1U              /* flags: pass the triggering message on */
#define TRIG_ENTER 2U             /* flags: CC fires on entering the D2 range */
struct mp_trigger {
    uint8_t port, channel, type;  /* channel 0..15, type enum mp_trig_type */
    uint8_t d1_lo, d1_hi, d2_lo, d2_hi; /* inclusive; D2 unused for PC */
    uint8_t flags;
    uint8_t count;                /* messages */
    uint8_t last_d2;              /* ENTER: last value seen, 0xff none */
    uint16_t first;               /* pool index of its first message */
};
struct mp_trig_msg {
    uint8_t type, channel, d1, d2; /* d1/d2 0..127 or MP_TRIG_IN; PC d2 = 0 */
};
void mp_triggers_init(void);
/* Pipeline: triggers matching a filtered input message (bit = index), with
 * ENTER state updated. *pass is cleared if one of them stops the input. */
uint32_t mp_trigger_match(unsigned port, const uint8_t message[3], int *pass);
/* Emit the messages of `fired` for input `message`, at most
 * MP_TRIG_EMIT_MAX in all. */
void mp_trigger_send(uint32_t fired, const uint8_t message[3]);
unsigned mp_trigger_count(void);
unsigned mp_trig_msg_total(void);
const struct mp_trigger *mp_trigger(unsigned index);
/* Append a trigger (header fields of t, no messages): index, or -1 if full. */
int mp_trigger_add(const struct mp_trigger *t);
/* Replace a trigger's header fields (its messages stay). */
void mp_trigger_set(unsigned index, const struct mp_trigger *t);
void mp_trigger_delete(unsigned index);
const struct mp_trig_msg *mp_trig_msg(unsigned trigger, unsigned row);
/* Insert a message at row (0..count): 0, or -1 if the trigger or pool is full. */
int mp_trig_msg_insert(unsigned trigger, unsigned row, const struct mp_trig_msg *m);
void mp_trig_msg_set(unsigned trigger, unsigned row, const struct mp_trig_msg *m);
void mp_trig_msg_delete(unsigned trigger, unsigned row);
/* Most messages that triggers up to and including `index` could send for
 * one input that fires it (overlapping triggers before it count first). Over
 * MP_TRIG_EMIT_MAX means some of its messages may be cut. */
unsigned mp_trigger_worst(unsigned index);
/* REC on Edit Trigger: send its messages now, IN values from the lower ends
 * of its ranges. */
void mp_trigger_test(unsigned index);

/* ------------------------------------------------------------------------
 * MIDI I/O (midi_router.c). Control task only unless noted.
 * Active Sensing (FE) and the undefined statuses F4, F5, F9, FD are dropped
 * on arrival: never processed, shown or passed through.
 * ------------------------------------------------------------------------ */
/* Queue a message for USB MIDI OUT. Returns 0, or -1 if the queue is full. */
int mp_emit(const uint8_t *message, unsigned length);
/* Queue a received SysEx frame (the event being processed) for output.
 * Returns -1 if it is too long to forward or there is no room. */
int mp_emit_sysex(const struct mp_event *event);
/* SysEx from fm1_app_sysex (routing task): queued in order with short
 * messages. */
void mp_input_sysex(unsigned source, const uint8_t *message, size_t bytes);
/* Send a message generated on the FM-1 itself (knobs, keys) straight to
 * the output, bypassing the pipeline, and push it out at once. */
void mp_send_local(const uint8_t *message, unsigned length);
/* The bytes of a stored SysEx frame (the event being processed); returns
 * its length, or 0 if its bytes were not stored. */
unsigned mp_event_sysex(const struct mp_event *event, uint8_t out[MP_SYSEX_FORWARD_MAX]);
/* Queue a SysEx message generated here (a dump packet, at most
 * MP_SYSEX_FORWARD_MAX bytes) and push it out. -1 if there is no room. */
int mp_send_sysex_local(const uint8_t *message, unsigned length);
/* Nonzero when nothing is waiting in the output queue. */
int mp_output_idle(void);
struct mp_stats {
    uint32_t received;    /* messages taken from the input queue */
    uint32_t sent;        /* messages accepted by the USB transport */
    uint32_t lost_input;  /* input queue overflows (routing task) */
    uint32_t lost_output; /* output queue overflows or transport errors */
    uint32_t sysex;       /* complete SysEx messages received */
};
void mp_stats_read(struct mp_stats *stats);
/* Drain pending input and output now. Safe to call from UI code on the
 * control task (not re-entrant with itself). */
void mp_service(void);
/* Last call of every control iteration (end of groove_screen_control). */
void mp_end_of_iteration(void);

/* ------------------------------------------------------------------------
 * Latency diagnostics (midi_router.c)
 * Each input is timestamped on arrival (routing task) and timed again when
 * the USB transport accepts its output. Read over SysEx:
 *   F0 7D 4D 50 01 F7   reply F0 7D 4D 50 01 <values> F7, see diag_reply()
 *   F0 7D 4D 50 02 F7   reset the counters
 * tools/latency_test.py reads and prints them.
 * ------------------------------------------------------------------------ */
/* Microseconds, wrapping at 2^32. Any task. */
uint32_t mp_time_us(void);
/* groove_screen_control start and end times, for the stats (screen.c). */
void mp_note_screen_time(uint32_t start, uint32_t end);
/* SysEx diagnostic command waiting (1 read, 2 reset), or 0. transfer.c. */
unsigned mp_diag_take(void);

/* ------------------------------------------------------------------------
 * SysEx (transfer.c)
 * ------------------------------------------------------------------------ */
/* Complete SysEx messages received from any port. Any context. */
uint32_t mp_sysex_count(void);

/* ------------------------------------------------------------------------
 * MIDI monitor (monitor.c). Control task only.
 * Records input as received, before the pipeline. MIDI clock (F8) is not
 * listed; it drives the header's CLK indicator instead.
 * ------------------------------------------------------------------------ */
#define MON_EVENTS 100U
void mon_init(void);
void mon_log(const struct mp_event *event);
void mon_log_sysex(uint8_t source, uint16_t bytes, uint8_t fate);
void mon_clock(void);

/* Monitor search (design/monitor-search.md): hides monitor rows, never MIDI.
 * Searches combine with AND; each is kept while off. Not stored. */
enum { MON_SEARCH_PORT, MON_SEARCH_CHANNEL, MON_SEARCH_TYPE, MON_SEARCH_D1, MON_SEARCH_D2, MON_SEARCHES };
#define MON_TYPES 16U
struct mon_search {
    uint8_t on[MON_SEARCHES];
    uint8_t ports;      /* bit per port: 1 = INC */
    uint16_t channels;  /* bit per channel: 1 = INC */
    uint16_t types;     /* bit per mon_type_names entry: 1 = INC */
    uint8_t lo[2], hi[2]; /* D1, D2 ranges, lo <= hi */
    uint8_t exc[2];       /* 0 = INC (show inside), 1 = EXC (hide inside) */
};
extern struct mon_search mon_search;
extern const char *const mon_type_names[MON_TYPES]; /* NON NOF PAT CC PC CAT PB SYX ... RST */
int mon_search_active(void);
/* mon_search_keys and the search pages are declared with the other pages. */
/* Note name with 60 = C4, at most 4 characters ("C#-1"); returns length. */
unsigned mp_format_note(char *out, uint8_t note);

/* ------------------------------------------------------------------------
 * Text display (screen.c). Control task only.
 * A 20 x 15 character grid: 5x7 glyphs at 2x scale in 12x16 px cells, so
 * each text row is exactly one 16-row LCD band. Each cell has a text and a
 * background colour; each row can have a 1 px line on its bottom pixel row.
 * ------------------------------------------------------------------------ */
#define UI_COLUMNS 20U
#define UI_ROWS 15U
/* Palette from design/fm1-screens.json. */
enum {
    UI_BLACK = 0x0000,
    UI_WHITE = 0xef5d,
    UI_DIM = 0x738e,
    UI_PANEL = 0x18e3,  /* header background */
    UI_ACCENT = 0x07ff, /* cyan: column headings */
    UI_GREEN = 0x07e0,
    UI_AMBER = 0xfd20,
    UI_RED = 0xf800,
    UI_BLUE = 0x3c9f,   /* editable values; focused field background */
    UI_PURPLE = 0xac9f, /* screen titles */
    UI_ORANGE = 0xfba5, /* field labels */
    UI_PINK = 0xed18,   /* values set by buttons, not SELECT (Live Keys octave) */
    UI_DARK_YELLOW = 0x8c60, /* monitor rows a remap rule matched */
    UI_DARK_GREY = 0x4a49,   /* monitor rows a filter blocked */
    UI_TEAL = 0x4eae         /* monitor rows that fired an event trigger */
};
void ui_clear(void);
/* Write text at row/column on the row's background, clipped to the row.
 * Lowercase is shown as uppercase. Unchanged cells are not redrawn. */
void ui_print(unsigned row, unsigned column, const char *text, uint16_t color);
/* As ui_print with an explicit background (e.g. a focused field). */
void ui_print_colors(unsigned row, unsigned column, const char *text, uint16_t color, uint16_t background);
/* Fill the rest of the row from column with spaces. */
void ui_clear_to_end(unsigned row, unsigned column);
/* Row background, and an optional 1 px line on the row's bottom pixel row. */
void ui_row_style(unsigned row, uint16_t background, int line, uint16_t line_color);
/* Standard screen header on row 0: panel background, purple title, dim
 * underline. Columns after the title are cleared up to `keep_from`, so a
 * page can draw its own indicator after that without flicker. */
void ui_header(const char *title, unsigned keep_from);
/* An editable value: blue, or black on blue while focused. */
void ui_field(unsigned row, unsigned column, const char *text, int focused);
/* A horizontal bar across the row's middle: `width` px (0..240) in `color`,
 * the rest in `track`. Removed by ui_clear. */
void ui_row_bar(unsigned row, unsigned width, uint16_t color, uint16_t track);
/* Up to 10 characters at 4x size, filling rows `row` and `row + 1`.
 * ui_clear removes it. */
void ui_big(unsigned row, const char *text, uint16_t color);
/* Push changed rows to the LCD with non-blocking DMA. Call often. */
void ui_flush(void);

/* Format an unsigned decimal into out (at least 11 bytes); returns length. */
unsigned mp_format_uint(char *out, uint32_t value);
/* As mp_format_uint, zero-padded to at least `width` digits. */
unsigned mp_format_padded(char *out, uint32_t value, unsigned width);

/* ------------------------------------------------------------------------
 * Panel input and pages (screen.c, page files). Control task only.
 * ------------------------------------------------------------------------ */
enum ui_button {
    BTN_OCT_DOWN, BTN_OCT_UP, BTN_FX, BTN_SEL, BTN_ENV, BTN_LFO, BTN_EDIT,
    BTN_GLO, BTN_HOME, BTN_SAVE, BTN_ARP, BTN_SEQ, BTN_PLAY, BTN_REC
};
#define UI_BUTTON(b) (1UL << (b))
/* Encoder indexes, named as printed on the panel. */
enum ui_encoder {
    ENC_SELECT, ENC_ALGORITHM, ENC_KNOB1, ENC_KNOB2, ENC_KNOB3, ENC_KNOB4, ENC_PRESETS
};
struct ui_input {
    uint32_t now;      /* fm1_millis() */
    uint32_t held;     /* buttons down */
    uint32_t pressed;  /* buttons that went down since the last call */
    uint32_t repeat;   /* pressed, plus auto-repeat while held */
    uint32_t keys;          /* note keys down, bit 0 = F3 ... bit 26 = G5 */
    uint32_t keys_pressed;  /* went down since the last call */
    uint32_t keys_released; /* went up since the last call */
    int16_t encoders[7];
};
#define UI_KEYS 27U
#define UI_KEY_LOWEST_NOTE 53U /* F3, with 60 = C4 */
/* White key k (0..15, from F3) for MIDI channel k+1; returns -1 if `key`
 * is a black key. */
int ui_white_key_index(unsigned key);
/* Key number (0..26) of the n-th white key (0..15). */
unsigned ui_white_key(unsigned n);
/* LEDs, cached so unchanged states aren't resent. 0..13 are the buttons
 * (enum ui_button), 14..40 the note keys. Page changes turn all off. */
#define UI_LED_KEY(k) (14U + (k))
void ui_led(unsigned led, int on);

/* Editing helpers for field pages. SELECT moves the focus (wrapping);
 * ui_step is ALGORITHM detents plus OCT+/OCT- presses with auto-repeat. */
unsigned ui_focus_move(unsigned focus, unsigned fields, const struct ui_input *in);
int ui_step(const struct ui_input *in);
/* Index of the lowest of the 16 white keys pressed this call, or -1. */
int ui_white_key_pressed(const struct ui_input *in);

/* A page: enter draws from scratch, update runs every control iteration
 * (redraw nonzero when the page should repaint), leave tidies up. */
struct ui_page {
    void (*enter)(void);
    void (*update)(const struct ui_input *in, int redraw);
    void (*leave)(void);
    /* A page outside the button cycles (an Add Rule page, a trigger's
     * pages): the button of its top-level page goes back one level. */
    const struct ui_page *parent;
};
/* Open a page (e.g. an Add Rule page from its list). */
void ui_show(const struct ui_page *page);
/* Two-press confirmation (REC to delete or save): the first press arms it
 * and its LED flashes; a second press within 5 s confirms. */
struct ui_confirm {
    uint8_t armed;
    uint32_t at;
};
/* Call with the button's press; returns 1 when confirmed. */
int ui_confirm_press(struct ui_confirm *c, int pressed, uint32_t now);
/* LED state: flashing while armed, else `idle`. Also expires the arming. */
int ui_confirm_led(struct ui_confirm *c, uint32_t now, int idle);
extern const struct ui_page monitor_page;  /* monitor.c */
extern const struct ui_page knobs_page;    /* knobs.c */
extern const struct ui_page live_keys_page; /* live_keys.c */
extern const struct ui_page channel_filter_page; /* filter_pages.c */
extern const struct ui_page message_filter_page; /* filter_pages.c */
extern const struct ui_page message_remap_page;    /* remap_pages.c */
extern const struct ui_page add_message_rule_page; /* remap_pages.c */
extern const struct ui_page channel_remap_page;    /* remap_pages.c */
extern const struct ui_page add_channel_rule_page; /* remap_pages.c */
extern const struct ui_page patch_select_page;     /* patch_select.c */
extern const struct ui_page sysex_dump_page;       /* sysex_page.c */
extern const struct ui_page event_triggers_page;   /* trigger_pages.c */
extern const struct ui_page edit_trigger_page;     /* trigger_pages.c */
extern const struct ui_page trigger_messages_page; /* trigger_pages.c */
extern const struct ui_page metronome_page;        /* clock_pages.c */
extern const struct ui_page clock_monitor_page;    /* clock_pages.c */
/* Monitor search pages OP1-OP5 (monitor_search.c), sub-pages of the monitor. */
extern const struct ui_page mon_search_pages[MON_SEARCHES];
/* OP1-OP5 keys on the monitor and its search pages: a short press toggles
 * that search, an 800 ms hold opens its page. Lights the OP key LEDs.
 * Returns 1 if a page was opened. */
int mon_search_keys(const struct ui_input *in);
/* Monitor shortcuts: open a rule or trigger prefilled from a message.
 * An existing rule for the same input is opened for editing; otherwise an
 * unsaved draft (Add Rule page). A trigger is added at once (trigger edits
 * are live) and opened on Edit Trigger; if 32 exist, the list shows FULL. */
void mp_open_message_rule_for(unsigned port, unsigned channel, unsigned type, unsigned in);
void mp_open_channel_rule_for(unsigned port, unsigned channel);
void mp_new_trigger_for(unsigned port, unsigned channel, unsigned type, unsigned d1);

/* Build Message Rule (rule_builder.c, design/rule-builder.md): a scratchpad
 * with an IN half (port, channel, type, value) and an OUT half (channel,
 * value, and the type it came from). Not stored; REC adds the rule to its
 * Message Remap list and clears it. GLO opens the page from any page and
 * returns from it (screen.c). type is enum mp_remap_type. */
extern const struct ui_page rule_builder_page;
void mp_builder_set_in(unsigned port, unsigned channel, unsigned type, unsigned value);
void mp_builder_set_out(unsigned type, unsigned channel, unsigned value);
/* Open the builder, remembering the current page for GLO to return to. */
void ui_open_builder(void);
/* 1 while entering a page that GLO returned to from the builder (so the
 * monitor can keep its scroll position). */
int ui_returning_from_builder(void);

/* KNOB 1-4 send their CC from every page (knobs.c). Channel and CC are
 * part of a patch; the value is not. */
#define MP_KNOBS 4U
#define MP_KNOB_MAX_CC 119U
struct mp_knob {
    uint8_t channel; /* 0..15 */
    uint8_t cc;      /* 0..119 */
    uint8_t value;   /* 0..127, last sent */
};
extern struct mp_knob mp_knobs[MP_KNOBS];
void knobs_init(void);
void knobs_defaults(void); /* channel and CC only */
void knobs_service(const struct ui_input *in);

/* Live Keys settings (live_keys.c), part of a patch. */
#define MP_BANK_OFF (-1)
struct mp_live_keys {
    uint8_t channel;          /* 0..15 */
    uint8_t vel_min, vel_max; /* 1..127, min <= max */
    int8_t octave;            /* -3..+3 */
    uint8_t program;          /* 0..127 */
    int8_t msb, lsb;          /* MP_BANK_OFF or 0..127 */
};
extern struct mp_live_keys mp_live_keys;
void live_keys_defaults(void);

/* ------------------------------------------------------------------------
 * Patches and storage (patch.c), design/patch-format.md. Control task only.
 * The edit buffer is the live state: filters, remaps, knob and Live Keys
 * settings, and mp_patch_name.
 * ------------------------------------------------------------------------ */
#define MP_PATCHES 16U
#define MP_PATCH_NAME 12U
#define MP_PATCH_BODY_MAX 3070U /* version 2, with event triggers */
#define MP_PATCH_EDIT_BUFFER 0x7fU /* SysEx patch number of the edit buffer */
extern char mp_patch_name[MP_PATCH_NAME]; /* edit buffer, space padded */
/* Read the global settings and load the last patch used. Called once, from
 * the first control iteration. */
void mp_patch_boot(void);
unsigned mp_patch_current(void);   /* slot the edit buffer came from */
int mp_patch_dirty(void);          /* edit buffer differs from that slot */
/* Encode the edit buffer as a patch body; returns its length. */
unsigned mp_patch_encode(uint8_t body[MP_PATCH_BODY_MAX]);
/* Check a whole body and, if valid, make it the edit buffer. 0 or -1. */
int mp_patch_decode(const uint8_t *body, unsigned length);
/* Load slot into the edit buffer (an empty or damaged slot gives the
 * defaults) and remember it for power-up. Returns 0, or an fm1_flash_result
 * if storage failed (the edit buffer is still loaded). */
int mp_patch_load(unsigned slot);
/* Write the edit buffer to slot. 0 or an fm1_flash_result. */
int mp_patch_save(unsigned slot);
/* Stored name of a slot (12 characters, not terminated), or 0 if empty. */
const char *mp_patch_stored_name(unsigned slot);
int mp_patch_storage_ok(void);     /* the user partition is large enough */
uint32_t mp_patch_write_ms(void);  /* duration of the last Flash write */
int mp_patch_valid(const uint8_t *body, unsigned length);
/* Changes whenever the edit buffer is replaced (load, dump), so pages can
 * refresh and Add Rule pages can drop rule indexes that no longer hold. */
unsigned mp_patch_generation(void);
unsigned mp_patch_default_body(unsigned slot, uint8_t body[MP_PATCH_BODY_MAX]);
/* A slot's stored body, or the default body if it is empty. */
unsigned mp_patch_read(unsigned slot, uint8_t body[MP_PATCH_BODY_MAX]);
/* A complete body received over SysEx for `slot` (0-15 or
 * MP_PATCH_EDIT_BUFFER). Reloads the edit buffer if it is the edit buffer
 * or the patch in use. No Flash access. 0, or -1 if invalid. */
int mp_patch_receive(unsigned slot, const uint8_t *body, unsigned length);
/* Write a received body to its slot's Flash sector. 0 or fm1_flash_result. */
int mp_patch_store(unsigned slot, const uint8_t *body, unsigned length);
/* Global settings (Sysex Dump page). Set takes effect at once (RAM);
 * save writes Flash only if they differ from what is stored. */
unsigned mp_device_id(void);
int mp_allow_dump_request(void);
void mp_device_settings_set(unsigned device_id, int allow);
int mp_device_settings_save(void);

/* Write all global settings (device, metronome) if they differ from Flash.
 * 0 or an fm1_flash_result. mp_device_settings_save is the same call. */
int mp_global_settings_save(void);

/* ------------------------------------------------------------------------
 * MIDI clock lockout, metronome and clock monitor (clock.c),
 * design/clock-and-metronome.md. Control task only unless marked.
 * ------------------------------------------------------------------------ */
#define MP_CLOCK_NONE 0xffU
/* Clock messages (F2 SPP, F8 tick, FA start, FB continue, FC stop) after the
 * filters. Returns 1 to pass the message on, 0 if the lockout drops it. */
void mp_clock_init(void);
int mp_is_clock_message(uint8_t status);
int mp_clock_accept(const struct mp_event *in);
/* Every control iteration: the 2 s owner timeout and the audio time base. */
void mp_clock_poll(void);
struct mp_clock_status {
    uint8_t owner;       /* port, or MP_CLOCK_NONE */
    uint8_t running;     /* between START/CONTINUE and STOP */
    uint32_t elapsed_ms; /* since START, paused while stopped */
};
void mp_clock_status(struct mp_clock_status *out);
/* BPM measurement runs only while the Clock Monitor is shown. */
void mp_clock_bpm_enable(int on);
int mp_clock_bpm10(void); /* BPM x 10, or -1 when no recent ticks */

enum { MP_TIME_BASES = 4 }; /* 1/4, 1/8, 1/16, 1/32 */
#define MP_CLICK_HZ_MIN 200U
#define MP_CLICK_HZ_MAX 4000U
struct mp_metronome {
    uint8_t enabled;
    uint8_t time_base;  /* 0..3 */
    uint8_t beats;      /* 1..16 clicks per bar */
    uint16_t click_hz;  /* 200..4000, steps of 10 */
    uint16_t accent_hz; /* first beat of each bar */
};
extern struct mp_metronome mp_metronome;
void mp_metronome_defaults(void);
/* Bar and beat (from 1) of the last click position; 0 before any tick. */
int mp_metronome_position(unsigned *bar, unsigned *beat);

/* Click synthesis (audio_output.c). mp_click_post: control task; the
 * frame is in the audio ISR's frame count (mp_audio_frames). */
extern volatile uint32_t mp_audio_frames;
void mp_click_init(void);
void mp_click_post(uint32_t frame, unsigned hz);

/* ------------------------------------------------------------------------
 * Patch SysEx (sysex.c), design/patch-format.md: F0 7D <device> 11 <cmd>.
 * Control task only.
 * ------------------------------------------------------------------------ */
#define MP_DUMP_ALL 16U
/* An incoming SysEx frame (the event being processed). Returns 1 if it was
 * a patch message for this device (consumed: not filtered or forwarded). */
int mp_sysex_receive(const uint8_t *message, unsigned length);
/* Send dumps and write received patches to Flash. Call from the control
 * iteration outside mp_service (it may write Flash). */
void mp_sysex_poll(void);
/* Start sending patch 0-15 or MP_DUMP_ALL. -1 if a dump is already going. */
int mp_sysex_send(unsigned what);
/* Nonzero while a dump is being sent; slot is the patch going out
 * (7F = edit buffer) and last the final one. */
int mp_sysex_sending(unsigned *slot, unsigned *last);
struct mp_sysex_counts {
    uint32_t received;  /* complete patches accepted */
    uint32_t rejected;  /* patches discarded: bad packet, checksum or body */
    uint32_t requests;  /* requests answered */
    int last_error;     /* last Flash result of a received patch, 0 = OK */
};
void mp_sysex_counts(struct mp_sysex_counts *counts);

#endif
