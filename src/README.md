# FM-1 MIDI Processor firmware

Custom firmware that turns the M-VAVE FM-1 into a MIDI processor. It's based on the official [FM1 Open V14](https://github.com/m-vave/fm1_open) source and SDK, with the synth, sequencer, storage and web simulator removed from the app. The feature spec is in `../Spec.txt`.

## Current state

TRS MIDI IN and USB MIDI IN pass through to USB MIDI OUT. The screen shows message counters, the last input message and the USB MIDI connection state. The synth output is muted.

## Layout

| Path | Purpose |
|---|---|
| `app/processor.c` | The MIDI pipeline. Features go here. |
| `app/midi_router.c` | Input queue from the routing task to the control task, plus the USB output queue. Build.ps1 compiles it without LTO by file name. |
| `app/transfer.c` | SysEx input. Build.ps1 compiles it without LTO by file name. |
| `app/screen.c` | 20x15 text grid on the 240x240 LCD, plus the UI entry points. Build.ps1 compiles it without LTO by file name. |
| `app/sdk_hooks.c` | Inert hooks that the engine library requires: project, samples and the DX7 library. |
| `app/audio_output.c` | Output mix. Synth audio is discarded; USB and Bluetooth audio pass through. |
| `app/board/` | Board drivers for the LCD, panel scanner and I2S audio. Unchanged from upstream. |
| `app/synth/engine.h` | Required by Build.ps1's ABI check. Keep it. |
| `sdk/` | M-VAVE binary SDK. **Do not edit.** Build.ps1 checks every file against `sdk/manifest.json`. |

### Constraints from the SDK

- `sdk/lib/libfm1_engine.a` is linked whole. It still contains the DX7 engine and sequencer, and Build.ps1 checks that they are present. This firmware never sends them MIDI and mutes their output.
- The library calls these app hooks: `groove_screen_*`, `groove_midi_*`, `groove_project_*`, `groove_samples_*`, `groove_transfer_*`, `groove_fm_*` and `fm1_audio_*`. They must all exist.
- `fm1_app_midi_received` runs on the routing task. `fm1_midi_send_port`, the screen, Flash and the controls are control-task only.
- Firmware-update SysEx is handled by the SDK before `fm1_app_sysex`, so this firmware can't block re-flashing.
- Don't write to the user Flash partition until a storage format is designed. It currently holds the stock projects, samples and voices.

## Build

Requires Windows x64, PowerShell and **JieLi Windows toolchain 2.5.3**, which provides PI32v2 clang 4.0.1. Download the [2.5.3 installer](https://jl-update.oss-cn-shenzhen.aliyuncs.com/2.5.3.exe) and follow the [installation guide](https://doc.zh-jieli.com/Tools/zh-cn/dev_tools/dev_env/jl_toolchain.html). Then:

~~~powershell
$env:FM1_PI32_TOOLCHAIN = 'C:\JL\pi32'
.\build.bat
~~~

The build produces two outputs:

- **`build\dx7\generated\FM1_OFFICIAL_OPEN.fwsc`**: the firmware. Build.ps1 also checks the linked image (`build\dx7\firmware-report.json`).
- **`build\updater\FM1-MidiProc-Updater.html`**: a local flashing page made by `tools\Make-Updater.ps1`. It's the SDK's own updater with this firmware embedded, plus the stock V15 image for rollback.

## Flash

**Quick path (once the device runs this firmware):** from the project root, run `python tools\fm1_flash.py --build`. It builds, then flashes over USB MIDI with the same protocol and checks as the web updater. Run `python tools\fm1_flash.py --info` to see the installed build, or `--help` for all options. It needs `python-rtmidi`, and no other program may hold the FM-1's MIDI port. If an update is interrupted, run the script again: it resumes from the loader.

**First install, or from stock firmware:** the online M-VAVE workbench can't flash external firmware files, so use the local updater page.

1. Back up any projects, voices and samples you want to keep, using the [M-VAVE workbench](https://open.m-vave.com/fm1-workbench).
2. Close any other app that uses the FM-1's MIDI ports: the DAW, MIDI monitors, and other workbench tabs.
3. Open `build\updater\FM1-MidiProc-Updater.html` in Chrome or Edge. Pick **English** at the top right, and allow MIDI/SysEx access when the browser asks.
4. Stay on the **Device update** tab (the page opens on it). On any other tab, **Connect device** tries the Open project-library protocol, which stock firmware doesn't answer. Connect the device and tick the backup confirmation, then start the update. If the page asks for **Force flash**, tick it and continue. This usually happens when the device runs stock V15 or another firmware.
5. Keep USB connected until the page reports success.

**Rollback:** use **Restore original V15** on the same page.

## MIDI latency

MIDI latency comes first. Measure it after any change to the MIDI path or anything that runs on the control task:

    python tools\latency_test.py                 single notes over USB, round trip
    python tools\latency_test.py --burst 8       8 notes at once

The script prints the host round trip, then the FM-1's own figures: time from arrival to the USB transport taking the output, in microseconds. It reads these with a diagnostic SysEx query (`F0 7D 4D 50 01 F7`, reset with `02`; see `app/midi_router.c`). Reference, 2026-11-01, monitor screen on: single notes mean 124 µs, max 433 µs; 8-note bursts mean 160 µs, max 824 µs.

Rules that keep it there:

- Messages are processed and handed to USB before any monitor or UI work (`mp_service`).
- The engine library drops wake-up requests made during a control iteration, so a message could wait for the next 10 ms tick. `mp_end_of_iteration()` must stay the last call of `groove_screen_control` (see "Control wake-ups" in `midi_router.c`). The latency test prints whether this fix is active.
- Any control-task work that can take more than about 200 µs calls `mp_service()` part-way through, as the screen's row drawing does.

## License

APP: [GPLv3 with the FM1 SDK linking exception](LICENSE.md). The SDK is a binary with separate terms. See [THIRD_PARTY.md](THIRD_PARTY.md).
