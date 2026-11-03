/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * MIDI Monitor: the last 100 input events as received (before the pipeline),
 * and the page that lists them. Layout: design/fm1-screens.json, "Midi Monitor".
 *
 *   row 0   MIDI MONITOR      CLK     header; CLK blinks with MIDI clock
 *   row 1   P CH TYP D1   D2  -12     column headings; amber = rows scrolled back
 *   row 2+  T 01 NON C4   100         13 events, newest at the bottom
 *
 * Rows whose message matched a remap rule are dark yellow, and rows a filter
 * blocked are dark grey. Both are recorded once, by the pipeline as it
 * processes the message.
 *
 * Controls: SELECT and OCT-/OCT+ scroll. REC clears in two steps: its LED
 * is lit on this page; the first press makes it flash, a second press within
 * 5 s clears the list. The bottom row shown is selected; the keys C#5, D#5
 * and F#5 turn it into a message rule, channel rule or trigger, and F5 and
 * G5 put it into Build Message Rule's IN or OUT (see "Shortcuts").
 *
 * Search (design/monitor-search.md): OP1-OP5 (F#3, G#3, A#3, C#4, D#4) turn
 * a port, channel, type, D1 or D2 search on/off with a short press, and open
 * its page when held. While any search is on, SRCH shows in the header, and
 * scrolling, the selection and the shortcuts use the shown rows only.
 * Control task only. */
#include "midiproc.h"
#include <string.h>

/* ---- Event buffer --------------------------------------------------------- */
struct mon_entry {
    uint8_t source, status, d1, d2;
    uint8_t fate;     /* enum mp_fate: remapped dark yellow, blocked dark grey,
                         fired a trigger teal */
    uint16_t bytes; /* SysEx length */
};
static struct mon_entry ring[MON_EVENTS];
static unsigned head;   /* next slot to write */
static unsigned count;  /* valid entries, <= MON_EVENTS */
static uint32_t added;  /* events ever logged; lets a scrolled view stay put */
static uint32_t clock_ticks, clock_ms;
static uint8_t clock_seen;

void mon_init(void)
{
    head = count = 0;
    added = clock_ticks = 0;
    clock_seen = 0;
}

static void push(const struct mon_entry *e)
{
    ring[head] = *e;
    head = (head + 1U) % MON_EVENTS;
    if (count < MON_EVENTS) ++count;
    ++added;
}

void mon_log(const struct mp_event *e)
{
    struct mon_entry m = {e->source, e->data[0], e->length > 1 ? e->data[1] : 0,
                          e->length > 2 ? e->data[2] : 0, e->fate, 0};
    if (m.status == 0xfa && e->fate != MP_BLOCKED) clock_ticks = 0; /* Start: align the CLK blink */
    push(&m);
}

void mon_log_sysex(uint8_t source, uint16_t bytes, uint8_t fate)
{
    struct mon_entry m = {source, 0xf0, 0, 0, fate, bytes};
    push(&m);
}

void mon_clock(void)
{
    ++clock_ticks;
    clock_ms = fm1_millis();
    clock_seen = 1;
}

/* Entry `back` rows before the newest (0 = newest). */
static const struct mon_entry *entry(unsigned back)
{
    return &ring[(head + MON_EVENTS - 1U - back) % MON_EVENTS];
}

/* ---- Search ------------------------------------------------------------------
 * design/monitor-search.md. Searches only choose which stored entries are
 * shown; every message is still recorded. All active searches must pass
 * (AND). Channel: system messages pass. D1/D2: a message without that byte
 * counts as outside the range. */
struct mon_search mon_search = {
    {0, 0, 0, 0, 0}, 0x07U, 0xffffU, 0xffffU, {0, 0}, {127, 127}, {0, 0}
};
const char *const mon_type_names[MON_TYPES] = {
    "NON", "NOF", "PAT", "CC", "PC", "CAT", "PB",
    "SYX", "MTC", "SPP", "SS", "TR", "STA", "CON", "STP", "RST"
};

int mon_search_active(void)
{
    for (unsigned i = 0; i < MON_SEARCHES; ++i)
        if (mon_search.on[i]) return 1;
    return 0;
}

/* Index into mon_type_names, or -1 (not listed: always shown). */
static int type_index(const struct mon_entry *e)
{
    switch (e->status < 0xf0 ? e->status & 0xf0U : e->status) {
    case 0x90: return e->d2 ? 0 : 1; /* velocity 0 is Note Off */
    case 0x80: return 1;
    case 0xa0: return 2;
    case 0xb0: return 3;
    case 0xc0: return 4;
    case 0xd0: return 5;
    case 0xe0: return 6;
    case 0xf0: return 7;
    case 0xf1: return 8;
    case 0xf2: return 9;
    case 0xf3: return 10;
    case 0xf6: return 11;
    case 0xfa: return 12;
    case 0xfb: return 13;
    case 0xfc: return 14;
    case 0xff: return 15;
    default:   return -1;
    }
}

/* Does the message have data byte 1 / 2? */
static int has_data(const struct mon_entry *e, unsigned which)
{
    if (e->status < 0xf0) {
        unsigned kind = e->status & 0xf0U;
        return which == 0 || (kind != 0xc0 && kind != 0xd0);
    }
    if (e->status == 0xf2) return 1;
    return which == 0 && (e->status == 0xf1 || e->status == 0xf3);
}

static int shown(const struct mon_entry *e)
{
    const struct mon_search *s = &mon_search;
    if (s->on[MON_SEARCH_PORT] && !((s->ports >> e->source) & 1U)) return 0;
    if (s->on[MON_SEARCH_CHANNEL] && e->status < 0xf0 && !((s->channels >> (e->status & 15U)) & 1U)) return 0;
    if (s->on[MON_SEARCH_TYPE]) {
        int t = type_index(e);
        if (t >= 0 && !((s->types >> t) & 1U)) return 0;
    }
    for (unsigned w = 0; w < 2U; ++w) {
        if (!s->on[MON_SEARCH_D1 + w]) continue;
        uint8_t v = w ? e->d2 : e->d1;
        int inside = has_data(e, w) && v >= s->lo[w] && v <= s->hi[w];
        if (s->exc[w] ? inside : !inside) return 0;
    }
    return 1;
}

/* The shown entries, newest first, as `back` positions. Without a search
 * the view is every entry. Rebuilt on each update while a search is on. */
static uint8_t view[MON_EVENTS];
static unsigned view_count;
static uint8_t searching;

static void build_view(void)
{
    searching = (uint8_t)mon_search_active();
    if (!searching) {
        view_count = count;
        return;
    }
    view_count = 0;
    for (unsigned back = 0; back < count; ++back)
        if (shown(entry(back))) view[view_count++] = (uint8_t)back;
}

/* The i-th shown entry, newest first. */
static const struct mon_entry *view_entry(unsigned i)
{
    return entry(searching ? view[i] : i);
}

/* Shown entries among the `fresh` newest. */
static unsigned shown_among_newest(unsigned fresh)
{
    if (!searching) return fresh;
    unsigned n = 0;
    while (n < view_count && view[n] < fresh) ++n;
    return n;
}

/* OP1-OP5: the lower five black keys F#3, G#3, A#3, C#4, D#4. */
enum { HOLD_MS = 800 };
static const uint8_t op_key[MON_SEARCHES] = {54U - UI_KEY_LOWEST_NOTE, 56U - UI_KEY_LOWEST_NOTE,
                                             58U - UI_KEY_LOWEST_NOTE, 61U - UI_KEY_LOWEST_NOTE,
                                             63U - UI_KEY_LOWEST_NOTE};
static uint32_t op_down_at[MON_SEARCHES];
static uint8_t op_down, op_held; /* bit per key: pressed here; hold already acted */

int mon_search_keys(const struct ui_input *in)
{
    int opened = 0;
    for (unsigned k = 0; k < MON_SEARCHES; ++k) {
        uint32_t key = 1UL << op_key[k];
        uint8_t bit = (uint8_t)(1U << k);
        if (in->keys_pressed & key) {
            op_down |= bit;
            op_held &= (uint8_t)~bit;
            op_down_at[k] = in->now;
        }
        if ((op_down & bit) && !(op_held & bit) && (in->keys & key) && in->now - op_down_at[k] >= HOLD_MS) {
            op_held |= bit;
            if (!opened) {
                ui_show(&mon_search_pages[k]);
                opened = 1;
            }
        }
        if (in->keys_released & key) {
            if ((op_down & bit) && !(op_held & bit)) mon_search.on[k] ^= 1U; /* short press */
            op_down &= (uint8_t)~bit;
        }
    }
    for (unsigned k = 0; k < MON_SEARCHES; ++k) ui_led(UI_LED_KEY(op_key[k]), mon_search.on[k]);
    return opened;
}

/* ---- Formatting ------------------------------------------------------------
 * Columns: 0 port, 2 channel, 5 type, 9 data 1, 14 data 2. The design had
 * data 2 at 13, which ran into 4-character note names such as C#-1. */
static void put(char *line, unsigned at, const char *s)
{
    while (*s && at < UI_COLUMNS) line[at++] = *s++;
}

static void put_uint(char *line, unsigned at, uint32_t v)
{
    char digits[11];
    mp_format_uint(digits, v);
    put(line, at, digits);
}

unsigned mp_format_note(char *out, uint8_t note)
{
    static const char names[12][3] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const char *name = names[note % 12U];
    unsigned n = 0;
    while (*name) out[n++] = *name++;
    int octave = (int)(note / 12U) - 1;
    if (octave < 0) { out[n++] = '-'; octave = -octave; }
    out[n++] = (char)('0' + octave);
    out[n] = 0;
    return n;
}

static void put_note(char *line, unsigned at, uint8_t note)
{
    char text[6];
    mp_format_note(text, note);
    put(line, at, text);
}

static void put_signed(char *line, unsigned at, int v)
{
    put(line, at++, v < 0 ? "-" : "+");
    put_uint(line, at, (uint32_t)(v < 0 ? -v : v));
}

static const char *type_name(uint8_t status)
{
    if (status < 0xf0) {
        static const char *const voice[8] = {"NOF", "NON", "PAT", "CC", "PC", "CAT", "PB", ""};
        return voice[(status >> 4) & 7U];
    }
    switch (status) {
    case 0xf0: return "SYX";
    case 0xf1: return "MTC";
    case 0xf2: return "SPP";
    case 0xf3: return "SS";
    case 0xf6: return "TR";
    case 0xfa: return "STA";
    case 0xfb: return "CON";
    case 0xfc: return "STP";
    case 0xff: return "RST";
    default:   return 0; /* undefined status: shown as hex */
    }
}

static void format_entry(char line[UI_COLUMNS + 1], const struct mon_entry *e)
{
    static const char ports[3] = {'T', 'U', 'B'};
    static const char hex[] = "0123456789ABCDEF";
    memset(line, ' ', UI_COLUMNS);
    line[UI_COLUMNS] = 0;
    line[0] = e->source < 3U ? ports[e->source] : '?';
    if (e->status < 0xf0) {
        unsigned ch = (e->status & 15U) + 1U;
        line[2] = (char)('0' + ch / 10U);
        line[3] = (char)('0' + ch % 10U);
    } else {
        put(line, 2, "--");
    }
    const char *type = type_name(e->status);
    if (type) {
        put(line, 5, type);
    } else {
        line[5] = hex[e->status >> 4];
        line[6] = hex[e->status & 15U];
    }
    switch (e->status < 0xf0 ? e->status & 0xf0U : e->status) {
    case 0x80: case 0x90: case 0xa0: /* note, velocity or pressure */
        put_note(line, 9, e->d1);
        put_uint(line, 14, e->d2);
        break;
    case 0xb0: /* controller, value */
        put_uint(line, 9, e->d1);
        put_uint(line, 14, e->d2);
        break;
    case 0xc0: case 0xd0: case 0xf1: case 0xf3: /* one data byte */
        put_uint(line, 9, e->d1);
        break;
    case 0xe0: /* pitch bend, -8192..+8191 */
        put_signed(line, 9, (int)((unsigned)e->d1 | (unsigned)e->d2 << 7) - 8192);
        break;
    case 0xf2: /* song position, 0..16383 */
        put_uint(line, 9, (unsigned)e->d1 | (unsigned)e->d2 << 7);
        break;
    case 0xf0: { /* SysEx byte count, e.g. "24B" */
        char digits[11];
        unsigned n = mp_format_uint(digits, e->bytes);
        put(line, 9, digits);
        put(line, 9U + n, "B");
        break;
    }
    default:
        break;
    }
}

/* ---- Page ----------------------------------------------------------------- */
enum { LIST_TOP = 2, LIST_ROWS = UI_ROWS - LIST_TOP }; /* 13 rows */
static unsigned scroll;      /* rows back from the newest; 0 = live */
static uint32_t seen_added;  /* `added` at the last update */

/* REC clear confirmation */
enum { CONFIRM_MS = 5000, FLASH_MS = 100 };
static uint8_t clear_armed;
static uint32_t armed_at;

static unsigned max_scroll(void)
{
    return view_count > LIST_ROWS ? view_count - LIST_ROWS : 0U;
}

static void enter(void)
{
    ui_clear();
    clear_armed = 0;
    if (ui_returning_from_builder()) { /* GLO back: keep the selected row */
        build_view();                   /* update() follows new events */
        if (scroll > max_scroll()) scroll = max_scroll();
        return;
    }
    scroll = 0;
    seen_added = added;
    build_view();
}

/* ---- Shortcuts -------------------------------------------------------------
 * The bottom row shown is selected (black on blue). The last three black
 * keys turn its message into a rule or trigger, and the last two white keys
 * put it into the rule builder; each key is lit when its shortcut applies
 * to the selected message. */
enum { SHORTCUT_MSG_RULE, SHORTCUT_CH_RULE, SHORTCUT_TRIGGER, SHORTCUT_BUILD_IN, SHORTCUT_BUILD_OUT,
       SHORTCUTS };
static const uint8_t shortcut_key[SHORTCUTS] = {
    73U - UI_KEY_LOWEST_NOTE,  /* C#5: message rule */
    75U - UI_KEY_LOWEST_NOTE,  /* D#5: channel rule */
    78U - UI_KEY_LOWEST_NOTE,  /* F#5: trigger */
    77U - UI_KEY_LOWEST_NOTE,  /* F5: builder IN */
    79U - UI_KEY_LOWEST_NOTE,  /* G5: builder OUT */
};

/* Remap type (message rule) and trigger type of a message, or -1. */
static int remap_type_of(const struct mon_entry *e)
{
    switch (e->status < 0xf0 ? e->status & 0xf0U : 0U) {
    case 0x80: case 0x90: case 0xa0: return REMAP_NOTE;
    case 0xb0: return REMAP_CC;
    case 0xc0: return REMAP_PC;
    default:   return -1;
    }
}

static int trigger_type_of(const struct mon_entry *e)
{
    switch (e->status < 0xf0 ? e->status & 0xf0U : 0U) {
    case 0x80: return TRIG_NOF;
    case 0x90: return e->d2 ? TRIG_NON : TRIG_NOF;
    case 0xb0: return TRIG_CC;
    case 0xc0: return TRIG_PC;
    default:   return -1;
    }
}

static int shortcut_applies(unsigned s, const struct mon_entry *e)
{
    if (!e || e->source >= MP_PORTS) return 0;
    switch (s) {
    case SHORTCUT_MSG_RULE:
    case SHORTCUT_BUILD_IN:
    case SHORTCUT_BUILD_OUT: return remap_type_of(e) >= 0;
    case SHORTCUT_CH_RULE: return e->status < 0xf0;
    default: return trigger_type_of(e) >= 0;
    }
}

/* Returns 1 if a shortcut key opened another page. */
static int run_shortcut(const struct ui_input *in, const struct mon_entry *e)
{
    for (unsigned s = 0; s < SHORTCUTS; ++s) {
        int lit = shortcut_applies(s, e);
        ui_led(UI_LED_KEY(shortcut_key[s]), lit);
        if (!lit || !(in->keys_pressed & (1UL << shortcut_key[s]))) continue;
        unsigned channel = e->status & 15U;
        switch (s) {
        case SHORTCUT_MSG_RULE:
            mp_open_message_rule_for(e->source, channel, (unsigned)remap_type_of(e), e->d1);
            break;
        case SHORTCUT_CH_RULE:
            mp_open_channel_rule_for(e->source, channel);
            break;
        case SHORTCUT_BUILD_IN:
            mp_builder_set_in(e->source, channel, (unsigned)remap_type_of(e), e->d1);
            ui_open_builder();
            break;
        case SHORTCUT_BUILD_OUT:
            mp_builder_set_out((unsigned)remap_type_of(e), channel, e->d1);
            ui_open_builder();
            break;
        default:
            mp_new_trigger_for(e->source, channel, (unsigned)trigger_type_of(e), e->d1);
            break;
        }
        return 1;
    }
    return 0;
}

static void scroll_by(int rows) /* positive = older */
{
    int s = (int)scroll + rows;
    if (s < 0) s = 0;
    if ((unsigned)s > max_scroll()) s = (int)max_scroll();
    scroll = (unsigned)s;
}

static void update(const struct ui_input *in, int redraw)
{
    if (mon_search_keys(in)) return; /* OP key held: its search page opened */
    int was_searching = searching;
    build_view();
    if (searching != was_searching) scroll = 0; /* a search turned on or off */
    if (scroll > max_scroll()) scroll = max_scroll();

    /* While scrolled back, keep the same events on screen as new ones arrive. */
    uint32_t fresh = added - seen_added;
    seen_added = added;
    if (scroll && fresh) scroll_by((int)shown_among_newest(fresh > MON_EVENTS ? MON_EVENTS : fresh));

    if (clear_armed && in->now - armed_at >= CONFIRM_MS) clear_armed = 0;
    if (in->pressed & UI_BUTTON(BTN_REC)) {
        if (clear_armed) {
            mon_init();
            seen_added = 0;
            scroll = 0;
            clear_armed = 0;
            build_view();
        } else {
            clear_armed = 1;
            armed_at = in->now;
        }
    }
    /* Lit on this page; flashing (5 Hz) while waiting for the confirmation. */
    ui_led(BTN_REC, clear_armed ? ((in->now - armed_at) / FLASH_MS) % 2U == 0 : 1);
    ui_led(BTN_OCT_DOWN, 1); /* they scroll the list */
    ui_led(BTN_OCT_UP, 1);
    if (in->repeat & UI_BUTTON(BTN_OCT_DOWN)) scroll_by(1);
    if (in->repeat & UI_BUTTON(BTN_OCT_UP)) scroll_by(-1);
    if (in->encoders[ENC_SELECT]) scroll_by(-in->encoders[ENC_SELECT]); /* clockwise = newer */
    if (run_shortcut(in, view_count ? view_entry(scroll) : 0)) return;
    if (!redraw) return;

    /* Header: SRCH while a search is on; CLK is lit for the first half of
     * each beat while clock arrives. */
    ui_header("MIDI MONITOR", 17);
    ui_print(0, 13, searching ? "SRCH" : "    ", UI_AMBER);
    int clock_on = clock_seen && in->now - clock_ms < 500U && clock_ticks % 24U < 12U;
    ui_print(0, 17, clock_on ? "CLK" : "   ", UI_GREEN);

    ui_print(1, 0, "P CH TYP D1   D2", UI_ACCENT);
    char mark[5] = "    ";
    if (scroll) {
        char digits[11];
        unsigned n = mp_format_uint(digits, scroll);
        mark[3 - n] = '-';
        memcpy(mark + 4 - n, digits, n);
    }
    ui_print(1, 16, mark, UI_AMBER);

    /* Newest at the bottom; when the buffer is short, rows fill from the top. */
    unsigned rows = view_count - scroll < LIST_ROWS ? view_count - scroll : LIST_ROWS;
    for (unsigned row = 0; row < LIST_ROWS; ++row) {
        char line[UI_COLUMNS + 1];
        if (row < rows) {
            const struct mon_entry *e = view_entry(scroll + rows - 1U - row);
            format_entry(line, e);
            uint16_t color = e->fate == MP_TRIGGERED ? UI_TEAL
                           : e->fate == MP_REMAPPED  ? UI_DARK_YELLOW
                           : e->fate == MP_BLOCKED   ? UI_DARK_GREY : UI_WHITE;
            if (row == rows - 1U) ui_print_colors(LIST_TOP + row, 0, line, UI_BLACK, UI_BLUE); /* selected */
            else ui_print(LIST_TOP + row, 0, line, color);
        } else if (row == 0 && count == 0) {
            ui_print(LIST_TOP, 0, "WAITING FOR MIDI    ", UI_DIM);
        } else if (row == 0 && view_count == 0) {
            ui_print(LIST_TOP, 0, "NO MATCHES          ", UI_DIM);
        } else {
            ui_clear_to_end(LIST_TOP + row, 0);
        }
    }
}

const struct ui_page monitor_page = {enter, update, 0};
