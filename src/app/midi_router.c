/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * MIDI I/O: routing-task input queue, control-task processing and USB output.
 *
 * Build.ps1 compiles this file without LTO (by name), which keeps the
 * fm1_app_midi_received override from losing to the SDK's weak default.
 * Keep the file name. */
#include "midiproc.h"
#include "engine.h"
#include <string.h>

static void barrier(void)
{
    __asm__ volatile("csync" ::: "memory");
}

unsigned mp_message_length(uint8_t status)
{
    if (status >= 0xf8) return 1;                       /* real time */
    if (status == 0xf1 || status == 0xf3) return 2;     /* MTC QF, song select */
    if (status == 0xf2) return 3;                       /* song position */
    if (status >= 0xf0) return 1;                       /* tune request etc. */
    return (status & 0xe0U) == 0xc0U ? 2 : 3;           /* PC / channel AT */
}

/* ---- Time ---------------------------------------------------------------
 * fm1_millis() is the 10 ms OS tick plus an interpolation from the tick
 * timer's counter. Reading that counter directly gives microseconds. The
 * register addresses are the ones fm1_millis reads (count, then period). */
#define TICK_COUNT (*(volatile uint32_t *)0x1eef0f0)
#define TICK_PERIOD (*(volatile uint32_t *)0x1eef0f4)

uint32_t mp_time_us(void)
{
    for (unsigned tries = 0; tries < 4U; ++tries) {
        uint32_t before = TICK_COUNT;
        uint32_t ms = fm1_millis();
        uint32_t count = TICK_COUNT, period = TICK_PERIOD;
        if (!period) break;
        if (count >= before) /* no tick boundary while reading */
            return ms / 10U * 10000U + (uint32_t)((uint64_t)count * 10000U / period);
    }
    return fm1_millis() * 1000U;
}

/* ---- Input: routing task (producer) -> control task (consumer) ---------- */
enum { IN_QUEUE = 256 };
static struct {
    volatile unsigned read, write, lost;
    uint32_t data[IN_QUEUE];
    uint32_t stamp[IN_QUEUE]; /* mp_time_us() on arrival */
} input __attribute__((aligned(32)));

/* SysEx bytes, routing task -> control task. Each stored frame's input
 * queue entry follows its bytes, and frames are consumed in queue order. */
enum { SYSEX_RING = 2048 };
static struct {
    volatile unsigned read, write;
    uint8_t data[SYSEX_RING];
} sysex_in;

/* Input queue entry: status | d1 << 8 | d2 << 16 | length << 24 | source << 28.
 * SysEx has length 0, its byte count in d1 (low 7 bits) and d2 bits 0-3,
 * and d2 bit 4 set when its bytes are in sysex_in. Routing task only. */
static void input_push(uint32_t entry)
{
    unsigned w = input.write;
    input.data[w & (IN_QUEUE - 1)] = entry;
    input.stamp[w & (IN_QUEUE - 1)] = mp_time_us();
    barrier();
    input.write = w + 1U;
    fm1_control_request();
}

/* Routing task. No blocking, Flash, UI or allocation here. */
void fm1_app_midi_received(unsigned source, const uint8_t *m, size_t n)
{
    if (!m || !n || n > 3 || source > FM1_MIDI_BLE || m[0] < 0x80) return;
    /* Active Sensing and the undefined F4, F5, F9, FD: dropped on arrival,
     * never processed, shown or passed through. */
    if (m[0] == 0xfe || m[0] == 0xf4 || m[0] == 0xf5 || m[0] == 0xf9 || m[0] == 0xfd) return;
    if (input.write - input.read >= IN_QUEUE) { ++input.lost; return; }
    input_push((uint32_t)m[0] | ((uint32_t)(n > 1 ? m[1] : 0) << 8) |
               ((uint32_t)(n > 2 ? m[2] : 0) << 16) | ((uint32_t)n << 24) | ((uint32_t)source << 28));
}

/* Routing task (fm1_app_sysex runs there too: the library delivers SysEx
 * and short messages from the same function). Frames up to
 * MP_SYSEX_FORWARD_MAX bytes are stored for forwarding; longer ones are
 * only logged. */
void mp_input_sysex(unsigned source, const uint8_t *m, size_t n)
{
    if (!m || n < 2 || n > 1024 || source > FM1_MIDI_BLE) return;
    if (input.write - input.read >= IN_QUEUE) { ++input.lost; return; }
    uint32_t stored = 0;
    unsigned w = sysex_in.write;
    if (n <= MP_SYSEX_FORWARD_MAX && SYSEX_RING - (w - sysex_in.read) >= n) {
        for (size_t i = 0; i < n; ++i) sysex_in.data[(w + i) & (SYSEX_RING - 1)] = m[i];
        barrier();
        sysex_in.write = w + (unsigned)n;
        stored = 1;
    }
    input_push(0xf0U | ((uint32_t)(n & 0x7fU) << 8) | ((uint32_t)((n >> 7) & 0x0fU) << 16) |
               (stored << 20) | ((uint32_t)source << 28));
}

static uint32_t current_stamp; /* arrival time of the event being processed */

static int input_pop(struct mp_event *e)
{
    unsigned r = input.read;
    if (r == input.write) return 0;
    barrier();
    uint32_t v = input.data[r & (IN_QUEUE - 1)];
    current_stamp = input.stamp[r & (IN_QUEUE - 1)];
    barrier();
    input.read = r + 1U;
    e->data[0] = (uint8_t)v;
    e->data[1] = (uint8_t)(v >> 8);
    e->data[2] = (uint8_t)(v >> 16);
    e->length = (uint8_t)((v >> 24) & 3U);
    e->source = (uint8_t)(v >> 28);
    e->sysex_bytes = 0;
    e->sysex_stored = 0;
    e->fate = MP_PASSED;
    e->stamp = current_stamp;
    if (e->length == 0) { /* SysEx */
        e->sysex_bytes = (uint16_t)(e->data[1] | (e->data[2] & 0x0fU) << 7);
        e->sysex_stored = (uint8_t)((e->data[2] >> 4) & 1U);
        e->data[1] = e->data[2] = 0;
    }
    return 1;
}

/* After processing: free a stored frame's bytes, whether or not it was sent. */
static void sysex_consume(const struct mp_event *e)
{
    if (e->length == 0 && e->sysex_stored) {
        barrier();
        sysex_in.read += e->sysex_bytes;
    }
}

/* ---- Latency statistics: control task only ------------------------------ */
/* Bucket upper bounds in microseconds; the last bucket is everything above. */
static const uint32_t bucket_limit[] = {50, 100, 200, 500, 1000, 2000, 5000};
enum { BUCKETS = sizeof bucket_limit / sizeof bucket_limit[0] + 1 };
static struct {
    uint32_t count, sum_us, max_us, retries, screen_max_us, service_max_us;
    /* Wait from arrival to the start of processing, and why it was long
     * (> 2 ms): the control task was idle (scheduling) or still busy with
     * an iteration. polls_max_us: the library's own polls between ours. */
    uint32_t wake_max_us, slow_idle, slow_busy, polls_max_us;
    uint32_t wake_fix; /* 1 if the library's pending flag was found */
    uint32_t bucket[BUCKETS];
    uint32_t iterations; /* control iterations, for the idle rate */
} lat;
static uint32_t iteration_end, midi_poll_end;
static volatile uint8_t *library_pending; /* see "Control wake-ups" */

static void latency_record(uint32_t us)
{
    unsigned b = 0;
    while (b < BUCKETS - 1U && us >= bucket_limit[b]) ++b;
    ++lat.bucket[b];
    ++lat.count;
    lat.sum_us += us;
    if (us > lat.max_us) lat.max_us = us;
}

void mp_note_screen_time(uint32_t start, uint32_t end)
{
    if (end - start > lat.screen_max_us) lat.screen_max_us = end - start;
    if (start - midi_poll_end > lat.polls_max_us) lat.polls_max_us = start - midi_poll_end;
    iteration_end = end;
    ++lat.iterations;
}

/* ---- Output: control task only ----------------------------------------- */
enum { OUT_QUEUE = 256 };
static uint32_t output[OUT_QUEUE], output_stamp[OUT_QUEUE];
static unsigned out_read, out_write;
static struct mp_stats stats;
/* SysEx waiting for output, in output-queue order. An output entry with
 * length 0 is a SysEx of (entry >> 8) bytes at sysex_out_read. */
static uint8_t sysex_out[SYSEX_RING];
static unsigned sysex_out_read, sysex_out_write;

int mp_emit_sysex(const struct mp_event *e)
{
    unsigned n = e->sysex_bytes;
    if (e->length != 0 || !e->sysex_stored) { ++stats.lost_output; return -1; } /* too long */
    if (out_write - out_read >= OUT_QUEUE || SYSEX_RING - (sysex_out_write - sysex_out_read) < n) {
        ++stats.lost_output;
        return -1;
    }
    unsigned r = sysex_in.read;
    for (unsigned i = 0; i < n; ++i)
        sysex_out[(sysex_out_write + i) & (SYSEX_RING - 1)] = sysex_in.data[(r + i) & (SYSEX_RING - 1)];
    sysex_out_write += n;
    output[out_write & (OUT_QUEUE - 1)] = (uint32_t)n << 8;
    output_stamp[out_write & (OUT_QUEUE - 1)] = current_stamp;
    ++out_write;
    return 0;
}

int mp_emit(const uint8_t *m, unsigned n)
{
    if (!m || !n || n > 3) return -1;
    if (out_write - out_read >= OUT_QUEUE) { ++stats.lost_output; return -1; }
    output[out_write & (OUT_QUEUE - 1)] = (uint32_t)m[0] | ((uint32_t)(n > 1 ? m[1] : 0) << 8) |
        ((uint32_t)(n > 2 ? m[2] : 0) << 16) | ((uint32_t)n << 24);
    output_stamp[out_write & (OUT_QUEUE - 1)] = current_stamp;
    ++out_write;
    return 0;
}

/* Returns nonzero if output is still waiting for transport space. */
static int output_flush(void)
{
    /* Nothing listening on USB: discard instead of backing up the pipeline. */
    if (!(fm1_midi_ready_ports() & (1U << FM1_MIDI_USB))) {
        out_read = out_write;
        sysex_out_read = sysex_out_write;
        return 0;
    }
    while (out_read != out_write) {
        uint32_t v = output[out_read & (OUT_QUEUE - 1)];
        unsigned length = (v >> 24) & 3U;
        int r;
        if (length) {
            uint8_t m[3] = {(uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16)};
            r = fm1_midi_send_port(FM1_MIDI_USB, m, length);
        } else { /* SysEx: copy out of the ring so it is contiguous */
            uint8_t m[MP_SYSEX_FORWARD_MAX];
            unsigned n = (v >> 8) & 0xffffU;
            for (unsigned i = 0; i < n; ++i) m[i] = sysex_out[(sysex_out_read + i) & (SYSEX_RING - 1)];
            r = fm1_midi_send_port(FM1_MIDI_USB, m, n);
            if (r != -2) sysex_out_read += n;
        }
        if (r == -2) { ++lat.retries; return 1; } /* transport ring full */
        if (r == 0) {
            ++stats.sent;
            uint32_t stamp = output_stamp[out_read & (OUT_QUEUE - 1)];
            if (stamp) latency_record(mp_time_us() - stamp); /* 0: generated locally */
        } else {
            ++stats.lost_output;
        }
        ++out_read;
    }
    return 0;
}

unsigned mp_event_sysex(const struct mp_event *e, uint8_t out[MP_SYSEX_FORWARD_MAX])
{
    if (e->length != 0 || !e->sysex_stored || e->sysex_bytes > MP_SYSEX_FORWARD_MAX) return 0;
    unsigned r = sysex_in.read;
    for (unsigned i = 0; i < e->sysex_bytes; ++i) out[i] = sysex_in.data[(r + i) & (SYSEX_RING - 1)];
    return e->sysex_bytes;
}

int mp_output_idle(void)
{
    return out_read == out_write;
}

int mp_send_sysex_local(const uint8_t *m, unsigned n)
{
    if (!m || n < 2 || n > MP_SYSEX_FORWARD_MAX) return -1;
    if (out_write - out_read >= OUT_QUEUE || SYSEX_RING - (sysex_out_write - sysex_out_read) < n) return -1;
    for (unsigned i = 0; i < n; ++i) sysex_out[(sysex_out_write + i) & (SYSEX_RING - 1)] = m[i];
    sysex_out_write += n;
    output[out_write & (OUT_QUEUE - 1)] = (uint32_t)n << 8;
    output_stamp[out_write & (OUT_QUEUE - 1)] = 0; /* local: not in the latency figures */
    ++out_write;
    (void)output_flush();
    return 0;
}

void mp_send_local(const uint8_t *m, unsigned n)
{
    uint32_t saved = current_stamp;
    current_stamp = 0; /* not an input: keep it out of the latency figures */
    (void)mp_emit(m, n);
    current_stamp = saved;
    (void)output_flush();
}

/* SysEx reply: F0 7D 4D 50 01, then each value as five 7-bit bytes (least
 * significant first) in this order: count, sum_us, max_us, retries,
 * screen_max_us, service_max_us, wake_max_us, slow_idle, slow_busy,
 * polls_max_us, wake_fix, lost_input, lost_output, bucket[0..7],
 * iterations. F7. */
enum { DIAG_VALUES = 14 + BUCKETS };
static void diag_reply(void)
{
    uint32_t values[DIAG_VALUES] = {lat.count, lat.sum_us, lat.max_us, lat.retries,
                                    lat.screen_max_us, lat.service_max_us, lat.wake_max_us,
                                    lat.slow_idle, lat.slow_busy, lat.polls_max_us,
                                    library_pending != 0, input.lost, stats.lost_output};
    memcpy(values + 13, lat.bucket, sizeof lat.bucket);
    values[13 + BUCKETS] = lat.iterations;
    uint8_t m[6 + 5 * DIAG_VALUES] = {0xf0, 0x7d, 0x4d, 0x50, 0x01};
    unsigned n = 5;
    for (unsigned i = 0; i < DIAG_VALUES; ++i)
        for (unsigned k = 0; k < 5U; ++k) m[n++] = (uint8_t)((values[i] >> (7U * k)) & 0x7fU);
    m[n++] = 0xf7;
    (void)fm1_midi_send_port(FM1_MIDI_USB, m, n);
}

void mp_stats_read(struct mp_stats *s)
{
    *s = stats;
    s->lost_input = input.lost;
    s->sysex = mp_sysex_count();
}

/* ---- Control wake-ups ------------------------------------------------------
 * The library runs a control iteration (groove_midi_poll, its own polls,
 * groove_transfer_poll, groove_screen_control) and only then clears its
 * private wake-up-pending flag:
 *
 *   fm1_application_message: call iteration; control_pending = 0
 *
 * A fm1_control_request() made during the iteration sees the flag still set,
 * posts nothing, and its message waits for the next 10 ms tick. Measured
 * before the fix: 10% of messages took 5-12 ms instead of under 1 ms.
 *
 * Fix: the last app hook clears the flag itself, then checks the input queue
 * one final time. Anything arriving after that posts a real wake-up (at
 * worst one spare iteration). The flag's address is read from
 * fm1_control_request's first load instruction, "c4 ff <address LE32>"
 * (r4 = address), checked against the surrounding bytes of the fixed
 * library code. If the code doesn't match, the fix stays off and MIDI is
 * still serviced from every hook. library_pending is declared above. */

static volatile uint8_t *find_library_pending(void)
{
    static const uint8_t expect_head[] = {0x75, 0x04, 0xc4, 0xff}; /* push; r4 = imm32 */
    static const uint8_t expect_tail[] = {0x48, 0x40, 0x80, 0x4f}; /* r0 = b[r4]; if r0 != 0 */
    const uint8_t *code = (const uint8_t *)(uintptr_t)&fm1_control_request;
    if (memcmp(code, expect_head, sizeof expect_head) || memcmp(code + 8, expect_tail, sizeof expect_tail))
        return 0;
    uint32_t address = (uint32_t)code[4] | (uint32_t)code[5] << 8 | (uint32_t)code[6] << 16 |
                       (uint32_t)code[7] << 24;
    if (address < 0x1c00000U || address >= 0x1d00000U) return 0; /* internal RAM only */
    return (volatile uint8_t *)(uintptr_t)address;
}

void mp_end_of_iteration(void)
{
    if (library_pending) {
        *library_pending = 0;
        barrier();
    }
    mp_service();
}

/* ---- SDK hooks (called by the closed engine library) -------------------- */
void groove_midi_init(void)
{
    library_pending = find_library_pending();
    memset(&input, 0, sizeof input);
    sysex_in.read = sysex_in.write = 0;
    out_read = out_write = 0;
    sysex_out_read = sysex_out_write = 0;
    memset(&stats, 0, sizeof stats);
    mon_init();
    mp_process_init();
}

/* Process and send up to one batch of input, then log it to the monitor.
 * Returns the number of messages taken. */
static unsigned run_batch(void)
{
    uint32_t start = mp_time_us();
    enum { BATCH = 64 };
    struct mp_event batch[BATCH];
    unsigned n = 0;
    while (n < BATCH && OUT_QUEUE - (out_write - out_read) >= mp_emit_reserve() && input_pop(&batch[n])) {
        ++stats.received;
        if (n == 0) {
            uint32_t wait = start - current_stamp;
            if ((int32_t)wait > 0 && wait > lat.wake_max_us) lat.wake_max_us = wait;
            if ((int32_t)wait > 2000) {
                if ((int32_t)(current_stamp - iteration_end) > 0) ++lat.slow_idle;
                else ++lat.slow_busy;
            }
        }
        mp_process(&batch[n]);
        sysex_consume(&batch[n]);
        (void)output_flush();
        ++n;
    }
    (void)output_flush();
    uint32_t busy = mp_time_us() - start;
    if (busy > lat.service_max_us) lat.service_max_us = busy;

    for (unsigned i = 0; i < n; ++i) {
        if (batch[i].length == 0) mon_log_sysex(batch[i].source, batch[i].sysex_bytes, batch[i].fate);
        else if (batch[i].data[0] == 0xf8) { /* clock: indicator only, not listed */
            if (batch[i].fate != MP_BLOCKED) mon_clock(); /* the owner's ticks only */
        }
        else mon_log(&batch[i]);
    }
    return n;
}

/* Latency first: each message is processed and handed to the USB transport
 * before anything else. Monitor logging and diagnostics come
 * after, and the input queue is checked once more at the end so a message
 * that arrived meanwhile is not left for the next control iteration. */
void mp_service(void)
{
    unsigned rounds = 0; /* bounded: at most 8 batches of 64 per call */
    mp_clock_poll(); /* audio time base (frequent samples help) and clock timeout */
    while (rounds < 8U && run_batch()) ++rounds;

    switch (mp_diag_take()) {
    case 1: diag_reply(); break;
    case 2: memset(&lat, 0, sizeof lat); break;
    default: break;
    }
    while (rounds < 8U && run_batch()) ++rounds;

    /* Still more input (the bound was reached): ask for another iteration.
     * Output blocked on a full transport ring (256 bytes, 85+ messages) is
     * not re-requested: the ring is drained by the connectivity task, which
     * a spinning control task could starve. It is retried on the next
     * iteration, and counted in lat.retries. */
    if (input.read != input.write) fm1_control_request();
}

/* Control task, first hook of every control iteration. mp_service() is also
 * called from groove_transfer_poll and groove_screen_control, and the
 * iteration ends with mp_end_of_iteration(): see "Control wake-ups". */
void groove_midi_poll(void)
{
    mp_service();
    midi_poll_end = mp_time_us();
}

/* The library's synth/sequencer reports into these. The processor has no
 * internal instrument, so they are inert. */
void groove_midi_clock_reset(void) {}
void groove_midi_panic(void) {}
FM1_DSP_CODE unsigned groove_midi_mode(unsigned track) { (void)track; return 0; }
FM1_DSP_CODE void groove_midi_note(unsigned track, unsigned note, unsigned velocity)
{ (void)track; (void)note; (void)velocity; }
FM1_DSP_CODE void groove_midi_track_reset(unsigned track) { (void)track; }
FM1_DSP_CODE void groove_midi_transport(unsigned owner, uint8_t status) { (void)owner; (void)status; }
FM1_DSP_CODE void groove_midi_clock(unsigned owner, unsigned tempo) { (void)owner; (void)tempo; }
