/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * Output mix (audio ISR): USB and Bluetooth audio pass through to the
 * headphone/line output at the host's volume, and the metronome click is
 * added at the MASTER pot's level (design/clock-and-metronome.md Q9). The
 * click is added after the copy sent to USB recording, so it never reaches
 * the computer (Q10). The library's synth output is discarded.
 *
 * Everything the ISR touches lives in RAM: this file's code is placed with
 * FM1_AUDIO_CODE, and the sine table is filled at init rather than kept in
 * Flash, which is unreadable while a patch is being written. */
#include "audio_output.h"
#include "midiproc.h"
#include "fm1_engine_control.h"
#include "fm1_audio_io.h"

static unsigned output_gain; /* MASTER, smoothed: 0..127 */
static int32_t usb_pcm[FM1_BLOCK_WORDS] __attribute__((aligned(32)));
static int16_t bluetooth_pcm[FM1_BLOCK_WORDS] __attribute__((aligned(32)));

/* ---- Click ------------------------------------------------------------------
 * A sine at the requested frequency, 1 ms attack, then an exponential decay
 * to -60 dB in about 25 ms. Requests come from the control task through a
 * small ring: the frame to start on (in mp_audio_frames counts) and the
 * phase increment. */
enum {
    CLICK_RING = 8,
    CLICK_SAMPLES = 1150,  /* ~26 ms */
    ATTACK_SHIFT = 5,      /* 32-sample attack */
    DECAY_Q15 = 32563,     /* exp(ln(0.001) / 1100) in Q15 */
    LATE_LIMIT = 4096      /* a request this many frames late is dropped */
};
volatile uint32_t mp_audio_frames;
static int16_t sine[256];
static struct {
    uint32_t frame, increment;
} click_ring[CLICK_RING];
static volatile unsigned click_write, click_read;
static uint32_t click_phase, click_increment;
static int32_t click_env;   /* Q15 */
static unsigned click_pos = CLICK_SAMPLES;

static inline __attribute__((always_inline)) void barrier(void) /* inlined: the ISR runs from RAM */
{
    __asm__ volatile("csync" ::: "memory");
}

void mp_click_init(void)
{
    /* sin by rotation: no libm, and done once on the control task. */
    double c = 0.99969881869620425, s = 0.024541228522912288; /* cos, sin(2 pi / 256) */
    double x = 1.0, y = 0.0;
    for (unsigned i = 0; i < 256U; ++i) {
        sine[i] = (int16_t)(y * 32767.0);
        double nx = x * c - y * s;
        y = x * s + y * c;
        x = nx;
    }
    click_read = click_write = 0;
}

/* Control task. A full ring drops the request. */
void mp_click_post(uint32_t frame, unsigned hz)
{
    unsigned w = click_write;
    if (w - click_read >= CLICK_RING) return;
    click_ring[w % CLICK_RING].frame = frame;
    click_ring[w % CLICK_RING].increment = (uint32_t)(((uint64_t)hz << 32) / FM1_SAMPLE_RATE);
    barrier();
    click_write = w + 1U;
}

static inline __attribute__((always_inline)) void add_clicks(int32_t *output, uint32_t first, unsigned level)
{
    int32_t gain = (int32_t)((level * level) >> 7); /* 0..126, a rough loudness curve */
    for (unsigned i = 0; i < FM1_BLOCK_FRAMES; ++i) {
        unsigned r = click_read;
        if (r != click_write) {
            barrier();
            int32_t due = (int32_t)(click_ring[r % CLICK_RING].frame - (first + i));
            if (due <= 0) {
                if (due > -LATE_LIMIT) {
                    click_increment = click_ring[r % CLICK_RING].increment;
                    click_phase = 0;
                    click_env = 32767;
                    click_pos = 0;
                }
                click_read = r + 1U;
            }
        }
        if (click_pos >= CLICK_SAMPLES) continue;
        int32_t s = sine[click_phase >> 24] * click_env >> 15;
        if (click_pos < (1U << ATTACK_SHIFT)) s = s * (int32_t)click_pos >> ATTACK_SHIFT;
        click_phase += click_increment;
        click_env = click_env * DECAY_Q15 >> 15;
        ++click_pos;
        int32_t v = s * gain; /* peak about half of full scale */
        for (unsigned ch = 0; ch < FM1_CHANNELS; ++ch) {
            int32_t o = output[i * FM1_CHANNELS + ch] + v;
            output[i * FM1_CHANNELS + ch] = o > 8388607 ? 8388607 : o < -8388608 ? -8388608 : o;
        }
    }
}

/* ---- Mix -------------------------------------------------------------------- */
void fm1_output_reset(void) { output_gain = 0; }
static inline __attribute__((always_inline)) int32_t limit(int64_t value)
{
    /* Bias signed 24-bit PCM into [0, 2^24). Unsigned arithmetic also
     * defines the check at the int64 limits, without two wide compares. */
    if (((uint64_t)value + 8388608U) >> 24)
        return value < 0 ? -8388608 : 8388607;
    return (int32_t)value;
}
FM1_AUDIO_CODE void fm1_output_mix(int32_t *output)
{
    if (!output) return;
    uint32_t first = mp_audio_frames;
    mp_audio_frames = first + FM1_BLOCK_FRAMES;
    unsigned target = fm1_engine_output_level();
    if (target > 127U) target = 127U;
    int delta = (int)target - (int)output_gain;
    output_gain = (unsigned)((int)output_gain + (delta > 8 ? 8 : delta < -8 ? -8 : delta));

    /* Pass-through at the host's playback volume (128 = unity). */
    uint16_t usb_gain[2] = {0, 0};
    int usb_frames = fm1_usb_audio_read(usb_pcm, FM1_BLOCK_FRAMES, usb_gain);
    unsigned left = usb_gain[0], right = usb_gain[1];
    /* Accumulate USB before clipping, then add Bluetooth.
     * Discard the block if the USB stream resets. */
    for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) {
        int64_t value = 0;
        if (usb_frames == FM1_BLOCK_FRAMES)
            value += ((int64_t)usb_pcm[i] * ((i & 1U) ? right : left)) >> 7;
        output[i] = usb_frames == -2 ? 0 : limit(value);
    }
    if (fm1_bluetooth_audio_read(bluetooth_pcm, FM1_BLOCK_FRAMES) == FM1_BLOCK_FRAMES) {
        /* output is already clipped to 24 bits. Adding signed 16-bit PCM
         * times 256 (16 to 24 bits) fits in 25 bits, so this needs no int64. */
        for (unsigned i = 0; i < FM1_BLOCK_WORDS; ++i) {
            int32_t value = output[i] + (int32_t)bluetooth_pcm[i] * 256;
            output[i] = value > 8388607 ? 8388607 : value < -8388608 ? -8388608 : value;
        }
    }
    (void)fm1_usb_audio_write(output, FM1_BLOCK_FRAMES); /* recording: no click */
    add_clicks(output, first, output_gain);
}
