/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * Patch SysEx: dumps and requests, as specified in design/patch-format.md.
 *
 *   F0 7D <device> 11 <command> <data> F7
 *   00 single patch dump    data: dump packet (patch 0-15, or 7F = edit buffer)
 *   01 all patch dump       data: dump packets for patches 0-15
 *   10 single patch request data: patch number 0-15 or 7F
 *   11 all patch request    no data
 *
 * A dump packet is: patch, index, count, payload (up to 98 body bytes,
 * 7-bit packed into up to 112 bytes), checksum. Each packet is a whole SysEx
 * message of at most 122 bytes, because the SDK sends at most 128.
 *
 * Receiving: packets are checked and collected one patch at a time; nothing
 * partial is applied. A complete patch for the edit buffer, or for the slot
 * in use, replaces the edit buffer at once. Patches for slots are written to
 * Flash once no dump packet has arrived for QUIET_MS, so a Flash write (which
 * pauses MIDI processing) never stalls a dump that is still arriving.
 *
 * Sending: one packet at a time, and only when the output queue is empty,
 * so live MIDI is never stuck behind more than one packet.
 *
 * Control task only. */
#include "midiproc.h"
#include <string.h>

enum {
    MANUFACTURER = 0x7d, MODEL = 0x11,
    CMD_DUMP = 0x00, CMD_DUMP_ALL = 0x01, CMD_REQUEST = 0x10, CMD_REQUEST_ALL = 0x11,
    HEADER = 5,                /* F0 7D device model command */
    BODY_PER_PACKET = 98,      /* 14 groups of 7 */
    PAYLOAD_MAX = 112,
    MAX_PACKETS = (MP_PATCH_BODY_MAX + BODY_PER_PACKET - 1) / BODY_PER_PACKET, /* 32 */
    QUIET_MS = 100,
    RX_TIMEOUT_MS = 1000,      /* a patch whose packets stop is dropped */
    PACKETS_PER_POLL = 4
};
_Static_assert(HEADER + 3 + PAYLOAD_MAX + 2 <= MP_SYSEX_FORWARD_MAX, "packet size");

static struct mp_sysex_counts counts;

/* ---- 7-bit packing ---------------------------------------------------------- */
/* Each group of up to 7 bytes: one byte of their top bits (bit i = byte i),
 * then the bytes with the top bit cleared. Returns the packed length. */
static unsigned pack(const uint8_t *in, unsigned n, uint8_t *out)
{
    unsigned w = 0;
    for (unsigned g = 0; g < n; g += 7U) {
        unsigned top = w++, len = n - g < 7U ? n - g : 7U;
        out[top] = 0;
        for (unsigned i = 0; i < len; ++i) {
            out[top] |= (uint8_t)((in[g + i] >> 7) << i);
            out[w++] = in[g + i] & 0x7fU;
        }
    }
    return w;
}

/* Returns the unpacked length, or -1 if the packing is malformed. */
static int unpack(const uint8_t *in, unsigned n, uint8_t *out)
{
    unsigned w = 0;
    for (unsigned g = 0; g < n; g += 8U) {
        unsigned len = n - g < 8U ? n - g : 8U;
        if (len < 2U) return -1; /* a group needs at least one data byte */
        uint8_t top = in[g];
        if (top >> (len - 1U)) return -1; /* top bits for bytes that aren't there */
        for (unsigned i = 1; i < len; ++i) out[w++] = (uint8_t)(in[g + i] | (((top >> (i - 1U)) & 1U) << 7));
    }
    return (int)w;
}

static uint8_t checksum(const uint8_t *p, unsigned n)
{
    unsigned sum = 0;
    while (n--) sum += *p++;
    return (uint8_t)((128U - sum % 128U) % 128U);
}

/* ---- Receiving -------------------------------------------------------------- */
static struct {
    uint8_t active, slot, next, count;
    unsigned length;
    uint8_t body[MP_PATCH_BODY_MAX];
} rx;

/* Patches waiting to be written to Flash. */
static uint8_t pending[MP_PATCHES][MP_PATCH_BODY_MAX];
static uint16_t pending_length[MP_PATCHES];
static uint16_t pending_mask;
static uint32_t last_packet_ms;

static int start_send(unsigned what, unsigned command);

static void reject(void)
{
    if (rx.active) ++counts.rejected;
    rx.active = 0;
}

static void complete(unsigned slot)
{
    if (mp_patch_receive(slot, rx.body, rx.length) != 0) { /* invalid body */
        ++counts.rejected;
        return;
    }
    ++counts.received;
    if (slot == MP_PATCH_EDIT_BUFFER) return; /* no Flash */
    memcpy(pending[slot], rx.body, rx.length);
    pending_length[slot] = (uint16_t)rx.length;
    pending_mask |= (uint16_t)(1U << slot);
}

static void receive_packet(unsigned command, const uint8_t *d, unsigned n)
{
    /* d: patch, index, count, payload..., checksum */
    last_packet_ms = fm1_millis();
    if (n < 5U || n - 4U > PAYLOAD_MAX) { reject(); return; }
    unsigned slot = d[0], index = d[1], count = d[2], payload = n - 4U;
    int slot_ok = slot < MP_PATCHES || (command == CMD_DUMP && slot == MP_PATCH_EDIT_BUFFER);
    if (!slot_ok || count < 1U || count > MAX_PACKETS || index >= count || checksum(d, n - 1U) != d[n - 1U]) {
        ++counts.rejected;
        rx.active = 0;
        return;
    }
    if (index == 0) {
        if (rx.active) ++counts.rejected; /* the previous patch never finished */
        rx.active = 1;
        rx.slot = (uint8_t)slot;
        rx.count = (uint8_t)count;
        rx.next = 0;
        rx.length = 0;
    } else if (!rx.active || slot != rx.slot || count != rx.count || index != rx.next) {
        reject(); /* missing, repeated or out-of-order packet */
        return;
    }
    uint8_t bytes[BODY_PER_PACKET + 7];
    int got = unpack(d + 3, payload, bytes);
    int last = index + 1U == count;
    if (got <= 0 || got > BODY_PER_PACKET || (!last && got != BODY_PER_PACKET) ||
        rx.length + (unsigned)got > MP_PATCH_BODY_MAX) {
        reject();
        return;
    }
    memcpy(rx.body + rx.length, bytes, (unsigned)got);
    rx.length += (unsigned)got;
    rx.next = (uint8_t)(index + 1U);
    if (last) {
        rx.active = 0;
        complete(slot);
    }
}

int mp_sysex_receive(const uint8_t *m, unsigned n)
{
    if (n < HEADER + 1U || m[0] != 0xf0 || m[1] != MANUFACTURER || m[2] != mp_device_id() || m[3] != MODEL ||
        m[n - 1U] != 0xf7)
        return 0;
    const uint8_t *d = m + HEADER;
    unsigned dn = n - HEADER - 1U;
    switch (m[4]) {
    case CMD_DUMP:
    case CMD_DUMP_ALL:
        receive_packet(m[4], d, dn);
        return 1;
    case CMD_REQUEST:
        if (dn == 1U && (d[0] < MP_PATCHES || d[0] == MP_PATCH_EDIT_BUFFER) && mp_allow_dump_request() &&
            start_send(d[0], CMD_DUMP) == 0)
            ++counts.requests;
        return 1;
    case CMD_REQUEST_ALL:
        if (dn == 0 && mp_allow_dump_request() && start_send(MP_DUMP_ALL, CMD_DUMP_ALL) == 0) ++counts.requests;
        return 1;
    default:
        return 1; /* ours, but an unknown command: ignored, not forwarded */
    }
}

/* ---- Sending ---------------------------------------------------------------- */
static struct {
    uint8_t active, command, slot, last_slot, index, count;
    unsigned length;
    uint8_t body[MP_PATCH_BODY_MAX];
} tx;

static unsigned packets_for(unsigned length)
{
    unsigned n = (length + BODY_PER_PACKET - 1U) / BODY_PER_PACKET;
    return n ? n : 1U;
}

/* Fetch the body of tx.slot: the edit buffer, a patch still waiting for
 * Flash, or the stored patch (defaults if the slot is empty). */
static void load_slot(void)
{
    unsigned s = tx.slot;
    if (s == MP_PATCH_EDIT_BUFFER) {
        tx.length = mp_patch_encode(tx.body);
    } else if ((pending_mask >> s) & 1U) {
        tx.length = pending_length[s];
        memcpy(tx.body, pending[s], tx.length);
    } else {
        tx.length = mp_patch_read(s, tx.body);
    }
    tx.index = 0;
    tx.count = (uint8_t)packets_for(tx.length);
}

static int start_send(unsigned what, unsigned command)
{
    if (tx.active) return -1;
    tx.active = 1;
    tx.command = (uint8_t)command;
    tx.slot = (uint8_t)(what == MP_DUMP_ALL ? 0U : what);
    tx.last_slot = (uint8_t)(what == MP_DUMP_ALL ? MP_PATCHES - 1U : what);
    tx.length = 0;
    tx.count = 0; /* loaded by the poll: it may read Flash */
    return 0;
}

int mp_sysex_send(unsigned what)
{
    if (what > MP_DUMP_ALL) return -1;
    return start_send(what, what == MP_DUMP_ALL ? CMD_DUMP_ALL : CMD_DUMP);
}

int mp_sysex_sending(unsigned *slot, unsigned *last)
{
    if (slot) *slot = tx.slot;
    if (last) *last = tx.last_slot;
    return tx.active;
}

static int send_packet(void)
{
    uint8_t m[MP_SYSEX_FORWARD_MAX] = {0xf0, MANUFACTURER, (uint8_t)mp_device_id(), MODEL, tx.command};
    unsigned at = tx.index * BODY_PER_PACKET;
    unsigned n = tx.length - at < BODY_PER_PACKET ? tx.length - at : BODY_PER_PACKET;
    uint8_t *d = m + HEADER;
    d[0] = tx.slot;
    d[1] = tx.index;
    d[2] = tx.count;
    unsigned payload = pack(tx.body + at, n, d + 3);
    d[3 + payload] = checksum(d, 3U + payload);
    unsigned length = HEADER + 3U + payload + 1U;
    m[length++] = 0xf7;
    return mp_send_sysex_local(m, length);
}

static void send_poll(void)
{
    for (unsigned i = 0; i < PACKETS_PER_POLL && tx.active; ++i) {
        if (tx.count == 0) load_slot(); /* next slot */
        if (!mp_output_idle()) return; /* wait: live MIDI or the last packet */
        if (send_packet() != 0) return;
        if (++tx.index < tx.count) continue;
        if (tx.slot >= tx.last_slot) {
            tx.active = 0;
        } else {
            ++tx.slot;
            tx.count = 0;
        }
    }
}

/* ---- Flash writes of received patches --------------------------------------- */
static void store_poll(void)
{
    if (rx.active && fm1_millis() - last_packet_ms >= RX_TIMEOUT_MS) reject();
    if (!pending_mask || rx.active || fm1_millis() - last_packet_ms < QUIET_MS) return;
    unsigned slot = 0;
    while (!((pending_mask >> slot) & 1U)) ++slot;
    pending_mask &= (uint16_t)~(1U << slot); /* one per iteration */
    counts.last_error = mp_patch_store(slot, pending[slot], pending_length[slot]);
}

void mp_sysex_poll(void)
{
    send_poll();
    store_poll();
}

void mp_sysex_counts(struct mp_sysex_counts *c)
{
    *c = counts;
}
