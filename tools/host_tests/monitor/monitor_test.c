/* Host tests of the MIDI Monitor search (src/app/monitor.c, monitor_search.c):
 * what the monitor shows, the OP keys (short press / hold), the search pages
 * and the shortcuts on the shown rows. Also the rule builder
 * (src/app/rule_builder.c) and its F5/G5 shortcuts. Run ../run.bat. */
#include "midiproc.h"
#include <stdio.h>

/* ---- Stubs: a text grid, LEDs, pages ---- */
static char grid[UI_ROWS][UI_COLUMNS + 1];
static uint16_t ink[UI_ROWS][UI_COLUMNS];
static int leds[41];
static const struct ui_page *current;
static uint32_t now_ms = 1000;
const char *const mp_port_names[MP_PORTS] = {"TRS", "USB", "BLE"};
static char opened[64];

uint32_t fm1_millis(void) { return now_ms; }
void ui_clear(void) { for (unsigned r = 0; r < UI_ROWS; ++r) { memset(grid[r], ' ', UI_COLUMNS); grid[r][UI_COLUMNS] = 0; } }
void ui_print_colors(unsigned row, unsigned col, const char *s, uint16_t fg, uint16_t bg)
{
    (void)bg;
    for (; *s && col < UI_COLUMNS; ++s, ++col) { grid[row][col] = *s; ink[row][col] = fg; }
}
void ui_print(unsigned row, unsigned col, const char *s, uint16_t fg) { ui_print_colors(row, col, s, fg, 0); }
void ui_clear_to_end(unsigned row, unsigned col) { for (; col < UI_COLUMNS; ++col) grid[row][col] = ' '; }
void ui_header(const char *t, unsigned keep) { ui_print(0, 0, t, UI_PURPLE); for (unsigned c = (unsigned)strlen(t); c < keep; ++c) grid[0][c] = ' '; }
void ui_field(unsigned row, unsigned col, const char *t, int f) { ui_print_colors(row, col, t, f ? UI_BLACK : UI_BLUE, 0); }
void ui_led(unsigned led, int on) { if (led < 41) leds[led] = !!on; }
void ui_show(const struct ui_page *p) { memset(leds, 0, sizeof leds); current = p; p->enter(); }
unsigned mp_format_uint(char *out, uint32_t v) { return (unsigned)sprintf(out, "%u", v); }
unsigned mp_format_padded(char *out, uint32_t v, unsigned w) { return (unsigned)sprintf(out, "%0*u", (int)w, v); }
static int is_white(unsigned k) { return (0xab5 >> ((UI_KEY_LOWEST_NOTE + k) % 12U)) & 1U; }
unsigned ui_white_key(unsigned n) { for (unsigned k = 0; k < UI_KEYS; ++k) if (is_white(k) && n-- == 0) return k; return 0; }
unsigned ui_focus_move(unsigned f, unsigned n, const struct ui_input *in)
{
    int d = in->encoders[ENC_SELECT];
    return d ? (unsigned)(((int)f + d % (int)n + (int)n) % (int)n) : f;
}
int ui_step(const struct ui_input *in)
{
    int d = in->encoders[ENC_ALGORITHM];
    if (in->repeat & UI_BUTTON(BTN_OCT_UP)) ++d;
    if (in->repeat & UI_BUTTON(BTN_OCT_DOWN)) --d;
    return d;
}
int ui_white_key_pressed(const struct ui_input *in)
{
    for (unsigned k = 0; k < UI_KEYS; ++k)
        if ((in->keys_pressed >> k) & 1U) {
            if (!is_white(k)) continue;
            int n = 0;
            for (unsigned j = 0; j < k; ++j) n += is_white(j);
            return n;
        }
    return -1;
}
void mp_open_message_rule_for(unsigned p, unsigned c, unsigned t, unsigned in) { sprintf(opened, "MSG %u %u %u %u", p, c, t, in); }
void mp_open_channel_rule_for(unsigned p, unsigned c) { sprintf(opened, "CH %u %u", p, c); }
void mp_new_trigger_for(unsigned p, unsigned c, unsigned t, unsigned d1) { sprintf(opened, "TRIG %u %u %u %u", p, c, t, d1); }

/* Rule pool stub: list sizes can be forced to test LIST FULL. */
static struct mp_msg_rule rules[MP_MSG_RULES_TOTAL];
static unsigned rule_count, forced_list_size;
static int last_replace = -2;
unsigned mp_msg_rule_count(void) { return rule_count; }
int mp_msg_rule_find(unsigned p, unsigned c, unsigned t, unsigned in)
{
    for (unsigned i = 0; i < rule_count; ++i)
        if (rules[i].port == p && rules[i].channel == c && rules[i].type == t && rules[i].in == in) return (int)i;
    return -1;
}
unsigned mp_msg_rules_in_list(unsigned p, unsigned c, unsigned t)
{
    unsigned n = forced_list_size;
    for (unsigned i = 0; i < rule_count; ++i) n += rules[i].port == p && rules[i].channel == c && rules[i].type == t;
    return n;
}
int mp_msg_rule_save(const struct mp_msg_rule *r, int replace)
{
    last_replace = replace;
    if (replace >= 0) { rules[replace] = *r; return 0; }
    if (rule_count >= MP_MSG_RULES_TOTAL) return -1;
    rules[rule_count++] = *r;
    return 0;
}
/* Two-press confirmation, as in screen.c. */
int ui_confirm_press(struct ui_confirm *c, int pressed, uint32_t now)
{
    if (c->armed && now - c->at >= 5000U) c->armed = 0;
    if (!pressed) return 0;
    if (c->armed) { c->armed = 0; return 1; }
    c->armed = 1;
    c->at = now;
    return 0;
}
int ui_confirm_led(struct ui_confirm *c, uint32_t now, int idle)
{
    if (c->armed && now - c->at >= 5000U) c->armed = 0;
    return c->armed ? ((now - c->at) / 100U) % 2U == 0 : idle;
}
/* GLO (screen.c): open the builder, and return to the page it came from. */
static const struct ui_page *builder_return;
static int returning;
void ui_open_builder(void) { if (current != &rule_builder_page) { builder_return = current; ui_show(&rule_builder_page); } }
int ui_returning_from_builder(void) { return returning; }
static void glo_back(void) { returning = 1; ui_show(builder_return); returning = 0; }

/* ---- Driving the pages ---- */
static uint32_t keys_down;
static void step(uint32_t ms, uint32_t press, uint32_t release, int sel, int alg, uint32_t buttons)
{
    now_ms += ms;
    struct ui_input in;
    memset(&in, 0, sizeof in);
    in.now = now_ms;
    keys_down = (keys_down | press) & ~release;
    in.keys = keys_down;
    in.keys_pressed = press;
    in.keys_released = release;
    in.encoders[ENC_SELECT] = (int16_t)sel;
    in.encoders[ENC_ALGORITHM] = (int16_t)alg;
    in.pressed = in.repeat = buttons;
    current->update(&in, 1);
}
static void idle(void) { step(10, 0, 0, 0, 0, 0); }
static const char *row(unsigned r)
{
    static char t[UI_COLUMNS + 1];
    memcpy(t, grid[r], UI_COLUMNS + 1);
    for (int i = UI_COLUMNS - 1; i >= 0 && t[i] == ' '; --i) t[i] = 0;
    return t;
}
static unsigned listed(void) /* rows in the monitor list */
{
    unsigned n = 0;
    for (unsigned r = 2; r < UI_ROWS; ++r) if (row(r)[0] && strcmp(row(r), "NO MATCHES") && strcmp(row(r), "WAITING FOR MIDI")) ++n;
    return n;
}
static int any_row(const char *text)
{
    for (unsigned r = 2; r < UI_ROWS; ++r) if (strstr(row(r), text)) return 1;
    return 0;
}
static void log3(unsigned port, uint8_t s, uint8_t d1, uint8_t d2, unsigned len)
{
    struct mp_event e = {0};
    e.source = (uint8_t)port; e.data[0] = s; e.data[1] = d1; e.data[2] = d2; e.length = (uint8_t)len;
    mon_log(&e);
}

enum { KEY_F3 = 0, OP1 = 1, OP2 = 3, OP3 = 5, OP4 = 8, OP5 = 10, CS5 = 20, F5 = 24, G5 = 26 };
#define K(k) (1UL << (k))
static void tap(unsigned k) { step(10, K(k), 0, 0, 0, 0); step(100, 0, K(k), 0, 0, 0); }

static int checks, failures;
static void check(int ok, const char *what) { ++checks; if (!ok) { ++failures; printf("FAIL %s\n", what); } }

int main(void)
{
    ui_clear();
    mon_init();
    log3(0, 0x90, 60, 100, 3);  /* T 01 NON C4 100 */
    log3(1, 0xB1, 7, 64, 3);    /* U 02 CC 7 64 */
    log3(1, 0x99, 36, 0, 3);    /* U 10 NON vel 0 = NOF */
    log3(2, 0xFA, 0, 0, 1);     /* B STA */
    mon_log_sysex(1, 6, MP_PASSED);
    log3(0, 0xC0, 5, 0, 2);     /* T 01 PC 5 */
    log3(1, 0xE0, 0, 64, 3);    /* U 01 PB 0 */
    ui_show(&monitor_page);
    idle();
    check(listed() == 7 && !strstr(row(0), "SRCH"), "no search: all 7 shown, no SRCH");

    /* OP1 short press: port search on (all INC = no change), LED lit. */
    tap(OP1);
    check(mon_search.on[MON_SEARCH_PORT] && current == &monitor_page, "OP1 short press turns the port search on");
    check(strstr(row(0), "SRCH") != 0 && leds[UI_LED_KEY(OP1)], "SRCH in header, OP1 LED lit");
    check(listed() == 7, "all ports INC: nothing hidden");
    mon_search.ports = 0x2; /* USB only */
    idle();
    check(listed() == 4 && !any_row("T 01") && !any_row("STA"), "USB only: 4 rows");
    tap(OP1);
    check(!mon_search.on[MON_SEARCH_PORT] && listed() == 7 && !leds[UI_LED_KEY(OP1)], "OP1 again: off, all back");

    /* Hold OP2: opens the channel page; release does not toggle. */
    step(10, K(OP2), 0, 0, 0, 0);
    step(700, 0, 0, 0, 0, 0);
    check(current == &monitor_page, "not opened before 800 ms");
    step(150, 0, 0, 0, 0, 0);
    check(current == &mon_search_pages[MON_SEARCH_CHANNEL], "hold OP2 opens Search Channel");
    step(50, 0, K(OP2), 0, 0, 0);
    check(!mon_search.on[MON_SEARCH_CHANNEL], "releasing after a hold doesn't toggle");
    check(!strcmp(row(0), "SEARCH CHANNEL  OP2") && strstr(row(4), "01: INC") && strstr(row(4), "09: INC"), "channel page layout");
    /* White key 2 (G3) toggles channel 2 to EXC; STATE on with ALGORITHM. */
    step(10, K(2), 0, 0, 0, 0);
    step(10, 0, K(2), 0, 0, 0);
    check(!((mon_search.channels >> 1) & 1U) && strstr(row(5), "02: EXC"), "white key toggles channel 2");
    check(!leds[UI_LED_KEY(2)] && leds[UI_LED_KEY(0)], "white key LEDs lit while INC");
    step(10, 0, 0, 0, 1, 0);
    check(mon_search.on[MON_SEARCH_CHANNEL] && strstr(row(2), "STATE:ON"), "STATE field turns it on");
    ui_show(&monitor_page); /* HOME (the page's parent) */
    idle();
    check(!any_row("U 02") && any_row("SYX") && any_row("STA") && listed() == 6, "channel 2 hidden; system messages pass");
    mon_search.on[MON_SEARCH_CHANNEL] = 0;

    /* Type search: NOF excluded hides Note On velocity 0. */
    mon_search.types = (uint16_t)~(1U << 1);
    tap(OP3);
    check(!any_row("U 10") && listed() == 6, "type: NON vel 0 counts as NOF");
    tap(OP3);

    /* D1 range 60-60 INC: only data 1 = 60; STA, SYX have no D1. */
    mon_search.lo[0] = mon_search.hi[0] = 60;
    tap(OP4);
    check(listed() == 1 && any_row("C4"), "D1 060-060 INC: only C4");
    mon_search.exc[0] = 1;
    idle();
    check(listed() == 6 && !any_row("C4") && any_row("STA"), "D1 060-060 EXC: C4 hidden, STA kept");
    mon_search.exc[0] = 0;
    tap(OP4);

    /* D2: PC and CAT have no D2; PB raw MSB 64. */
    mon_search.lo[1] = 64; mon_search.hi[1] = 64;
    tap(OP5);
    check(listed() == 2 && any_row("CC") && any_row("PB"), "D2 064-064: CC 64 and PB (raw MSB)");
    tap(OP5);

    /* No matches. */
    mon_search.ports = 0;
    tap(OP1);
    check(listed() == 0 && !strcmp(row(2), "NO MATCHES"), "NO MATCHES");
    tap(OP1);
    mon_search.ports = 7;

    /* Shortcut on the selected (bottom shown) row: only the CC row shown. */
    mon_search.types = 1U << 3; /* CC only */
    tap(OP3);
    opened[0] = 0;
    step(10, K(CS5), 0, 0, 0, 0);
    check(!strcmp(opened, "MSG 1 1 1 7"), "C#5 makes a rule from the shown CC row");
    current = &monitor_page;
    step(10, 0, K(CS5), 0, 0, 0);
    tap(OP3);

    /* Search page D1: FROM pushes TO, note names, focus moves. */
    mon_search.lo[0] = 0; mon_search.hi[0] = 10;
    step(10, K(OP4), 0, 0, 0, 0);
    step(850, 0, 0, 0, 0, 0);
    step(10, 0, K(OP4), 0, 0, 0);
    check(current == &mon_search_pages[MON_SEARCH_D1], "hold OP4 opens Search D1");
    step(10, 0, 0, 1, 0, 0);    /* focus FROM */
    step(10, 0, 0, 0, 20, 0);   /* FROM 0 -> 20, pushes TO */
    check(mon_search.lo[0] == 20 && mon_search.hi[0] == 20 && strstr(row(4), "FROM:020") && strstr(row(4), "TO:020"), "FROM pushes TO");
    check(strstr(row(5), "G#0") != 0, "note names under D1");
    /* Hold OP5 from a search page switches to it. */
    step(10, K(OP5), 0, 0, 0, 0);
    step(850, 0, 0, 0, 0, 0);
    idle(); /* the new page draws on its first update */
    check(current == &mon_search_pages[MON_SEARCH_D2] && !strcmp(row(0), "SEARCH D2  OP5"), "hold OP5 switches pages");
    step(10, 0, K(OP5), 0, 0, 0);
    /* Short press of a key on a search page toggles that search. */
    int before = mon_search.on[MON_SEARCH_D2];
    tap(OP5);
    check(mon_search.on[MON_SEARCH_D2] != before && strstr(row(2), before ? "OFF" : "ON"), "short press on its page toggles STATE");

    /* Scrolled back with a search on: the view stays on the same rows. */
    ui_show(&monitor_page);
    memset(mon_search.on, 0, sizeof mon_search.on);
    mon_init();
    for (int i = 0; i < 40; ++i) log3(i % 2 ? 1 : 0, 0x90, (uint8_t)i, 1, 3); /* alternate T/U */
    mon_search.ports = 0x1; /* TRS only: 20 shown */
    tap(OP1);
    step(10, 0, 0, -5, 0, 0); /* scroll back 5 */
    char before_row[UI_COLUMNS + 1];
    strcpy(before_row, row(14));
    log3(0, 0x90, 100, 1, 3);
    log3(1, 0x90, 101, 1, 3);
    idle();
    check(!strcmp(before_row, row(14)) && strstr(row(1), "-6"), "scrolled view stays put; counts shown rows");

    /* ---- Rule builder ---- */
    /* F5 on the selected (scrolled-back) TRS note: IN set, builder opens. */
    check(leds[UI_LED_KEY(F5)] && leds[UI_LED_KEY(G5)], "F5/G5 lit on a note row");
    tap(F5);
    idle();
    check(current == &rule_builder_page && !strcmp(row(0), "BUILD MESSAGE RULE"), "F5 opens Build Message Rule");
    check(strstr(row(2), "P:TRS") && strstr(row(2), "CH:01") && strstr(row(2), "TYP:NOTE"), "F5 sets IN port, channel, type");
    check(!strcmp(row(3), "VALUE:028") && !strcmp(row(9), "NEEDS OUT") && strstr(row(6), "---"), "IN value set, OUT empty");
    check(!leds[BTN_REC], "REC dark with OUT empty");
    /* GLO back: the monitor keeps its scroll position as new MIDI arrives. */
    log3(0, 0x90, 102, 1, 3);
    glo_back();
    idle();
    check(current == &monitor_page && !strcmp(before_row, row(14)) && strstr(row(1), "-7"), "GLO back keeps the selected row");
    /* G5 on a CC: TYPE MISMATCH, REC does nothing. */
    mon_search.on[MON_SEARCH_PORT] = 0;
    log3(1, 0xB3, 11, 5, 3); /* U 04 CC 11 */
    ui_show(&monitor_page);
    idle();
    tap(G5);
    idle();
    check(!strcmp(row(9), "TYPE MISMATCH") && !leds[BTN_REC], "OUT from a CC with a NOTE IN: mismatch, REC dark");
    step(10, 0, 0, 0, 0, UI_BUTTON(BTN_REC));
    step(10, 0, 0, 0, 0, UI_BUTTON(BTN_REC));
    check(rule_count == 0, "mismatch: REC saves nothing");
    /* G5 on a note on channel 3: ready, REC twice saves and clears. */
    glo_back();
    log3(1, 0x92, 62, 90, 3); /* U 03 NON D4 */
    idle();
    check(leds[UI_LED_KEY(G5)], "G5 lit");
    tap(G5);
    idle();
    check(!strcmp(row(6), "CH:03 VALUE:062") && strstr(row(7), "D4") && !strcmp(row(9), "REC TO ADD") && leds[BTN_REC],
          "G5 sets OUT channel and value; REC lit");
    step(10, 0, 0, 0, 0, UI_BUTTON(BTN_REC));
    check(rule_count == 0 && !strcmp(row(11), "REC AGAIN TO SAVE"), "first REC arms");
    step(10, 0, 0, 0, 0, UI_BUTTON(BTN_REC));
    check(rule_count == 1 && rules[0].port == 0 && rules[0].channel == 0 && rules[0].type == REMAP_NOTE &&
          rules[0].in == 28 && rules[0].out == 62 && rules[0].out_channel == 2 && last_replace == -1,
          "second REC saves TRS 01 NOTE 28 -> 62 ch 3");
    check(!strcmp(row(9), "ADDED: TRS 01 NOTE") && !strcmp(row(3), "VALUE:---") && !leds[BTN_REC], "cleared, ADDED shown");
    step(3100, 0, 0, 0, 0, 0);
    check(!strcmp(row(9), "F5:IN  G5:OUT"), "ADDED clears after 3 s");
    /* Editing an empty half starts it from defaults: USB 01 NOTE 60. */
    step(10, 0, 0, 3, 0, 0);  /* focus IN VALUE */
    step(10, 0, 0, 0, 1, 0);  /* 60 -> 61 */
    check(strstr(row(2), "P:USB") && strstr(row(2), "CH:01") && !strcmp(row(3), "VALUE:061") && strstr(row(4), "C#4"),
          "editing an empty IN starts from USB 01 NOTE 60");
    step(10, 0, 0, 1, 0, 0);  /* focus OUT CH */
    step(10, K(ui_white_key(4)), 0, 0, 0, 0); /* white key 5 = channel 5 */
    check(!strcmp(row(6), "CH:05 VALUE:061") && leds[UI_LED_KEY(ui_white_key(4))], "white key sets OUT CH; its key lit");
    /* An armed REC is cancelled by another control. */
    step(10, 0, 0, 0, 0, UI_BUTTON(BTN_REC));
    step(10, 0, 0, 0, 1, 0);
    step(10, 0, 0, 0, 0, UI_BUTTON(BTN_REC));
    check(rule_count == 1 && !strcmp(row(11), "REC AGAIN TO SAVE"), "another control cancels the armed REC");
    /* Same input as an existing rule: REPLACES A RULE, saved over it. */
    mp_builder_set_in(0, 0, REMAP_NOTE, 28);
    idle();
    check(!strcmp(row(9), "REPLACES A RULE"), "same input: REPLACES A RULE");
    step(10, 0, 0, 0, 0, UI_BUTTON(BTN_REC));
    step(10, 0, 0, 0, 0, UI_BUTTON(BTN_REC));
    check(rule_count == 1 && rules[0].out_channel == 5 && last_replace == 0, "saved over the existing rule"); /* ch 6 after the cancel step */
    /* A full list stops the save. */
    mp_builder_set_in(1, 0, REMAP_NOTE, 10);
    mp_builder_set_out(REMAP_NOTE, 0, 20);
    forced_list_size = MP_MSG_RULES_PER_LIST;
    idle();
    step(10, 0, 0, 0, 0, UI_BUTTON(BTN_REC));
    step(10, 0, 0, 0, 0, UI_BUTTON(BTN_REC));
    check(!strcmp(row(9), "LIST FULL (32)") && rule_count == 1 && !leds[BTN_REC], "LIST FULL: REC does nothing");
    forced_list_size = 0;
    /* F5/G5 stay dark on pitch bend. */
    glo_back();
    log3(1, 0xE0, 0, 64, 3);
    idle();
    check(!leds[UI_LED_KEY(F5)] && !leds[UI_LED_KEY(G5)], "F5/G5 dark on PB");

    printf("%d/%d passed\n", checks - failures, checks);
    return failures != 0;
}
