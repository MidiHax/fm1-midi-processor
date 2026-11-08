# Web patch editor

A page that edits the MIDI Processor's patches from a computer: filters, remaps, triggers, knobs and Live Keys, over USB, with no software to install.

**Built** (`html/editor.html`), except the status command in build step 5. The codec is tested against the firmware's own code (`tools/host_tests/codec`). Writing to a real FM-1 from the page is not yet tested. Items marked **Q** were questions; all five are decided and are listed at the end. The format it edits is in `patch-format.md`.

---

## Goals and limits

The FM-1's 240x240 screen and two encoders are fine for a change on stage, and slow for anything bigger. Filling a 32-row drum map or a trigger with 32 messages is the job this page is for.

| In | Out |
|---|---|
| The 16 stored patches and the edit buffer | Global settings (Device ID, Allow Dump Request, metronome). They are never sent in a dump. |
| Every field in a patch body | Flashing firmware. That stays on `flash.html`. |
| Reading, trying out and writing patches over USB | A MIDI monitor |
| `.syx` files, compatible with `tools/patch_sysex.py` | Accounts, servers, cloud storage |
| Undo and redo, which the FM-1 doesn't have | Learning values from played MIDI (see Limits) |

---

## Where it runs

- One static page, `html/editor.html`, on the same GitHub Pages site as the help guide and flash page. There is no build step and no server. Patches stay in the browser and on the device.
- Web MIDI with SysEx permission (`requestMIDIAccess({sysex: true})`), so desktop Chrome or Edge, as on `flash.html`. Without it the page still opens and edits `.syx` files, with the device controls disabled.
- Styling is `crt.css` from `css-crt-vibe`, copied to `html/crt.css` so the site doesn't depend on another repo. The Doto font comes from Google Fonts and falls back to monospace.

| File | Holds |
|---|---|
| `html/editor.html` | Layout and page script |
| `html/patch-codec.js` | Body encode and decode, validation, 7-bit packing, packets. No DOM, so it can be tested on its own. |
| `html/crt.css` | Copy of the stylesheet |

---

## What the firmware gives us

Everything needed for a working editor exists today. The status command below is a later addition that closes its two worst limits.

| Need | How |
|---|---|
| Read one patch | Request `10` with slot 0-15, or `7F` for the edit buffer |
| Read all | Request `11` |
| Try a patch without touching Flash | Dump `00` to `7F`. Replaces the edit buffer; the FM-1 shows `*`. |
| Store a patch | Dump `00` to slot 0-15. Written to Flash 100 ms after the last packet. |
| Load a stored patch into use | Read the slot, then dump it to `7F`. It shows as unsaved on the FM-1. |

### Connecting

1. Ask for MIDI access. List port pairs and pick the one named `USB-Midi`. If there are several, or none, show a dropdown.
2. Wait 50 ms after opening the ports. On Windows, a reply to a message sent sooner can be lost (see `tools/patch_sysex.py`).
3. Request `7F` with the remembered Device ID (default 0). A reply confirms the device, and the edit buffer becomes the first document.
4. Request all 16 patches to fill the bank.
5. No reply within 1.5 s: show "No reply. Check ALLOW DUMP REQ and DEVICE ID on the Sysex Dump page." Allow Dump Request defaults to YES.

The page can't read the Device ID. **Find device** tries all 128 values, but only on request: the 127 wrong ones aren't recognised, so they go through the pipeline like any other SysEx and appear on the monitor and on USB OUT when SYX passes. The reader must ignore those echoed requests.

### Sending

- Send a patch's packets in order, 5 ms apart (the same gap as `patch_sysex.py`), and never start a second dump before the first has gone out. A new packet 0 makes the FM-1 drop an unfinished patch.
- Replies are paced by the FM-1, which waits for its output to be idle. Busy MIDI slows them down, so timeouts run from the last packet received and not from the request.
- A request made during another reply gets no answer. Retry once after a timeout.
- Only USB is usable: the firmware ignores SysEx arriving over BLE.

### Limits

| Limit | Effect | Handling |
|---|---|---|
| A dump gets no acknowledgement | The page can't tell that a patch arrived | Read it back and compare bytes (below) |
| Flash errors aren't reported over SysEx | They show only on the Sysex Dump page, row 13 | Read back late enough to read Flash. See **Q2**. |
| The slot in use can't be read | A dump to that slot reloads the edit buffer and **silently discards unsaved panel edits** | Warn on every slot write. See **Q2**. |
| The FM-1 doesn't announce panel changes | The page's copy can go stale | Re-read the target just before writing, and stop if it changed since it was read |
| An empty slot reads as a default patch | Empty and untouched look the same | Show them the same, as the FM-1 does |
| Web MIDI hears only USB OUT | Played MIDI arrives after the pipeline, with no input port | No learn mode in this version |

### Read-back

After a write, request the patch again and compare it byte for byte with what was sent. The encoder writes canonical order, so the bytes match exactly.

Slots are stored in Flash after 100 ms of quiet, one per control iteration, and a request before that is answered from RAM. So wait 100 ms plus 100 ms per slot written, then read. This timing is an estimate: a read that comes too early passes even if Flash later fails. The status command replaces the estimate once the firmware has it.

### Status command (firmware addition, Q2)

A new request, answered only when Allow Dump Request is YES, like the other requests:

| Direction | Command | Data |
|---|---|---|
| Request | `12` | None |
| Reply | `02` | The table below |

| Byte | Field | Values |
|---|---|---|
| 0 | Status format version | `1` |
| 1 | Slot in use | 0-15 |
| 2 | Unsaved changes | 0 or 1 (`mp_patch_dirty`) |
| 3-5 | Firmware version: major, minor, patch | 0-127 each |
| 6-8 | Slots waiting for Flash | 16-bit mask, 7 bits per byte, low byte first |
| 9 | Last store result | 0 = OK, 1 = failed |

What the editor does with it:
- **Writes to the slot in use:** warns only when that slot really has unsaved panel edits, and names them as such.
- **Read-back:** polls until no slots are waiting, then checks the store result and compares bytes. This replaces the timing estimate above.
- **Bank:** marks the slot in use.

An older firmware doesn't answer. The editor asks once on connecting, and after a timeout keeps the behaviour and warnings described above. The reply goes out through the same idle-output check as a dump, so it can't delay live MIDI.

---

## Model

The page holds **documents**, one per source: the edit buffer, each of the 16 slots, and any opened file. Each document has its current state, the state last read from the device, and its own undo history. Switching between them loses nothing.

There are three distinct actions, labelled for what they do:

| Button | Sends | Flash | Confirmation |
|---|---|---|---|
| **READ** | Request. Replaces the draft with the device's copy. | No | Asks first if there are edits |
| **TRY** | Dump to `7F` | No | None |
| **WRITE TO SLOT** | Dump to the chosen slot, then read-back | Yes | Two presses, as with REC on the FM-1: the button flashes for 5 s |

**LIVE** sends a TRY automatically, 150 ms after the last edit, with at most one dump in flight. It is **off by default** and resets to off on each visit (**Q1**).

Side effects of every TRY, on top of replacing the edit buffer:
- It resets each trigger's ENTER memory.
- A sub-page open on the FM-1 returns to its list.
- If a remap changes while a note is held, its Note Off follows the new rule and the note can hang.

**Bank writes** send only the slots whose bytes differ, each as a single dump (`00`).

**Files:** `.syx`, made of the same packets as a device dump, so `patch_sysex.py` can read them. On import, the Device ID is replaced by the connected device's. A draft is kept in browser storage, for recovering from a closed tab.

---

## Layout

Three columns, stacking on narrow screens.

| Area | Contents |
|---|---|
| Header band | Title, port, Device ID, connection LED |
| Left: **Bank** | Edit buffer, then slots 1-16: number, name, `*` when edited |
| Centre, top | Patch name (12 characters; typing is uppercased and limited to the allowed set), READ, UNDO, REDO, TRY, LIVE, WRITE TO SLOT |
| Centre: **signal path** | The sections as a strip in pipeline order, `IN → CHANNEL FILTER → MESSAGE FILTER → TRIGGERS → MESSAGE REMAP → CHANNEL REMAP → OUT`, plus KNOBS + KEYS on its own. Each shows a summary, and an LED if edited. |
| Centre: section | The editor for the selected section |
| Right | Capacity meters, problems, transfer log with the bytes sent |

The strip is the navigation because it teaches the order things happen in, which is what decides whether a rule works: a blocked message never reaches a trigger, and triggers match values before any remap.

| Section | Editor |
|---|---|
| **Channel Filter** | One row per port, 16 channel cells. Row buttons for ALL and NONE. |
| **Message Filter** | One port at a time, shared with Channel Remap as on the FM-1. System types as a row of cells; channel types as a grid of 7 types by 16 channels, with row and column headers toggling a whole line. |
| **Triggers** | List in firing order, with up and down. Header fields, then the message table with add, duplicate and delete. `IN` is typed as a value. D2 shows `---` for PC. |
| **Message Remap** | One flat, filterable table: port, channel, type, input, output, output channel, with note names. |
| **Channel Remap** | A 16x16 grid per port, input down the side and output across. One click per input; a second click removes it. |
| **Knobs + Keys** | Four knobs (channel, CC). Live Keys fields, with program shown 1-128 and bank as a number or `OFF`. |

Every filter cell says what it does, in text: **OK** on a lit cell, **Block** on a dithered one. Clicking a cell switches it. The word, the fill and the colour all change, so the state doesn't depend on colour alone.

---

## Validation

The firmware rejects a whole patch if any field is out of range, and reports nothing back. So the page checks first, and disables TRY and WRITE while an error stands.

| Rule | Limit |
|---|---|
| Name | 12 characters: `A-Z`, `0-9`, `-`, space |
| Channel rules | 32; one per port and input channel |
| Message rules | 256 in all; 32 per port, channel and type; one per port, channel, type and input |
| Triggers | 32; 32 messages each, 256 in all |
| Trigger ranges | From ≤ to. Raising FROM pushes TO up and lowering TO pushes FROM down, as on the FM-1. |
| Trigger messages | PC rows have D2 of 0 |
| Knob CC | 0-119 |
| Live Keys velocity | 1-127, min ≤ max; octave -3 to +3 |

**Duplicates need their own check.** The firmware's validator accepts them, then keeps only the last one on load, so a duplicate would silently vanish. The page marks both rows as an error.

Warnings, which don't block a write:
- `NON WITHOUT NOF`, as on the FM-1.
- Overlapping triggers that could exceed the 64-message cap for one input.

**Canonical order:** channel rules sorted by port and input; message rules by port, channel, type and input; triggers in list order. **Versions:** version 1 bodies open normally and are written as version 2. A body with a higher version opens read-only, because writing it back would drop fields this page doesn't know.

---

## Styling

`crt.css` unchanged, set to amber with `--crt-h: 35` on the page (**Q4**). Only layout and the filter and routing grids need extra CSS, written with its colour variables.

| Part | Class |
|---|---|
| Sections, bank, side panels | `crt-panel`, `crt-titlebar` |
| Page bands | `crt-header`, `crt-footer` |
| Tables | `crt-table`, with `aria-selected` for the selected row |
| Inputs | `crt-field`, `crt-switch` |
| Actions | `crt-btn`, `crt-btn-primary` for TRY, `crt-btn-danger` for WRITE |
| Capacity | `crt-meter` |
| Connection, edited, armed | `crt-led` with `amber`, `red`, `blink` |
| Transfer log | `crt-well crt-glow` |
| Blocked cells | `crt-dither` |

Scanlines stay on by default at the stylesheet's own low opacity. All animation stops under reduced motion, which `crt.css` already handles.

---

## Testing

- **Codec, in Node:** round-trip random valid patches; check the limits and each rejection in the table above.
- **Against the firmware:** `tools/host_tests` gains a case that feeds the codec's bytes to the real `body_valid` and `mp_patch_encode`, so the two can't drift apart.
- **Golden files:** a `.syx` saved by `patch_sysex.py` decodes and re-encodes to identical bytes, and the reverse.
- **On the device:** the largest patch (3,070 bytes, 32 packets) round-trips; a write to the slot in use; a request with Allow Dump Request off; busy MIDI during a full read.

---

## Build order

1. Codec and its tests. Nothing else depends on the device being right until this is.
2. Connect and read: every section read-only. Useful on its own for checking a patch.
3. Editing, validation, undo, TRY and files.
4. WRITE TO SLOT with read-back, then bank writes.
5. The status command in the firmware, and the editor's use of it.

---

## Questions

| # | Question | Answer |
|---|---|---|
| Q1 | Should LIVE start on? | **Decided: off.** Turned on, the first edit replaces the edit buffer and discards any unsaved panel work. |
| Q2 | Add a status command to the firmware? | **Decided: yes, as a later step** (see Status command). The editor works without it, with warnings. |
| Q3 | Separate page or part of the help site? | **Decided: separate page,** linked from the help guide and README. It's a tool, and the CRT styling would clash with the site's. |
| Q4 | Which hue? | **Decided: amber,** `--crt-h: 35`. |
| Q5 | Are bank writes needed in the first version? | **Decided: yes.** Reading all 16 is needed anyway, and sending only what changed is small once single writes work. |
