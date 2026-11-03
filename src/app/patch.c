/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * Patches: the edit buffer, the patch body encoding and Flash storage, as
 * specified in design/patch-format.md. Bodies are written as version 2
 * (version 1 plus event triggers); version 1 bodies still load, with no
 * triggers.
 *
 * The edit buffer is the live state the pipeline and pages already use:
 * mp_filters, the remap rules, the event triggers, mp_knobs (channel and
 * CC), mp_live_keys and mp_patch_name. A patch body is that state as one byte string; the same
 * bytes go to Flash and (7-bit packed) to SysEx.
 *
 * Flash: sector 0 and 1 hold the global settings (A/B copies, the higher
 * valid sequence number wins), sectors 2-17 hold patches 0-15. Each sector
 * holds one record: "FMPX", type, slot, length, sequence, body, CRC-32.
 * A sector without a valid record reads as empty, which includes the stock
 * firmware's data still in the partition: nothing is written until a patch
 * is saved or loaded.
 *
 * Flash erase and write run on the control task and pause MIDI processing
 * (input is queued meanwhile); MIDI is serviced before and between them.
 * Control task only. */
#include "midiproc.h"
#include <string.h>

/* ---- Patch body layout (patch-format.md, "Patch body") ------------------- */
enum {
    VERSION = 2, GLOBAL_VERSION = 2,
    OFF_NAME = 1, OFF_CHANNELS = 13, OFF_SYSTEM = 19, OFF_VOICE = 25,
    OFF_KNOBS = 73, OFF_LIVE = 81, OFF_CH_COUNT = 88, OFF_MSG_COUNT = 89,
    FIXED = 91, CH_RULE_BYTES = 3, MSG_RULE_BYTES = 6, BANK_OFF_BYTE = 0xff,
    /* Version 2: after the rules, trigger count (1), message count (2),
     * the triggers, then their messages in trigger order. */
    TRIG_HEAD = 3, TRIG_BYTES = 10, TRIG_MSG_BYTES = 4
};
_Static_assert(FIXED + CH_RULE_BYTES * MP_CH_RULES + MSG_RULE_BYTES * MP_MSG_RULES_TOTAL + TRIG_HEAD +
                   TRIG_BYTES * MP_TRIGGERS + TRIG_MSG_BYTES * MP_TRIG_MSGS_TOTAL ==
               MP_PATCH_BODY_MAX, "patch body size");
_Static_assert(12 + MP_PATCH_BODY_MAX + 4 <= FM1_FLASH_SECTOR_BYTES, "a patch fits one sector");

/* ---- Flash layout (patch-format.md, "Flash layout") ----------------------- */
enum {
    SECTOR = FM1_FLASH_SECTOR_BYTES,
    GLOBAL_SECTOR = 0, PATCH_SECTOR = 2, SECTORS = PATCH_SECTOR + MP_PATCHES,
    HEADER = 12, CRC_BYTES = 4,
    RECORD_GLOBAL = 1, RECORD_PATCH = 2,
    /* Global body: version, device ID, allow dump request, last patch;
     * version 2 adds the metronome: enabled, time base, beats per bar,
     * click Hz (u16), first-beat Hz (u16). Version 1 still loads. */
    GLOBAL_BYTES_V1 = 4, GLOBAL_BYTES = 11
};
static const uint8_t magic[4] = {'F', 'M', 'P', 'X'};

/* Record buffer: header, body, CRC, padded to a word. Patch bodies are
 * encoded straight into it. Also the scratch for mp_patch_dirty. */
static uint8_t record[(HEADER + MP_PATCH_BODY_MAX + CRC_BYTES + 3U) & ~3U] __attribute__((aligned(4)));
static uint8_t *const body = record + HEADER;

/* The body as last loaded or saved, to tell whether there are unsaved edits. */
static uint8_t clean[MP_PATCH_BODY_MAX];
static unsigned clean_length;

char mp_patch_name[MP_PATCH_NAME];
static unsigned current;
static unsigned generation; /* changes whenever the edit buffer is replaced */
static int storage_ok;
static uint32_t write_ms; /* Flash time of the last load or save */

/* Names of the stored patches, read once at boot and kept up to date. */
static char stored_name[MP_PATCHES][MP_PATCH_NAME];
static uint16_t stored_mask; /* bit = slot holds a valid patch */

static struct {
    uint8_t device_id, allow_dump, last_patch;
    uint32_t sequence;
    unsigned sector;          /* 0 or 1: copy written last */
} globals = {0, 1, 0, 0, 1};
static uint8_t saved_globals[GLOBAL_BYTES]; /* the global body as in Flash (or defaults) */

/* ---- Helpers --------------------------------------------------------------- */
static uint32_t crc32(const uint8_t *p, unsigned n)
{
    uint32_t c = 0xffffffffU;
    while (n--) {
        c ^= *p++;
        for (unsigned b = 0; b < 8; ++b) c = (c >> 1) ^ (0xedb88320U & (0U - (c & 1U)));
    }
    return ~c;
}

static void put16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static unsigned get16(const uint8_t *p) { return p[0] | (unsigned)p[1] << 8; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v & 0xffffU); put16(p + 2, v >> 16); }
static uint32_t get32(const uint8_t *p) { return get16(p) | (uint32_t)get16(p + 2) << 16; }

static int name_char_ok(uint8_t c)
{
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == ' ';
}

static void default_name(char name[MP_PATCH_NAME], unsigned slot)
{
    memcpy(name, "PATCH 00    ", MP_PATCH_NAME);
    name[6] = (char)('0' + (slot + 1U) / 10U);
    name[7] = (char)('0' + (slot + 1U) % 10U);
}

/* ---- Edit buffer <-> patch body ------------------------------------------- */
/* Message rules are written in a fixed order (port, channel, type, input),
 * so the same rules always give the same body whatever order they were
 * added in. */
static unsigned rule_key(const struct mp_msg_rule *r)
{
    return (unsigned)r->port << 13 | (unsigned)r->channel << 9 | (unsigned)r->type << 7 | r->in;
}

unsigned mp_patch_encode(uint8_t b[MP_PATCH_BODY_MAX])
{
    memset(b, 0, FIXED);
    b[0] = VERSION;
    memcpy(b + OFF_NAME, mp_patch_name, MP_PATCH_NAME);
    for (unsigned p = 0; p < MP_PORTS; ++p) {
        put16(b + OFF_CHANNELS + 2U * p, mp_filters.channels[p]);
        put16(b + OFF_SYSTEM + 2U * p, mp_filters.system[p]);
        memcpy(b + OFF_VOICE + 16U * p, mp_filters.voice[p], 16);
    }
    for (unsigned k = 0; k < MP_KNOBS; ++k) {
        b[OFF_KNOBS + 2U * k] = mp_knobs[k].channel;
        b[OFF_KNOBS + 2U * k + 1U] = mp_knobs[k].cc;
    }
    const struct mp_live_keys *l = &mp_live_keys;
    uint8_t *q = b + OFF_LIVE;
    q[0] = l->channel;
    q[1] = l->vel_min;
    q[2] = l->vel_max;
    q[3] = (uint8_t)l->octave;
    q[4] = l->program;
    q[5] = l->msb == MP_BANK_OFF ? BANK_OFF_BYTE : (uint8_t)l->msb;
    q[6] = l->lsb == MP_BANK_OFF ? BANK_OFF_BYTE : (uint8_t)l->lsb;

    unsigned c = mp_ch_rule_count(), m = mp_msg_rule_count();
    b[OFF_CH_COUNT] = (uint8_t)c;
    put16(b + OFF_MSG_COUNT, m);
    uint8_t *w = b + FIXED;
    for (unsigned i = 0; i < c; ++i) { /* already sorted */
        const struct mp_ch_rule *r = mp_ch_rule(i);
        *w++ = r->port;
        *w++ = r->in;
        *w++ = r->out;
    }
    static uint8_t order[MP_MSG_RULES_TOTAL];
    for (unsigned i = 0; i < m; ++i) { /* insertion sort of pool indexes */
        unsigned key = rule_key(mp_msg_rule(i)), at = i;
        while (at && rule_key(mp_msg_rule(order[at - 1U])) > key) { order[at] = order[at - 1U]; --at; }
        order[at] = (uint8_t)i;
    }
    for (unsigned i = 0; i < m; ++i) {
        const struct mp_msg_rule *r = mp_msg_rule(order[i]);
        *w++ = r->port;
        *w++ = r->channel;
        *w++ = r->type;
        *w++ = r->in;
        *w++ = r->out;
        *w++ = r->out_channel;
    }
    unsigned t = mp_trigger_count();
    *w++ = (uint8_t)t;
    put16(w, mp_trig_msg_total());
    w += 2;
    for (unsigned i = 0; i < t; ++i) {
        const struct mp_trigger *g = mp_trigger(i);
        const uint8_t bytes[TRIG_BYTES] = {g->port, g->channel, g->type, g->d1_lo, g->d1_hi,
                                           g->d2_lo, g->d2_hi, g->flags, g->count, 0};
        memcpy(w, bytes, TRIG_BYTES);
        w += TRIG_BYTES;
    }
    for (unsigned i = 0; i < t; ++i)
        for (unsigned k = 0; k < mp_trigger(i)->count; ++k) {
            const struct mp_trig_msg *s = mp_trig_msg(i, k);
            *w++ = s->type;
            *w++ = s->channel;
            *w++ = s->d1;
            *w++ = s->d2;
        }
    return (unsigned)(w - b);
}

static int bank_ok(uint8_t v) { return v <= 127U || v == BANK_OFF_BYTE; }
static int range_ok(uint8_t lo, uint8_t hi) { return lo <= hi && hi <= 127U; }
static int trig_value_ok(uint8_t v) { return v <= 127U || v == MP_TRIG_IN; }

/* The version 2 trigger section at r, ending exactly at end. */
static int triggers_valid(const uint8_t *r, const uint8_t *end)
{
    if (end - r < TRIG_HEAD) return 0;
    unsigned t = r[0], m = get16(r + 1);
    if (t > MP_TRIGGERS || m > MP_TRIG_MSGS_TOTAL) return 0;
    if ((unsigned)(end - r) != TRIG_HEAD + TRIG_BYTES * t + TRIG_MSG_BYTES * m) return 0;
    r += TRIG_HEAD;
    unsigned sum = 0;
    for (unsigned i = 0; i < t; ++i, r += TRIG_BYTES) {
        if (r[0] >= MP_PORTS || r[1] > 15U || r[2] >= TRIG_TYPES || !range_ok(r[3], r[4]) ||
            !range_ok(r[5], r[6]) || (r[7] & ~(TRIG_PASS | TRIG_ENTER)) || r[8] > MP_TRIG_MSGS || r[9])
            return 0;
        sum += r[8];
    }
    if (sum != m) return 0;
    for (unsigned i = 0; i < m; ++i, r += TRIG_MSG_BYTES)
        if (r[0] >= TRIG_TYPES || r[1] > 15U || !trig_value_ok(r[2]) || !trig_value_ok(r[3]) ||
            (r[0] == TRIG_PC && r[3] != 0))
            return 0;
    return 1;
}

/* Every field is checked before anything is applied (patch-format.md). */
static int body_valid(const uint8_t *b, unsigned n)
{
    if (n < FIXED || b[0] < 1U || b[0] > VERSION) return 0;
    unsigned c = b[OFF_CH_COUNT], m = get16(b + OFF_MSG_COUNT);
    if (c > MP_CH_RULES || m > MP_MSG_RULES_TOTAL) return 0;
    unsigned rules_end = FIXED + CH_RULE_BYTES * c + MSG_RULE_BYTES * m;
    if (b[0] == 1U ? n != rules_end : n < rules_end || !triggers_valid(b + rules_end, b + n)) return 0;
    for (unsigned i = 0; i < MP_PATCH_NAME; ++i)
        if (!name_char_ok(b[OFF_NAME + i])) return 0;
    for (unsigned p = 0; p < MP_PORTS; ++p) {
        if (get16(b + OFF_SYSTEM + 2U * p) >> SYS_TYPES) return 0;
        for (unsigned ch = 0; ch < 16U; ++ch)
            if (b[OFF_VOICE + 16U * p + ch] >> VOICE_TYPES) return 0;
    }
    for (unsigned k = 0; k < MP_KNOBS; ++k)
        if (b[OFF_KNOBS + 2U * k] > 15U || b[OFF_KNOBS + 2U * k + 1U] > MP_KNOB_MAX_CC) return 0;
    const uint8_t *q = b + OFF_LIVE;
    int8_t octave = (int8_t)q[3];
    if (q[0] > 15U || q[1] < 1U || q[1] > 127U || q[2] < 1U || q[2] > 127U || q[1] > q[2] ||
        octave < -3 || octave > 3 || q[4] > 127U || !bank_ok(q[5]) || !bank_ok(q[6]))
        return 0;
    const uint8_t *r = b + FIXED;
    for (unsigned i = 0; i < c; ++i, r += CH_RULE_BYTES)
        if (r[0] >= MP_PORTS || r[1] > 15U || r[2] > 15U) return 0;
    static uint8_t per_list[MP_PORTS][16][REMAP_TYPES];
    memset(per_list, 0, sizeof per_list);
    for (unsigned i = 0; i < m; ++i, r += MSG_RULE_BYTES) {
        if (r[0] >= MP_PORTS || r[1] > 15U || r[2] >= REMAP_TYPES || r[3] > 127U || r[4] > 127U ||
            r[5] > 15U)
            return 0;
        if (++per_list[r[0]][r[1]][r[2]] > MP_MSG_RULES_PER_LIST) return 0;
    }
    return 1;
}

int mp_patch_decode(const uint8_t *b, unsigned n)
{
    if (!body_valid(b, n)) return -1;
    memcpy(mp_patch_name, b + OFF_NAME, MP_PATCH_NAME);
    for (unsigned p = 0; p < MP_PORTS; ++p) {
        mp_filters.channels[p] = (uint16_t)get16(b + OFF_CHANNELS + 2U * p);
        mp_filters.system[p] = (uint16_t)get16(b + OFF_SYSTEM + 2U * p);
        memcpy(mp_filters.voice[p], b + OFF_VOICE + 16U * p, 16);
    }
    for (unsigned k = 0; k < MP_KNOBS; ++k) {
        mp_knobs[k].channel = b[OFF_KNOBS + 2U * k];
        mp_knobs[k].cc = b[OFF_KNOBS + 2U * k + 1U];
    }
    const uint8_t *q = b + OFF_LIVE;
    mp_live_keys = (struct mp_live_keys){q[0], q[1], q[2], (int8_t)q[3], q[4],
                                         q[5] == BANK_OFF_BYTE ? MP_BANK_OFF : (int8_t)q[5],
                                         q[6] == BANK_OFF_BYTE ? MP_BANK_OFF : (int8_t)q[6]};
    unsigned c = b[OFF_CH_COUNT], m = get16(b + OFF_MSG_COUNT);
    mp_remap_init();
    const uint8_t *r = b + FIXED;
    for (unsigned i = 0; i < c; ++i, r += CH_RULE_BYTES) {
        struct mp_ch_rule rule = {r[0], r[1], r[2]};
        (void)mp_ch_rule_save(&rule, -1);
    }
    for (unsigned i = 0; i < m; ++i, r += MSG_RULE_BYTES) {
        struct mp_msg_rule rule = {r[0], r[1], r[2], r[3], r[4], r[5]};
        (void)mp_msg_rule_save(&rule, -1);
    }
    mp_triggers_init();
    if (b[0] >= 2U) {
        unsigned t = r[0];
        const uint8_t *msg = r + TRIG_HEAD + TRIG_BYTES * t;
        r += TRIG_HEAD;
        for (unsigned i = 0; i < t; ++i, r += TRIG_BYTES) {
            struct mp_trigger g = {r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], 0, 0, 0};
            int index = mp_trigger_add(&g);
            for (unsigned k = 0; k < r[8]; ++k, msg += TRIG_MSG_BYTES) {
                struct mp_trig_msg s = {msg[0], msg[1], msg[2], msg[3]};
                (void)mp_trig_msg_insert((unsigned)index, k, &s);
            }
        }
    }
    ++generation;
    return 0;
}

int mp_patch_valid(const uint8_t *b, unsigned n) { return body_valid(b, n); }
unsigned mp_patch_generation(void) { return generation; }

/* The default patch (patch-format.md, "Defaults"): everything passes, no
 * rules, knobs on channel 1 with CC 20-23, Live Keys channel 1, velocity
 * 100-127, octave 0, program 1, bank OFF. */
unsigned mp_patch_default_body(unsigned slot, uint8_t b[MP_PATCH_BODY_MAX])
{
    memset(b, 0, FIXED + TRIG_HEAD);
    b[0] = VERSION;
    default_name((char *)b + OFF_NAME, slot);
    for (unsigned p = 0; p < MP_PORTS; ++p) {
        put16(b + OFF_CHANNELS + 2U * p, 0xffffU);
        put16(b + OFF_SYSTEM + 2U * p, (1U << SYS_TYPES) - 1U);
        memset(b + OFF_VOICE + 16U * p, (1U << VOICE_TYPES) - 1U, 16);
    }
    for (unsigned k = 0; k < MP_KNOBS; ++k) b[OFF_KNOBS + 2U * k + 1U] = (uint8_t)(20U + k);
    static const uint8_t live[7] = {0, 100, 127, 0, 0, BANK_OFF_BYTE, BANK_OFF_BYTE};
    memcpy(b + OFF_LIVE, live, sizeof live);
    return FIXED + TRIG_HEAD; /* no rules, no triggers */
}

static void load_defaults(unsigned slot)
{
    (void)mp_patch_decode(body, mp_patch_default_body(slot, body));
}

static void mark_clean(void)
{
    clean_length = mp_patch_encode(clean);
}

int mp_patch_dirty(void)
{
    unsigned n = mp_patch_encode(body);
    return n != clean_length || memcmp(body, clean, n) != 0;
}

/* ---- Flash records ---------------------------------------------------------- */
/* Read the record in `sector` into `record`. Returns its body length, or -1
 * if the sector holds no valid record of this type and slot. */
static int read_record(unsigned sector, unsigned type, unsigned slot)
{
    uint32_t at = sector * SECTOR;
    if (fm1_flash_read(at, record, HEADER) != FM1_FLASH_OK) return -1;
    unsigned n = get16(record + 6);
    if (memcmp(record, magic, 4) != 0 || record[4] != type || record[5] != slot || n > MP_PATCH_BODY_MAX)
        return -1;
    mp_service();
    if (fm1_flash_read(at + HEADER, body, n + CRC_BYTES) != FM1_FLASH_OK) return -1;
    if (crc32(record, HEADER + n) != get32(body + n)) return -1;
    return (int)n;
}

/* Write `n` body bytes, already in `body`, as the record in `sector`. */
static int write_record(unsigned sector, unsigned type, unsigned slot, unsigned n, uint32_t sequence)
{
    if (!storage_ok) return FM1_FLASH_UNAVAILABLE;
    memcpy(record, magic, 4);
    record[4] = (uint8_t)type;
    record[5] = (uint8_t)slot;
    put16(record + 6, n);
    put32(record + 8, sequence);
    put32(body + n, crc32(record, HEADER + n));
    unsigned total = HEADER + n + CRC_BYTES, padded = (total + 3U) & ~3U;
    memset(record + total, 0xff, padded - total); /* erased Flash value */

    uint32_t start = fm1_millis();
    mp_service();
    int result = fm1_flash_erase(sector * SECTOR);
    mp_service();
    if (result == FM1_FLASH_OK) result = fm1_flash_write(sector * SECTOR, record, padded);
    mp_service();
    write_ms += fm1_millis() - start;
    return result;
}

static void encode_globals(uint8_t *b)
{
    const struct mp_metronome *m = &mp_metronome;
    b[0] = GLOBAL_VERSION;
    b[1] = globals.device_id;
    b[2] = globals.allow_dump;
    b[3] = globals.last_patch;
    b[4] = m->enabled;
    b[5] = m->time_base;
    b[6] = m->beats;
    b[7] = (uint8_t)m->click_hz;
    b[8] = (uint8_t)(m->click_hz >> 8);
    b[9] = (uint8_t)m->accent_hz;
    b[10] = (uint8_t)(m->accent_hz >> 8);
}

static int write_globals(void)
{
    encode_globals(body);
    unsigned sector = globals.sector ^ 1U; /* the other copy survives a failed write */
    int result = write_record(GLOBAL_SECTOR + sector, RECORD_GLOBAL, 0, GLOBAL_BYTES, globals.sequence + 1U);
    if (result == FM1_FLASH_OK) {
        globals.sector = sector;
        ++globals.sequence;
        encode_globals(saved_globals);
    }
    return result;
}

static int hz_valid(unsigned hz)
{
    return hz >= MP_CLICK_HZ_MIN && hz <= MP_CLICK_HZ_MAX && hz % 10U == 0;
}

/* A valid global body of version 1 or 2. */
static int globals_valid(const uint8_t *b, int n)
{
    if (n < GLOBAL_BYTES_V1 || b[3] >= MP_PATCHES || b[1] > 127U || b[2] > 1U) return 0;
    if (b[0] == 1) return n == GLOBAL_BYTES_V1;
    return b[0] == 2 && n == GLOBAL_BYTES && b[4] <= 1U && b[5] < MP_TIME_BASES && b[6] >= 1U && b[6] <= 16U &&
           hz_valid(b[7] | (unsigned)b[8] << 8) && hz_valid(b[9] | (unsigned)b[10] << 8);
}

static void read_globals(void)
{
    int found = 0;
    for (unsigned s = 0; s < 2U; ++s) {
        int n = read_record(GLOBAL_SECTOR + s, RECORD_GLOBAL, 0);
        if (!globals_valid(body, n)) continue;
        uint32_t sequence = get32(record + 8);
        if (found && (int32_t)(sequence - globals.sequence) <= 0) continue;
        found = 1;
        globals.device_id = body[1];
        globals.allow_dump = body[2];
        globals.last_patch = body[3];
        globals.sequence = sequence;
        globals.sector = s;
        mp_metronome_defaults(); /* version 1 has no metronome */
        if (body[0] == 2) {
            struct mp_metronome *m = &mp_metronome;
            m->enabled = body[4];
            m->time_base = body[5];
            m->beats = body[6];
            m->click_hz = (uint16_t)(body[7] | body[8] << 8);
            m->accent_hz = (uint16_t)(body[9] | body[10] << 8);
        }
        /* A version 1 record is kept until settings next change; the
         * metronome defaults then go out with them as version 2. */
        encode_globals(saved_globals);
    }
}

/* Read a stored patch into `body`; returns its length, or -1 if empty. */
static int read_patch(unsigned slot)
{
    int n = read_record(PATCH_SECTOR + slot, RECORD_PATCH, slot);
    return n >= 0 && body_valid(body, (unsigned)n) ? n : -1;
}

/* ---- Public ------------------------------------------------------------------ */
void mp_patch_boot(void)
{
    storage_ok = fm1_flash_size() >= SECTORS * SECTOR;
    stored_mask = 0;
    mp_metronome_defaults();
    encode_globals(saved_globals); /* nothing stored: defaults count as saved */
    if (storage_ok) {
        read_globals();
        for (unsigned slot = 0; slot < MP_PATCHES; ++slot) {
            if (read_patch(slot) < 0) continue;
            memcpy(stored_name[slot], body + OFF_NAME, MP_PATCH_NAME);
            stored_mask |= (uint16_t)(1U << slot);
        }
    }
    current = globals.last_patch;
    int n = storage_ok ? read_patch(current) : -1;
    if (n < 0 || mp_patch_decode(body, (unsigned)n) != 0) load_defaults(current);
    mark_clean();
}

int mp_patch_load(unsigned slot)
{
    if (slot >= MP_PATCHES) return FM1_FLASH_INVALID;
    write_ms = 0;
    int n = storage_ok ? read_patch(slot) : -1;
    if (n < 0 || mp_patch_decode(body, (unsigned)n) != 0) load_defaults(slot);
    current = slot;
    mark_clean();
    if (globals.last_patch == slot || !storage_ok) return storage_ok ? 0 : FM1_FLASH_UNAVAILABLE;
    globals.last_patch = (uint8_t)slot;
    return write_globals();
}

int mp_patch_save(unsigned slot)
{
    if (slot >= MP_PATCHES) return FM1_FLASH_INVALID;
    write_ms = 0;
    unsigned n = mp_patch_encode(body);
    int result = write_record(PATCH_SECTOR + slot, RECORD_PATCH, slot, n, 0);
    if (result != FM1_FLASH_OK) {
        stored_mask &= (uint16_t)~(1U << slot); /* may be partly written */
        return result;
    }
    memcpy(stored_name[slot], mp_patch_name, MP_PATCH_NAME);
    stored_mask |= (uint16_t)(1U << slot);
    current = slot;
    mark_clean();
    if (globals.last_patch == slot) return 0;
    globals.last_patch = (uint8_t)slot;
    return write_globals();
}

unsigned mp_patch_read(unsigned slot, uint8_t out[MP_PATCH_BODY_MAX])
{
    int n = storage_ok && slot < MP_PATCHES ? read_patch(slot) : -1;
    if (n < 0) return mp_patch_default_body(slot, out);
    memcpy(out, body, (unsigned)n);
    return (unsigned)n;
}

int mp_patch_receive(unsigned slot, const uint8_t *b, unsigned n)
{
    if (!body_valid(b, n)) return -1;
    if (slot == MP_PATCH_EDIT_BUFFER) return mp_patch_decode(b, n); /* stays unsaved */
    if (slot != current) return 0;
    (void)mp_patch_decode(b, n); /* the patch in use was overwritten: reload it */
    mark_clean();
    return 0;
}

int mp_patch_store(unsigned slot, const uint8_t *b, unsigned n)
{
    if (slot >= MP_PATCHES || !body_valid(b, n)) return FM1_FLASH_INVALID;
    write_ms = 0;
    memcpy(body, b, n);
    int result = write_record(PATCH_SECTOR + slot, RECORD_PATCH, slot, n, 0);
    if (result != FM1_FLASH_OK) {
        stored_mask &= (uint16_t)~(1U << slot);
        return result;
    }
    memcpy(stored_name[slot], b + OFF_NAME, MP_PATCH_NAME);
    stored_mask |= (uint16_t)(1U << slot);
    return 0;
}

unsigned mp_device_id(void) { return globals.device_id; }
int mp_allow_dump_request(void) { return globals.allow_dump; }

void mp_device_settings_set(unsigned device_id, int allow)
{
    if (device_id <= 127U) globals.device_id = (uint8_t)device_id;
    globals.allow_dump = (uint8_t)!!allow;
}

int mp_global_settings_save(void)
{
    uint8_t now[GLOBAL_BYTES];
    encode_globals(now);
    if (!memcmp(now, saved_globals, GLOBAL_BYTES)) return 0;
    if (!storage_ok) return FM1_FLASH_UNAVAILABLE;
    write_ms = 0;
    return write_globals();
}

int mp_device_settings_save(void) { return mp_global_settings_save(); }

unsigned mp_patch_current(void) { return current; }
int mp_patch_storage_ok(void) { return storage_ok; }
uint32_t mp_patch_write_ms(void) { return write_ms; }

const char *mp_patch_stored_name(unsigned slot)
{
    return slot < MP_PATCHES && ((stored_mask >> slot) & 1U) ? stored_name[slot] : 0;
}
