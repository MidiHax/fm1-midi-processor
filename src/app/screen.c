/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * Display: a 20 x 15 text grid, panel input and the control-task UI entry points.
 *
 * Build.ps1 compiles this file without LTO and with -Oz (by name). Keep the
 * file name: groove_screen_init/control are called by the engine library. */
#include "midiproc.h"
#include "version.h"
#include <string.h>

/* 5x7 glyphs from the FM1 Open firmware. Five columns per glyph, bit 0 at
 * the top. Digits followed by uppercase. */
static const uint8_t font[36][5] = {
    {0x3e, 0x51, 0x49, 0x45, 0x3e}, {0, 0x42, 0x7f, 0x40, 0},       {0x42, 0x61, 0x51, 0x49, 0x46},
    {0x21, 0x41, 0x45, 0x4b, 0x31}, {0x18, 0x14, 0x12, 0x7f, 0x10}, {0x27, 0x45, 0x45, 0x45, 0x39},
    {0x3c, 0x4a, 0x49, 0x49, 0x30}, {0x01, 0x71, 0x09, 0x05, 0x03}, {0x36, 0x49, 0x49, 0x49, 0x36},
    {0x06, 0x49, 0x49, 0x29, 0x1e}, {0x7e, 0x11, 0x11, 0x11, 0x7e}, {0x7f, 0x49, 0x49, 0x49, 0x36},
    {0x3e, 0x41, 0x41, 0x41, 0x22}, {0x7f, 0x41, 0x41, 0x22, 0x1c}, {0x7f, 0x49, 0x49, 0x49, 0x41},
    {0x7f, 0x09, 0x09, 0x09, 0x01}, {0x3e, 0x41, 0x49, 0x49, 0x7a}, {0x7f, 0x08, 0x08, 0x08, 0x7f},
    {0, 0x41, 0x7f, 0x41, 0},       {0x20, 0x40, 0x41, 0x3f, 0x01}, {0x7f, 0x08, 0x14, 0x22, 0x41},
    {0x7f, 0x40, 0x40, 0x40, 0x40}, {0x7f, 0x02, 0x0c, 0x02, 0x7f}, {0x7f, 0x04, 0x08, 0x10, 0x7f},
    {0x3e, 0x41, 0x41, 0x41, 0x3e}, {0x7f, 0x09, 0x09, 0x09, 0x06}, {0x3e, 0x41, 0x51, 0x21, 0x5e},
    {0x7f, 0x09, 0x19, 0x29, 0x46}, {0x46, 0x49, 0x49, 0x49, 0x31}, {0x01, 0x01, 0x7f, 0x01, 0x01},
    {0x3f, 0x40, 0x40, 0x40, 0x3f}, {0x1f, 0x20, 0x40, 0x20, 0x1f}, {0x3f, 0x40, 0x38, 0x40, 0x3f},
    {0x63, 0x14, 0x08, 0x14, 0x63}, {0x07, 0x08, 0x70, 0x08, 0x07}, {0x61, 0x51, 0x49, 0x45, 0x43}};
/* ASCII code followed by five columns. */
static const uint8_t punctuation[][6] = {
    {45,8,8,8,8,8}, {47,0x60,0x18,6,1,0}, {43,8,8,0x3e,8,8},
    {35,0x14,0x7f,0x14,0x7f,0x14}, {37,0x63,0x13,8,0x64,0x63},
    {62,0x41,0x22,0x14,8,0}, {63,2,1,0x51,9,6},
    {33,0,0,0x5f,0,0}, {34,0,7,0,7,0}, {36,0x24,0x2a,0x7f,0x2a,0x12},
    {38,0x36,0x49,0x55,0x22,0x50}, {39,0,5,3,0,0}, {40,0,0x1c,0x22,0x41,0},
    {41,0,0x41,0x22,0x1c,0}, {42,0x14,8,0x3e,8,0x14}, {44,0,0x50,0x30,0,0},
    {46,0,0x60,0x60,0,0}, {58,0,0x36,0x36,0,0}, {59,0,0x56,0x36,0,0},
    {60,8,0x14,0x22,0x41,0}, {61,0x14,0x14,0x14,0x14,0x14},
    {64,0x32,0x49,0x79,0x41,0x3e}, {91,0,0x7f,0x41,0x41,0},
    {92,1,6,0x18,0x60,0}, {93,0,0x41,0x41,0x7f,0}, {94,4,2,1,2,4},
    {95,0x40,0x40,0x40,0x40,0x40}, {96,0,1,2,4,0}, {123,0,8,0x36,0x41,0},
    {124,0,0,0x7f,0,0}, {125,0,0x41,0x36,8,0}, {126,8,4,8,0x10,8}
};

static const uint8_t *glyph(char c)
{
    static const uint8_t blank[5] = {0};
    if (c >= '0' && c <= '9') return font[c - '0'];
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z') return font[10 + c - 'A'];
    for (unsigned i = 0; i < sizeof punctuation / sizeof punctuation[0]; ++i)
        if ((uint8_t)c == punctuation[i][0]) return punctuation[i] + 1;
    return blank;
}

/* ---- Text grid ----------------------------------------------------------
 * Cell 12 x 16 px: glyph 10 x 14 (2x), one blank column pair on the right,
 * one blank row above and below. 20 cells = 240 px; 15 rows = 240 px.
 * A row line is drawn on the cell's bottom pixel row, below the glyphs. */
enum { CELL_W = 12, CELL_H = 16, SCALE = 2, GLYPH_TOP = 1 };
_Static_assert(UI_COLUMNS * CELL_W == FM1_SCREEN_WIDTH, "grid width");
_Static_assert(UI_ROWS * CELL_H == FM1_SCREEN_HEIGHT, "grid height");
_Static_assert(FM1_SCREEN_WIDTH * CELL_H <= FM1_SCREEN_DMA_PIXELS, "band size");

static char cells[UI_ROWS][UI_COLUMNS];
static uint16_t ink[UI_ROWS][UI_COLUMNS], paper[UI_ROWS][UI_COLUMNS];
static uint16_t row_paper[UI_ROWS], row_line[UI_ROWS];
static uint16_t row_has_line; /* bit per row */
static uint16_t row_has_bar;  /* bit per row */
static uint8_t bar_width[UI_ROWS];
static uint16_t bar_color[UI_ROWS], bar_track[UI_ROWS];
enum { BAR_TOP = 5, BAR_BOTTOM = 10 }; /* pixel rows within the cell */
/* 4x text (ui_big): a top row and the row below it. */
enum { BIG_CHARS = 10, BIG_SCALE = 4, BIG_CELL_W = 24, BIG_GLYPH_TOP = 2 };
static uint16_t big_rows;     /* bit per top row */
static char big_text[UI_ROWS][BIG_CHARS];
static uint16_t big_ink[UI_ROWS];
static uint16_t dirty_rows;   /* bit per row */
static unsigned next_row;     /* round-robin start for flushing */
static uint16_t band[FM1_SCREEN_WIDTH * CELL_H] __attribute__((aligned(32)));

static void set_cell(unsigned row, unsigned column, char c, uint16_t fg, uint16_t bg)
{
    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (cells[row][column] == c && paper[row][column] == bg && (ink[row][column] == fg || c == ' '))
        return;
    cells[row][column] = c;
    ink[row][column] = fg;
    paper[row][column] = bg;
    dirty_rows |= (uint16_t)(1U << row);
}

void ui_clear(void)
{
    dirty_rows |= row_has_bar | big_rows | (uint16_t)(big_rows << 1);
    row_has_bar = 0;
    big_rows = 0;
    for (unsigned r = 0; r < UI_ROWS; ++r) {
        ui_row_style(r, UI_BLACK, 0, 0);
        ui_clear_to_end(r, 0);
    }
}

void ui_clear_to_end(unsigned row, unsigned column)
{
    if (row >= UI_ROWS) return;
    for (; column < UI_COLUMNS; ++column) set_cell(row, column, ' ', UI_BLACK, row_paper[row]);
}

void ui_print_colors(unsigned row, unsigned column, const char *s, uint16_t fg, uint16_t bg)
{
    if (row >= UI_ROWS || !s) return;
    for (; *s && column < UI_COLUMNS; ++s, ++column) set_cell(row, column, *s, fg, bg);
}

void ui_print(unsigned row, unsigned column, const char *s, uint16_t fg)
{
    if (row < UI_ROWS) ui_print_colors(row, column, s, fg, row_paper[row]);
}

void ui_row_style(unsigned row, uint16_t background, int line, uint16_t line_color)
{
    if (row >= UI_ROWS) return;
    uint16_t bit = (uint16_t)(1U << row);
    if (row_paper[row] != background) {
        for (unsigned c = 0; c < UI_COLUMNS; ++c)
            if (paper[row][c] == row_paper[row]) paper[row][c] = background;
        row_paper[row] = background;
        dirty_rows |= bit;
    }
    if (!!(row_has_line & bit) != !!line || (line && row_line[row] != line_color)) {
        row_has_line = (uint16_t)(line ? row_has_line | bit : row_has_line & ~bit);
        row_line[row] = line_color;
        dirty_rows |= bit;
    }
}

void ui_header(const char *title, unsigned keep_from)
{
    ui_row_style(0, UI_PANEL, 1, UI_DIM);
    ui_print(0, 0, title, UI_PURPLE);
    unsigned column = (unsigned)strlen(title);
    for (; column < keep_from && column < UI_COLUMNS; ++column) set_cell(0, column, ' ', UI_BLACK, UI_PANEL);
}

void ui_field(unsigned row, unsigned column, const char *text, int focused)
{
    if (row >= UI_ROWS) return;
    ui_print_colors(row, column, text, focused ? UI_BLACK : UI_BLUE, focused ? UI_BLUE : row_paper[row]);
}

void ui_row_bar(unsigned row, unsigned width, uint16_t color, uint16_t track)
{
    if (row >= UI_ROWS) return;
    if (width > FM1_SCREEN_WIDTH) width = FM1_SCREEN_WIDTH;
    uint16_t bit = (uint16_t)(1U << row);
    if ((row_has_bar & bit) && bar_width[row] == width && bar_color[row] == color && bar_track[row] == track)
        return;
    row_has_bar |= bit;
    bar_width[row] = (uint8_t)width;
    bar_color[row] = color;
    bar_track[row] = track;
    dirty_rows |= bit;
}

void ui_big(unsigned row, const char *text, uint16_t color)
{
    if (row + 1U >= UI_ROWS || !text) return;
    char t[BIG_CHARS];
    unsigned i = 0;
    for (; i < BIG_CHARS && text[i]; ++i) t[i] = text[i];
    for (; i < BIG_CHARS; ++i) t[i] = ' ';
    uint16_t bit = (uint16_t)(1U << row);
    if ((big_rows & bit) && big_ink[row] == color && !memcmp(big_text[row], t, BIG_CHARS)) return;
    memcpy(big_text[row], t, BIG_CHARS);
    big_ink[row] = color;
    big_rows |= bit;
    dirty_rows |= (uint16_t)(3U << row);
}

/* One half (0 top, 1 bottom) of a 4x text line. */
static void render_big(unsigned top, unsigned half)
{
    uint16_t fg = big_ink[top], bg = row_paper[top];
    for (unsigned column = 0; column < BIG_CHARS; ++column) {
        if (column % 3U == 0) mp_service();
        const uint8_t *shape = glyph(big_text[top][column]);
        for (unsigned y = 0; y < CELL_H; ++y) {
            unsigned yy = half * CELL_H + y;
            unsigned bit = yy >= BIG_GLYPH_TOP ? (yy - BIG_GLYPH_TOP) / BIG_SCALE : 7U;
            uint16_t *pixel = band + y * FM1_SCREEN_WIDTH + column * BIG_CELL_W;
            for (unsigned x = 0; x < BIG_CELL_W; ++x) {
                unsigned col = x / BIG_SCALE;
                *pixel++ = bit < 7U && col < 5U && (shape[col] & (1U << bit)) ? fg : bg;
            }
        }
    }
}

/* Drawing a row takes about 0.8 ms, so MIDI is serviced every 5 cells to
 * keep it from waiting behind the screen. mp_service only touches the
 * monitor's event ring, never the cells or the band buffer. */
static void render_row(unsigned row)
{
    if (big_rows & (1U << row)) { render_big(row, 0); return; }
    if (row && (big_rows & (1U << (row - 1U)))) { render_big(row - 1U, 1); return; }
    for (unsigned column = 0; column < UI_COLUMNS; ++column) {
        if (column % 5U == 0) mp_service();
        const uint8_t *shape = glyph(cells[row][column]);
        uint16_t fg = ink[row][column], bg = paper[row][column];
        for (unsigned y = 0; y < CELL_H; ++y) {
            uint16_t *pixel = band + y * FM1_SCREEN_WIDTH + column * CELL_W;
            if (y == CELL_H - 1U && (row_has_line & (1U << row))) {
                for (unsigned x = 0; x < CELL_W; ++x) *pixel++ = row_line[row];
                continue;
            }
            if (y >= BAR_TOP && y <= BAR_BOTTOM && (row_has_bar & (1U << row))) {
                for (unsigned x = 0; x < CELL_W; ++x)
                    *pixel++ = column * CELL_W + x < bar_width[row] ? bar_color[row] : bar_track[row];
                continue;
            }
            unsigned bit = y >= GLYPH_TOP ? (y - GLYPH_TOP) / SCALE : 7U;
            for (unsigned x = 0; x < CELL_W; ++x) {
                unsigned col = x / SCALE;
                *pixel++ = bit < 7U && col < 5U && (shape[col] & (1U << bit)) ? fg : bg;
            }
        }
    }
}

/* One band per call, only once the previous DMA has finished, so drawing
 * never holds up MIDI on the control task (same approach as the stock UI). */
void ui_flush(void)
{
    int pending = fm1_screen_pending();
    if (pending > 0) return;
    if (pending < 0) dirty_rows = (uint16_t)((1U << UI_ROWS) - 1U); /* DMA timed out: redraw all */
    if (!dirty_rows) return;
    unsigned row = next_row;
    while (!(dirty_rows & (1U << row))) row = (row + 1U) % UI_ROWS;
    render_row(row);
    if (fm1_screen_submit(0, row * CELL_H, FM1_SCREEN_WIDTH, CELL_H, band,
                          sizeof band / sizeof band[0]) == 0) {
        dirty_rows &= (uint16_t)~(1U << row);
        next_row = (row + 1U) % UI_ROWS;
    }
}

unsigned mp_format_uint(char *out, uint32_t value)
{
    char digits[10];
    unsigned n = 0;
    do { digits[n++] = (char)('0' + value % 10U); value /= 10U; } while (value);
    for (unsigned i = 0; i < n; ++i) out[i] = digits[n - 1 - i];
    out[n] = 0;
    return n;
}

unsigned mp_format_padded(char *out, uint32_t value, unsigned width)
{
    char digits[11];
    unsigned n = mp_format_uint(digits, value), pad = n < width ? width - n : 0;
    memset(out, '0', pad);
    memcpy(out + pad, digits, n + 1U);
    return pad + n;
}

/* ---- LEDs ----------------------------------------------------------------- */
enum { LEDS = 41 };
static int8_t led_state[LEDS]; /* last state sent; -1 unknown */

void ui_led(unsigned led, int on)
{
    if (led >= LEDS || led_state[led] == !!on) return;
    fm1_led_set(led, !!on);
    led_state[led] = (int8_t)!!on;
}

static void leds_off(void)
{
    for (unsigned i = 0; i < LEDS; ++i) ui_led(i, 0);
}

/* ---- Keys ------------------------------------------------------------------ */
static int is_white(unsigned key)
{
    static const uint16_t white = 0xab5; /* C D E F G A B, bit = semitone */
    return (white >> ((UI_KEY_LOWEST_NOTE + key) % 12U)) & 1U;
}

int ui_white_key_index(unsigned key)
{
    if (key >= UI_KEYS || !is_white(key)) return -1;
    int n = 0;
    for (unsigned k = 0; k < key; ++k) n += is_white(k);
    return n;
}

unsigned ui_white_key(unsigned n)
{
    for (unsigned k = 0; k < UI_KEYS; ++k)
        if (is_white(k) && n-- == 0) return k;
    return 0;
}

unsigned ui_focus_move(unsigned focus, unsigned fields, const struct ui_input *in)
{
    int d = in->encoders[ENC_SELECT];
    if (!d || !fields) return focus;
    return (unsigned)(((int)focus + d % (int)fields + (int)fields) % (int)fields);
}

enum { CONFIRM_MS = 5000, CONFIRM_FLASH_MS = 100 };

int ui_confirm_press(struct ui_confirm *c, int pressed, uint32_t now)
{
    if (c->armed && now - c->at >= CONFIRM_MS) c->armed = 0;
    if (!pressed) return 0;
    if (c->armed) {
        c->armed = 0;
        return 1;
    }
    c->armed = 1;
    c->at = now;
    return 0;
}

int ui_confirm_led(struct ui_confirm *c, uint32_t now, int idle)
{
    if (c->armed && now - c->at >= CONFIRM_MS) c->armed = 0;
    return c->armed ? ((now - c->at) / CONFIRM_FLASH_MS) % 2U == 0 : idle;
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
    for (unsigned key = 0; key < UI_KEYS; ++key)
        if (in->keys_pressed & (1UL << key)) {
            int n = ui_white_key_index(key);
            if (n >= 0) return n;
        }
    return -1;
}

/* ---- Panel input ---------------------------------------------------------- */
enum { REPEAT_DELAY_MS = 400, REPEAT_RATE_MS = 80, FRAME_MS = 50 };
static uint32_t previous_buttons, previous_keys, repeat_next[14];

static void read_input(struct ui_input *in, const struct fm1_controls *c)
{
    in->now = fm1_millis();
    in->held = c->buttons;
    in->pressed = c->buttons & ~previous_buttons;
    in->repeat = in->pressed;
    for (unsigned b = 0; b < 14; ++b) {
        uint32_t bit = UI_BUTTON(b);
        if (in->pressed & bit) repeat_next[b] = in->now + REPEAT_DELAY_MS;
        else if ((in->held & bit) && (int32_t)(in->now - repeat_next[b]) >= 0) {
            in->repeat |= bit;
            repeat_next[b] = in->now + REPEAT_RATE_MS;
        }
    }
    previous_buttons = c->buttons;
    in->keys = c->keys & ((1UL << UI_KEYS) - 1U);
    in->keys_pressed = in->keys & ~previous_keys;
    in->keys_released = previous_keys & ~in->keys;
    previous_keys = in->keys;
    memcpy(in->encoders, c->encoders, sizeof in->encoders);
}

/* ---- SDK hooks (called by the engine library on the control task) ------- */
static uint32_t last_frame;

/* Library: maps the MASTER knob ADC to the output level (USB/Bluetooth audio
 * pass-through). */
int groove_master_update(unsigned adc);
/* Library: engine-wide render counters. Reading them also keeps
 * fm1_master_gain linked, which Build.ps1 requires. */
struct fm1_render_stats {
    uint32_t render_ticks, peak_ticks, midi_ticks, adapted;
    uint32_t overloads, voice_capacity, master_gain, deadline_ticks;
};
void groove_audio_stats(struct fm1_render_stats *stats);

/* ---- Pages ------------------------------------------------------------------
 * A screen button owns a cycle of pages: pressing it again moves to the next
 * one, pressing it from another group opens the group's first page
 * (design/decisions.md, "Screen navigation"). */
static const struct ui_page *const home_pages[] = {&monitor_page};
static const struct ui_page *const edit_pages[] = {&knobs_page, &metronome_page};
/* FX: the pipeline stages in order (remaps to follow). */
static const struct ui_page *const fx_pages[] = {&channel_filter_page, &message_filter_page,
                                                 &message_remap_page, &channel_remap_page};
static const struct ui_page *const arp_pages[] = {&live_keys_page};
static const struct ui_page *const save_pages[] = {&patch_select_page, &sysex_dump_page};
static const struct ui_page *const sel_pages[] = {&event_triggers_page};
static const struct ui_page *const seq_pages[] = {&clock_monitor_page};
static const struct {
    enum ui_button button;
    const struct ui_page *const *pages;
    unsigned count;
} groups[] = {
    {BTN_HOME, home_pages, sizeof home_pages / sizeof home_pages[0]},
    {BTN_EDIT, edit_pages, sizeof edit_pages / sizeof edit_pages[0]},
    {BTN_FX, fx_pages, sizeof fx_pages / sizeof fx_pages[0]},
    {BTN_ARP, arp_pages, sizeof arp_pages / sizeof arp_pages[0]},
    {BTN_SAVE, save_pages, sizeof save_pages / sizeof save_pages[0]},
    {BTN_SEL, sel_pages, sizeof sel_pages / sizeof sel_pages[0]},
    {BTN_SEQ, seq_pages, sizeof seq_pages / sizeof seq_pages[0]},
};

/* The top-level page a sub-page (Add Rule, trigger pages) belongs to. */
static const struct ui_page *root_of(const struct ui_page *p)
{
    while (p->parent) p = p->parent;
    return p;
}
static const struct ui_page *page;
/* GLO: the page the builder was opened from, to return to. */
static const struct ui_page *builder_return;
static uint8_t returning_from_builder;

static void show(const struct ui_page *next)
{
    if (page && page->leave) page->leave();
    leds_off();
    page = next;
    page->enter();
}

void ui_open_builder(void)
{
    if (page == &rule_builder_page) return;
    builder_return = page;
    show(&rule_builder_page);
}

int ui_returning_from_builder(void)
{
    return returning_from_builder;
}

/* GLO on the builder: back to the page it was opened from. */
static void leave_builder(void)
{
    returning_from_builder = 1;
    show(builder_return ? builder_return : &monitor_page);
    returning_from_builder = 0;
}

/* Buttons that open pages are lit on every page (cached, so cheap). */
static void light_page_buttons(void)
{
    for (unsigned g = 0; g < sizeof groups / sizeof groups[0]; ++g) ui_led(groups[g].button, 1);
    ui_led(BTN_GLO, 1);
}

void ui_show(const struct ui_page *next)
{
    show(next);
}

static void navigate(const struct ui_input *in)
{
    if (in->pressed & UI_BUTTON(BTN_GLO)) {
        if (page == &rule_builder_page) leave_builder();
        else ui_open_builder();
        return;
    }
    for (unsigned g = 0; g < sizeof groups / sizeof groups[0]; ++g) {
        if (!(in->pressed & UI_BUTTON(groups[g].button))) continue;
        if (page->parent) { /* a sub-page: its group's button goes back one level */
            for (unsigned i = 0; i < groups[g].count; ++i)
                if (groups[g].pages[i] == root_of(page)) {
                    show(page->parent);
                    return;
                }
        }
        unsigned next = 0;
        for (unsigned i = 0; i < groups[g].count; ++i)
            if (groups[g].pages[i] == page) next = (i + 1U) % groups[g].count;
        show(groups[g].pages[next]);
        return;
    }
}

/* ---- Splash ------------------------------------------------------------------
 * Shown for 2 s at power-up, then the monitor. MIDI is processed (and
 * logged) as usual meanwhile; the panel buttons do nothing until it ends. */
enum { SPLASH_MS = 2000 };
static uint32_t splash_at;

static void splash_centered(unsigned row, const char *text, uint16_t color)
{
    ui_print(row, (UI_COLUMNS - (unsigned)strlen(text)) / 2U, text, color);
}

static void splash_enter(void)
{
    ui_clear();
    splash_at = fm1_millis();
}

static void splash_update(const struct ui_input *in, int redraw)
{
    if (in->now - splash_at >= SPLASH_MS) {
        show(&monitor_page);
        return;
    }
    if (!redraw) return;
    ui_big(3, "   FM-1", UI_PURPLE);
    splash_centered(6, "MIDI PROCESSOR", UI_WHITE);
    splash_centered(9, "BY MIDIHAX", UI_ACCENT);
    splash_centered(12, "VERSION " MIDIPROC_VERSION, UI_DIM);
}

static const struct ui_page splash_page = {splash_enter, splash_update, 0, 0};

void groove_screen_init(void)
{
    fm1_controls_init();
    memset(led_state, -1, sizeof led_state);
    leds_off();
    fm1_screen_init();
    memset(cells, ' ', sizeof cells);
    memset(ink, 0, sizeof ink);
    memset(paper, 0, sizeof paper);
    memset(row_paper, 0, sizeof row_paper);
    row_has_line = row_has_bar = 0;
    dirty_rows = (uint16_t)((1U << UI_ROWS) - 1U);
    previous_buttons = previous_keys = 0;
    knobs_init();
    page = 0;
    show(&splash_page);
    struct ui_input none;
    memset(&none, 0, sizeof none);
    none.now = fm1_millis();
    page->update(&none, 1);
    last_frame = none.now;
}

/* Last of the app hooks in each control iteration. MIDI is serviced again
 * here, before and after the screen work, because the library drops wake-up
 * requests made during an iteration (see "Control wake-ups" in
 * midi_router.c). */
void groove_screen_control(void)
{
    static uint8_t booted;
    if (!booted) { /* Flash is read on the control task, after every init */
        booted = 1;
        mp_patch_boot();
    }
    mp_service();
    mp_sysex_poll(); /* dumps out, received patches to Flash */
    uint32_t start = mp_time_us();
    struct fm1_controls controls;
    fm1_controls_read(&controls);
    (void)groove_master_update(controls.knob);

    struct ui_input in;
    read_input(&in, &controls);
    knobs_service(&in); /* MIDI first: the knobs send from every page */
    const struct ui_page *before = page;
    if (page != &splash_page) navigate(&in);
    /* The edit buffer was replaced (a load or a received dump): a sub-page's
     * rule or trigger index may no longer hold, so go back to its list. */
    static unsigned generation;
    if (generation != mp_patch_generation()) {
        generation = mp_patch_generation();
        if (page->parent) show(root_of(page)); /* other pages redraw from live state */
        if (builder_return) builder_return = root_of(builder_return);
    }

    /* Input is handled on every call. The page is redrawn at most every
     * 50 ms, or at once after any control change so editing feels direct. */
    int touched = in.repeat || in.keys_pressed || in.keys_released || page != before;
    for (unsigned e = 0; e < 7U; ++e) touched |= in.encoders[e] != 0;
    int redraw = touched || in.now - last_frame >= FRAME_MS;
    if (redraw) {
        last_frame = in.now;
        struct fm1_render_stats render;
        groove_audio_stats(&render); /* not shown; see declaration */
    }
    page->update(&in, redraw);
    light_page_buttons();
    mp_service();
    ui_flush();
    mp_note_screen_time(start, mp_time_us());
    mp_end_of_iteration(); /* must stay last */
}
