/* SPDX-License-Identifier: GPL-3.0-only WITH GPL-3.0-linking-exception
 * SysEx input. Future home of settings dump/load and patch control.
 *
 * Build.ps1 compiles this file without LTO (by name), which keeps the
 * fm1_app_sysex override from losing to the SDK's weak default, and its
 * firmware check requires a real fm1_app_sysex. Keep the file name.
 *
 * Firmware-update SysEx is handled by the SDK before fm1_app_sysex, so
 * nothing here can block re-flashing. */
#include "midiproc.h"

static volatile uint32_t sysex_count;
static volatile uint8_t diag_command;

unsigned mp_diag_take(void)
{
    unsigned c = diag_command;
    if (c) diag_command = 0;
    return c;
}

/* Routing task (the library delivers SysEx from the same function as short
 * messages). Maximum 1024 bytes. Copy or queue; no Flash, UI, blocking. */
void fm1_app_sysex(const uint8_t *m, size_t n)
{
    if (!m || n < 2 || m[0] != 0xf0 || m[n - 1] != 0xf7) return;
    /* Latency diagnostics query (non-commercial ID 7D, "MP"): not logged,
     * not filtered, not forwarded. Our own control SysEx will bypass the
     * filter in the same way (open question 9). */
    if (n == 6 && m[1] == 0x7d && m[2] == 0x4d && m[3] == 0x50 && (m[4] == 1 || m[4] == 2)) {
        diag_command = m[4];
        fm1_control_request();
        return;
    }
    sysex_count = sysex_count + 1U;
    mp_input_sysex(fm1_midi_input_source(), m, n);
}

uint32_t mp_sysex_count(void) { return sysex_count; }

/* Build.ps1 requires these library symbols in the linked image. The removed
 * synth UI and storage code used to reference them; with --gc-sections they
 * would be discarded. The table keeps them without calling them, and must be
 * referenced from live code (groove_transfer_init) or the linker drops it. */
extern void groove_tuning_capture(void);
extern void groove_tuning_apply(void);
extern void groove_project_apply_edit(void);
__attribute__((used)) static const void *const keep_runtime_symbols[] = {
    (const void *)fm1_ble_set_role,
    (const void *)fm1_ble_scan,
    (const void *)fm1_ble_connect,
    (const void *)fm1_ble_disconnect,
    (const void *)fm1_ble_get_status,
    (const void *)fm1_ble_get_device,
    (const void *)fm1_bluetooth_configure,
    (const void *)fm1_bluetooth_get_status,
    (const void *)groove_tuning_capture,
    (const void *)groove_tuning_apply,
    (const void *)groove_project_apply_edit,
};

/* SDK hooks (called by the engine library). */
void groove_transfer_init(void)
{
    __asm__ volatile("" : : "r"(keep_runtime_symbols) : "memory");
}
/* Between the library's own polls: another chance to move MIDI along. */
void groove_transfer_poll(void) { mp_service(); }
