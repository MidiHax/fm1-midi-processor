/* Host stub of the declarations clock.c uses. Keep in step with src/app/midiproc.h. */
#pragma once
#include <stdint.h>
#include <stddef.h>
#define FM1_SAMPLE_RATE 44100
#define MP_PORTS 3U
struct mp_event {
    uint8_t source, length;
    uint8_t data[3];
    uint8_t sysex_stored;
    uint8_t fate;
    uint16_t sysex_bytes;
    uint32_t stamp;
};
uint32_t fm1_millis(void);
uint32_t mp_time_us(void);
#define MP_CLOCK_NONE 0xffU
void mp_clock_init(void);
int mp_is_clock_message(uint8_t status);
int mp_clock_accept(const struct mp_event *in);
void mp_clock_poll(void);
struct mp_clock_status { uint8_t owner; uint8_t running; uint32_t elapsed_ms; };
void mp_clock_status(struct mp_clock_status *out);
void mp_clock_bpm_enable(int on);
int mp_clock_bpm10(void);
enum { MP_TIME_BASES = 4 };
#define MP_CLICK_HZ_MIN 200U
#define MP_CLICK_HZ_MAX 4000U
struct mp_metronome { uint8_t enabled, time_base, beats; uint16_t click_hz, accent_hz; };
extern struct mp_metronome mp_metronome;
void mp_metronome_defaults(void);
int mp_metronome_position(unsigned *bar, unsigned *beat);
extern volatile uint32_t mp_audio_frames;
void mp_click_post(uint32_t frame, unsigned hz);
