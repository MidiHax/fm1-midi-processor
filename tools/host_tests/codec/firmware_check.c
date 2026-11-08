/* Runs the web editor codec's test vectors through the real firmware code
 * (src/app/patch.c with remap.c and triggers.c), so the two can't drift apart.
 *
 * Each vector line: <want> <hex body>
 *   want 1 = valid, and decode then encode gives identical bytes
 *   want 2 = valid, but an older format, so the bytes change when re-encoded
 *   want 0 = the firmware must reject it
 *
 * Vectors come from codec_test.js. Run ../run.bat. */
#include "midiproc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Flash is not used by these checks, only the patch body code is. */
int fm1_flash_read(uint32_t a, void *p, size_t n) { (void)a; (void)p; (void)n; return FM1_FLASH_UNAVAILABLE; }
int fm1_flash_erase(uint32_t a) { (void)a; return FM1_FLASH_UNAVAILABLE; }
int fm1_flash_write(uint32_t a, const void *p, size_t n) { (void)a; (void)p; (void)n; return FM1_FLASH_UNAVAILABLE; }
uint32_t fm1_flash_size(void) { return 0; }
uint32_t fm1_millis(void) { return 0; }

void mp_service(void) {}
void mp_send_local(const uint8_t *m, unsigned n) { (void)m; (void)n; }
int mp_emit(const uint8_t *m, unsigned n) { (void)m; (void)n; return 0; }
void mp_metronome_defaults(void) {}
struct mp_metronome mp_metronome;
struct mp_filters mp_filters;
struct mp_knob mp_knobs[MP_KNOBS];
struct mp_live_keys mp_live_keys;

static int hex(int c) { return c <= '9' ? c - '0' : c - 'a' + 10; }

int main(int argc, char **argv)
{
    static char line[16384];
    static uint8_t body[8192], out[MP_PATCH_BODY_MAX];
    FILE *f = argc > 1 ? fopen(argv[1], "r") : NULL;
    if (!f) { puts("no vectors file"); return 1; }
    int total = 0, failures = 0;
    while (fgets(line, sizeof line, f)) {
        int want = line[0] - '0';
        const char *p = line + 2;
        unsigned n = 0;
        while (p[0] != '\n' && p[0] && p[1]) { body[n++] = (uint8_t)(hex(p[0]) << 4 | hex(p[1])); p += 2; }
        ++total;
        int valid = mp_patch_valid(body, n) != 0;
        if (valid != (want != 0)) {
            printf("FAIL: vector %d (%u bytes): firmware says %s\n", total, n, valid ? "valid" : "invalid");
            ++failures;
            continue;
        }
        if (want != 1) continue;
        if (mp_patch_decode(body, n) != 0) { printf("FAIL: vector %d did not decode\n", total); ++failures; continue; }
        unsigned m = mp_patch_encode(out);
        if (m != n || memcmp(out, body, n) != 0) {
            printf("FAIL: vector %d: firmware re-encodes %u bytes differently from the codec's %u\n", total, m, n);
            ++failures;
        }
    }
    fclose(f);
    printf("firmware check: %d/%d passed\n", total - failures, total);
    return failures != 0;
}
